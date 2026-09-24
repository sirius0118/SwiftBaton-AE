/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sb-kernel-catalog.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#define SBK_CTL_MAGIC UINT32_C(0x53424b31)
#define SBK_CTL_VERSION 1
struct control_header {
  uint32_t magic, version, pid, count;
  int32_t error;
  uint32_t reserved;
};
struct control_range {
  uint64_t address, pages;
};
struct catalog_entry {
  struct sbk_catalog_record record;
  int fd, drain_fd, ready_fd;
  unsigned staged, final, claimed, started, drained;
  uint64_t *hot;
  size_t hot_count;
};
struct sbk_catalog {
  pthread_mutex_t stage_lock;
  int session, phase;
  unsigned features;
  struct sbk_config config;
  struct catalog_entry **entries;
  size_t count;
};
static int full_io(int fd, void *buffer, size_t n, int output) {
  size_t done = 0;
  while (done < n) {
    ssize_t ret = output
                      ? send(fd, (char *)buffer + done, n - done, MSG_NOSIGNAL)
                      : recv(fd, (char *)buffer + done, n - done, 0);
    if (ret < 0 && errno == EINTR)
      continue;
    if (ret <= 0)
      return ret < 0 ? -errno : -EPIPE;
    done += ret;
  }
  return 0;
}
static int send_right(int socket, int fd) {
  char marker = 'K', control[CMSG_SPACE(sizeof(int))] = {0};
  struct iovec iov = {&marker, 1};
  struct msghdr msg = {0};
  struct cmsghdr *cm;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  cm = CMSG_FIRSTHDR(&msg);
  cm->cmsg_level = SOL_SOCKET;
  cm->cmsg_type = SCM_RIGHTS;
  cm->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cm), &fd, sizeof(fd));
  ssize_t ret;
  do {
    ret = sendmsg(socket, &msg, MSG_NOSIGNAL);
  } while (ret < 0 && errno == EINTR);
  return ret == 1 ? 0 : (ret < 0 ? -errno : -EIO);
}
static int receive_right(int socket) {
  char marker = 0, control[CMSG_SPACE(sizeof(int) * 4)] = {0};
  struct iovec iov = {&marker, 1};
  struct msghdr msg = {0};
  int fds[4], n = 0, bad = 0;
  struct cmsghdr *cm;
  msg.msg_iov = &iov;
  msg.msg_iovlen = 1;
  msg.msg_control = control;
  msg.msg_controllen = sizeof(control);
  ssize_t ret;
  do {
    ret = recvmsg(socket, &msg, MSG_CMSG_CLOEXEC);
  } while (ret < 0 && errno == EINTR);
  if (ret < 0)
    return -errno;
  for (cm = CMSG_FIRSTHDR(&msg); cm; cm = CMSG_NXTHDR(&msg, cm)) {
    if (cm->cmsg_level != SOL_SOCKET || cm->cmsg_type != SCM_RIGHTS ||
        cm->cmsg_len < CMSG_LEN(sizeof(int))) {
      bad = 1;
      continue;
    }
    size_t bytes = cm->cmsg_len - CMSG_LEN(0), nr = bytes / sizeof(int);
    if (bytes % sizeof(int))
      bad = 1;
    for (size_t i = 0; i < nr; i++) {
      int fd;
      memcpy(&fd, (char *)CMSG_DATA(cm) + i * sizeof(fd), sizeof(fd));
      if (n < 4)
        fds[n++] = fd;
      else {
        close(fd);
        bad = 1;
      }
    }
  }
  if (ret != 1 || marker != 'K' || (msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC)) ||
      bad || n != 1) {
    for (int i = 0; i < n; i++)
      close(fds[i]);
    return -EPROTO;
  }
  return fds[0];
}
static int valid_record(const struct sbk_catalog_record *r) {
  return r->source_pid && r->source_pid <= INT32_MAX &&
         r->restore_pid <= INT32_MAX && !(r->address & 4095) &&
         r->remote.pages && r->remote.pages <= (1ULL << 20) &&
         r->address <= UINT64_MAX - (r->remote.pages << 12) &&
         !(r->remote.address & 4095) &&
         r->remote.address <= UINT64_MAX - (r->remote.pages << 12) &&
         r->remote.id && r->remote.id <= SBK_MAX_REGIONS;
}
/* Prevalidate lists before any final MR switch or descriptor delivery. */
static int valid_indices(const uint64_t *indices, size_t n, uint64_t pages) {
  unsigned char *seen;
  size_t bytes = (pages + 7) / 8;
  if (n > pages || (n && !indices))
    return -EINVAL;
  if (!n)
    return 0;
  seen = calloc(bytes, 1);
  if (!seen)
    return -ENOMEM;
  for (size_t i = 0; i < n; i++) {
    uint64_t k = indices[i];
    if (k >= pages || (seen[k / 8] & (1u << (k % 8)))) {
      free(seen);
      return -EINVAL;
    }
    seen[k / 8] |= 1u << (k % 8);
  }
  free(seen);
  return 0;
}
static int same_source(const struct sbk_catalog_record *a,
                       const struct sbk_catalog_record *b) {
  return a->source_pid == b->source_pid && a->address == b->address &&
         a->remote.pages == b->remote.pages;
}
static struct catalog_entry *find_entry(struct sbk_catalog *c,
                                        const struct sbk_catalog_record *r) {
  for (size_t i = 0; i < c->count; i++)
    if (same_source(&c->entries[i]->record, r))
      return c->entries[i];
  return NULL;
}
static void free_entry(struct catalog_entry *e) {
  if (e->fd >= 0)
    close(e->fd);
  if (e->drain_fd >= 0)
    close(e->drain_fd);
  if (e->ready_fd >= 0)
    close(e->ready_fd);
  free(e->hot);
  free(e);
}
static int new_entry(struct sbk_catalog *c, const struct sbk_catalog_record *r,
                     struct catalog_entry **out) {
  if (c->count == SBK_MAX_REGIONS)
    return -E2BIG;
  struct catalog_entry *e = calloc(1, sizeof(*e));
  if (!e)
    return -ENOMEM;
  e->record = *r;
  e->fd = e->drain_fd = e->ready_fd = -1;
  e->fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
  if (e->fd < 0)
    goto fail;
  struct sbk_region_bind bind = {.session_fd = c->session, .remote = r->remote};
  if (ioctl(e->fd, SBK_IOC_BIND_REGION, &bind))
    goto fail;
  struct sbk_config cfg = c->config;
  cfg.pages = r->remote.pages;
  if (ioctl(e->fd, SBK_IOC_CONFIG, &cfg))
    goto fail;
  e->drain_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (e->drain_fd < 0)
    goto fail;
  if (ioctl(e->fd, SBK_IOC_WATCH_DRAIN, &e->drain_fd))
    goto fail;
  c->entries[c->count++] = e;
  *out = e;
  return 0;
fail:;
  int error = -errno;
  free_entry(e);
  return error;
}
struct sbk_catalog *sbk_catalog_create(int session,
                                       const struct sbk_config *config) {
  if (!config || config->version != SBK_ABI_VERSION ||
      config->backend != SBK_BACKEND_RDMA) {
    errno = EINVAL;
    return NULL;
  }
  struct sbk_catalog *c = calloc(1, sizeof(*c));
  if (!c)
    return NULL;
  c->session = fcntl(session, F_DUPFD_CLOEXEC, 3);
  if (c->session < 0) {
    free(c);
    return NULL;
  }
  c->entries = calloc(SBK_MAX_REGIONS, sizeof(*c->entries));
  if (!c->entries) {
    close(c->session);
    free(c);
    return NULL;
  }
  int init = pthread_mutex_init(&c->stage_lock, NULL);
  if (init) {
    free(c->entries); close(c->session); free(c); errno = init; return NULL;
  }
  c->config = *config;
  struct sbk_capabilities caps;
  int cap_ret = ioctl(c->session, SBK_IOC_CAPABILITIES, &caps);
  if (cap_ret || caps.version != SBK_ABI_VERSION) {
    int error = cap_ret ? errno : EPROTO;
    sbk_catalog_destroy(c);
    errno = error;
    return NULL;
  }
  c->features = caps.features;
  return c;
}
void sbk_catalog_destroy(struct sbk_catalog *c) {
  if (!c)
    return;
  for (size_t i = 0; i < c->count; i++)
    free_entry(c->entries[i]);
  close(c->session);
  free(c->entries);
  pthread_mutex_destroy(&c->stage_lock);
  free(c);
}
int sbk_catalog_stage(struct sbk_catalog *c, const struct sbk_catalog_record *r,
                      const uint64_t *indices, size_t n) {
  if (!c || !r || !valid_record(r) || r->restore_pid ||
      n > r->remote.pages || (n && !indices))
    return -EINVAL;
  int ret = valid_indices(indices, n, r->remote.pages);
  if (ret) return ret;
  pthread_mutex_lock(&c->stage_lock);
  if (c->phase) { ret = -EINVAL; goto unlock; }
  struct catalog_entry *e = find_entry(c, r);
  if (!e) {
    ret = new_entry(c, r, &e);
    if (ret) goto unlock;
  } else if (memcmp(&e->record.remote, &r->remote, sizeof(r->remote))) {
    ret = -ESTALE;
    goto unlock;
  }
  pthread_mutex_unlock(&c->stage_lock);
  /* Entry pointers and fds remain stable until all PS workers join. There is
   * no catalog lock across allocation/RDMA completion in PRETRANSFER. Each
   * region's kernel control mutex handles replay of overlapping requests. */
  if (c->features & SBK_FEATURE_PARALLEL_PS) {
    struct sbk_hot_list list = {.indices = (uintptr_t)indices, .count = n};
    if (ioctl(e->fd, SBK_IOC_PRETRANSFER_MANY, &list)) ret = -errno;
  } else {
    for (size_t i = 0; i < n; i += SBK_MAX_BATCH) {
      struct sbk_hot_list list = {.indices = (uintptr_t)(indices + i), .count = n - i};
      if (list.count > SBK_MAX_BATCH) list.count = SBK_MAX_BATCH;
      if (ioctl(e->fd, SBK_IOC_PRETRANSFER, &list)) { ret = -errno; break; }
    }
  }
  pthread_mutex_lock(&c->stage_lock);
  if (ret) c->phase = -1;
  else e->staged = 1;
unlock:
  pthread_mutex_unlock(&c->stage_lock);
  return ret;
}

/* Final sparse ranges can overlap several earlier PS ranges. Move ownership,
 * then invalidate the final dirty/PFN set before exposing any mapping. */
static int import_ps_slices(struct sbk_catalog *c, struct catalog_entry *e) {
  if (!(c->features & SBK_FEATURE_PS_SLICE))
    return 0; /* Older modules safely refetch unmatched shapes. */
  uint64_t begin = e->record.address, end = begin + e->record.remote.pages * 4096;
  for (size_t i = 0; i < c->count; i++) {
    struct catalog_entry *s = c->entries[i];
    if (s == e || !s->staged || s->final || s->record.source_pid != e->record.source_pid)
      continue;
    uint64_t first = s->record.address > begin ? s->record.address : begin;
    uint64_t last = s->record.address + s->record.remote.pages * 4096;
    if (last > end)
      last = end;
    if (last <= first)
      continue;
    struct sbk_ps_slice slice = {.source_fd = s->fd,
        .source_offset = (first - s->record.address) / 4096,
        .destination_offset = (first - begin) / 4096, .pages = (last - first) / 4096};
    if (ioctl(e->fd, SBK_IOC_IMPORT_PS, &slice))
      return -errno;
  }
  return 0;
}
int sbk_catalog_seal(struct sbk_catalog *c,
                     const struct sbk_catalog_final *final, size_t n) {
  if (!c || c->phase || !final || !n || n > SBK_MAX_REGIONS)
    return -EINVAL;
  for (size_t i = 0; i < n; i++) {
    const struct sbk_catalog_record *r = &final[i].record;
    if (!valid_record(r) || !r->restore_pid ||
        final[i].dirty_count > r->remote.pages ||
        final[i].hot_count > r->remote.pages ||
        (final[i].dirty_count && !final[i].dirty) ||
        (final[i].hot_count && !final[i].hot))
      return -EINVAL;
    int ret =
        valid_indices(final[i].dirty, final[i].dirty_count, r->remote.pages);
    if (ret)
      return ret;
    ret = valid_indices(final[i].hot, final[i].hot_count, r->remote.pages);
    if (ret)
      return ret;
    for (size_t j = 0; j < i; j++) {
      const struct sbk_catalog_record *p = &final[j].record;
      if (same_source(r, p) ||
          (r->restore_pid == p->restore_pid &&
           r->address < p->address + (p->remote.pages << 12) &&
           p->address < r->address + (r->remote.pages << 12)))
        return -EINVAL;
    }
  }
  c->phase = -1; /* Any subsequent failure requires discarding the catalog. */
  for (size_t i = 0; i < n; i++) {
    const struct sbk_catalog_final *f = &final[i];
    struct catalog_entry *e = find_entry(c, &f->record);
    int ret;
    if (!e) {
      ret = new_entry(c, &f->record, &e);
      if (ret)
        return ret;
      ret = import_ps_slices(c, e);
      if (ret)
        return ret;
    }
    if (f->hot_count) {
      e->hot = malloc(f->hot_count * sizeof(*e->hot));
      if (!e->hot)
        return -ENOMEM;
      memcpy(e->hot, f->hot, f->hot_count * sizeof(*e->hot));
      e->hot_count = f->hot_count;
    }
    struct sbk_region_seal seal = {
        .remote = f->record.remote,
        .dirty = {.indices = (uintptr_t)f->dirty, .count = f->dirty_count}};
    if (ioctl(e->fd, SBK_IOC_SEAL_REGION, &seal))
      return -errno;
    e->record = f->record;
    e->final = 1;
  }
  for (size_t i = 0; i < c->count;) {
    if (c->entries[i]->final) {
      i++;
      continue;
    }
    free_entry(c->entries[i]);
    c->entries[i] = c->entries[--c->count];
  }
  c->phase = 1;
  return 0;
}
static int compare_address(const void *a, const void *b) {
  const struct catalog_entry *x = *(struct catalog_entry *const *)a,
                             *y = *(struct catalog_entry *const *)b;
  return (x->record.address > y->record.address) -
         (x->record.address < y->record.address);
}
int sbk_catalog_serve(struct sbk_catalog *c, int socket) {
  struct control_header request,
      reply = {.magic = SBK_CTL_MAGIC, .version = SBK_CTL_VERSION};
  struct catalog_entry **entries = NULL;
  struct control_range *ranges = NULL;
  size_t n = 0;
  int ret, ready = -1;
  if (!c || c->phase != 1)
    return -EINVAL;
  ret = full_io(socket, &request, sizeof(request), 0);
  if (ret)
    return ret;
  if (request.magic != SBK_CTL_MAGIC || request.version != SBK_CTL_VERSION ||
      !request.pid || request.count || request.error || request.reserved)
    return -EPROTO;
  reply.pid = request.pid;
  entries = calloc(c->count, sizeof(*entries));
  if (!entries)
    return -ENOMEM;
  for (size_t i = 0; i < c->count; i++)
    if (c->entries[i]->record.restore_pid == request.pid) {
      if (c->entries[i]->claimed) {
        ret = -EALREADY;
        goto fail;
      }
      entries[n++] = c->entries[i];
    }
  if (!n) {
    ret = -ENOENT;
    goto fail;
  }
  qsort(entries, n, sizeof(*entries), compare_address);
  ranges = calloc(n, sizeof(*ranges));
  if (!ranges) {
    ret = -ENOMEM;
    goto fail;
  }
  for (size_t i = 0; i < n; i++)
    ranges[i] = (struct control_range){entries[i]->record.address,
                                       entries[i]->record.remote.pages};
  ready = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
  if (ready < 0) {
    ret = -errno;
    goto fail;
  }
  reply.count = n;
  ret = full_io(socket, &reply, sizeof(reply), 1);
  if (ret)
    goto fail;
  ret = full_io(socket, ranges, n * sizeof(*ranges), 1);
  if (ret)
    goto fail;
  for (size_t i = 0; i < n; i++) {
    ret = send_right(socket, entries[i]->fd);
    if (ret)
      goto fail;
  }
  ret = send_right(socket, ready);
  if (ret)
    goto fail;
  /* One readiness event per process, owned by its first sorted region. */
  entries[0]->ready_fd = ready;
  ready = -1;
  for (size_t i = 0; i < n; i++)
    entries[i]->claimed = 1;
  free(ranges);
  free(entries);
  return 0;
fail:
  if (ready >= 0)
    close(ready);
  free(ranges);
  free(entries);
  c->phase = -1;
  return ret;
}
int sbk_catalog_receive(int socket, uint32_t pid,
                        struct sbk_restore_region **out, unsigned int *count,
                        int *ready) {
  struct control_header h = {
      .magic = SBK_CTL_MAGIC, .version = SBK_CTL_VERSION, .pid = pid};
  struct control_range *ranges = NULL;
  struct sbk_restore_region *table = NULL;
  unsigned int got = 0, n = 0;
  int ret;
  if (!out || !count || !ready || !pid)
    return -EINVAL;
  *out = NULL;
  *count = 0;
  *ready = -1;
  ret = full_io(socket, &h, sizeof(h), 1);
  if (ret)
    return ret;
  ret = full_io(socket, &h, sizeof(h), 0);
  if (ret)
    return ret;
  if (h.magic != SBK_CTL_MAGIC || h.version != SBK_CTL_VERSION ||
      h.pid != pid || h.error || h.reserved || !h.count ||
      h.count > SBK_MAX_REGIONS)
    return -EPROTO;
  n = h.count;
  ranges = calloc(n, sizeof(*ranges));
  table = calloc(n, sizeof(*table));
  if (!ranges || !table) {
    ret = -ENOMEM;
    goto fail;
  }
  ret = full_io(socket, ranges, n * sizeof(*ranges), 0);
  if (ret)
    goto fail;
  uint64_t end = 0;
  for (unsigned int i = 0; i < n; i++) {
    if (!ranges[i].pages || ranges[i].pages > (1ULL << 20) ||
        (ranges[i].address & 4095) || ranges[i].address < end ||
        ranges[i].address > UINT64_MAX - (ranges[i].pages << 12)) {
      ret = -EPROTO;
      goto fail;
    }
    end = ranges[i].address + (ranges[i].pages << 12);
    table[i] = (struct sbk_restore_region){
        .address = ranges[i].address, .pages = ranges[i].pages, .fd = -1};
  }
  for (; got < n; got++) {
    ret = receive_right(socket);
    if (ret < 0)
      goto fail;
    table[got].fd = ret;
  }
  ret = receive_right(socket);
  if (ret < 0)
    goto fail;
  *ready = ret;
  *out = table;
  *count = n;
  free(ranges);
  return 0;
fail:
  for (unsigned int i = 0; i < got; i++)
    close(table[i].fd);
  free(table);
  free(ranges);
  return ret;
}
int sbk_catalog_poll(struct sbk_catalog *c, int timeout) {
  if (!c || c->phase != 1 || timeout < 0)
    return -EINVAL;
  struct pollfd *p = calloc(c->count * 2, sizeof(*p));
  if (!p)
    return -ENOMEM;
  for (size_t i = 0; i < c->count; i++) {
    p[2 * i] = (struct pollfd){.fd = c->entries[i]->ready_fd, .events = POLLIN};
    p[2 * i + 1] = (struct pollfd){
        .fd = c->entries[i]->drained ? -1 : c->entries[i]->drain_fd,
        .events = POLLIN};
  }
  int polled;
  do {
    polled = poll(p, c->count * 2, timeout);
  } while (polled < 0 && errno == EINTR);
  if (polled < 0) {
    int ret = -errno;
    free(p);
    return ret;
  }
  size_t done = 0;
  int ret = 0;
  for (size_t i = 0; i < c->count; i++) {
    struct catalog_entry *e = c->entries[i];
    uint64_t value;
    if ((p[2 * i].revents | p[2 * i + 1].revents) &
        (POLLERR | POLLNVAL | POLLHUP)) {
      ret = -EIO;
      break;
    }
    if (p[2 * i].revents & POLLIN) {
      if (read(e->ready_fd, &value, sizeof(value)) != sizeof(value) || !value) {
        ret = -EIO;
        break;
      }
      close(e->ready_fd);
      e->ready_fd = -1;
      for (size_t j = 0; j < c->count; j++) {
        struct catalog_entry *v = c->entries[j];
        if (v->record.restore_pid != e->record.restore_pid)
          continue;
        struct sbk_drain_status d;
        if (ioctl(v->fd, SBK_IOC_DRAIN_STATUS, &d) || !d.armed) {
          ret = -EPROTO;
          break;
        }
        struct sbk_hot_list hot = {.indices = (uintptr_t)v->hot,
                                   .count = v->hot_count};
        if (!d.drained && ioctl(v->fd, SBK_IOC_BACKGROUND, &hot)) {
          int error = errno;
          if (error != EINVAL || ioctl(v->fd, SBK_IOC_DRAIN_STATUS, &d) ||
              d.retired_tokens != d.pages) {
            ret = -error;
            break;
          }
        }
        v->started = 1;
      }
      if (ret)
        break;
    }
    if (p[2 * i + 1].revents & POLLIN) {
      struct sbk_drain_status d;
      if (read(e->drain_fd, &value, sizeof(value)) != sizeof(value) || !value ||
          ioctl(e->fd, SBK_IOC_DRAIN_STATUS, &d) || !d.drained ||
          d.retired_tokens != d.pages) {
        ret = -EIO;
        break;
      }
      e->drained = 1;
    }
    done += e->drained && e->started;
  }
  free(p);
  if (ret) {
    c->phase = -1;
    return ret;
  }
  return done == c->count;
}
int sbk_catalog_totals(struct sbk_catalog *c, struct sbk_stats *out) {
  if (!c || !out)
    return -EINVAL;
  memset(out, 0, sizeof(*out));
  for (size_t i = 0; i < c->count; i++) {
    struct sbk_stats s;
    if (ioctl(c->entries[i]->fd, SBK_IOC_STATS, &s))
      return -errno;
    out->faults += s.faults;
    out->hits += s.hits;
    out->waits += s.waits;
    out->errors += s.errors;
    for (int j = 0; j < SBK_LANES; j++)
      out->fetched[j] += s.fetched[j];
    out->installed_ahead += s.installed_ahead;
    out->skipped_install += s.skipped_install;
    out->fault_ns += s.fault_ns;
    if (s.fault_max_ns > out->fault_max_ns)
      out->fault_max_ns = s.fault_max_ns;
    for (int j = 0; j < 32; j++)
      out->hist_ns_pow2[j] += s.hist_ns_pow2[j];
    out->batches += s.batches;
    out->pages += s.pages;
    out->completed += s.completed;
    out->pretransferred += s.pretransferred;
    out->invalidated += s.invalidated;
  }
  return 0;
}
