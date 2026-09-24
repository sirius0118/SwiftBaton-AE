/* SPDX-License-Identifier: GPL-2.0 */
#include "sb-kernel-transfer.h"
#include "cr-sync.h"
#include "cr_options.h"
#include "log.h"
#include "pre-transfer.h"
#include "sb-kernel-catalog.h"
#include "sb-kernel-precopy.h"
#include "sb-kernel-sparse.h"
#include "sb-kernel-final-gate.h"
#include "sb-kernel-hot.h"
#include "sb-kernel-work.h"
#include "sb-kernel.h"
#include "sb-trace.h"
#include "uffd.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define LAZY_PAGES_RESTORE_FINISHED 0x52535446U
#define SBK_WIRE_MAGIC 0x53424b43U
struct sbk_wire_header {
  uint32_t magic, version, count, reserved;
};
struct sbk_wire_region {
  struct sbk_catalog_record record;
  uint64_t hot_count, dirty_count;
};
static int session_fd = -1, peer_fd = -1;
static bool source_final_exposed;
static struct sbk_catalog *destination;
static struct sbk_catalog_final *final_regions;
static unsigned int nr_final;
static struct sbk_final_gate final_gate = SBK_FINAL_GATE_INIT;
extern volatile struct pid_data_list *pid_data_list;
extern int list_length;
static uint64_t kernel_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

int sb_kernel_options(void) {
  if (!opts.sb_kernel_transfer)
    return 0;
  if ((!opts.lazy_pages && opts.mode != CR_LAZY_PAGES) || !opts.sb_image_rdma ||
      !opts.sb_u_precopy || opts.sb_parent_stage || opts.sb_parallel_transfer ||
      opts.track_mem || opts.sb_defer_fault_credits) {
    pr_err("K integration currently requires lazy-pages, image-rdma, u-precopy "
           "without track-mem; "
           "disable parent-stage and parallel-transfer.\n");
    return -EINVAL;
  }
  return 0;
}
int sb_kernel_connect(int socket_fd, int source) {
  struct sbk_rdma_setup setup = {.role = source ? SBK_RDMA_SOURCE
                                                : SBK_RDMA_DESTINATION,
                                 .port = 1,
                                 .gid_index = opts.sb_kernel_gid,
                                 .timeout_ms = opts.sb_kernel_timeout_ms ? opts.sb_kernel_timeout_ms : 2000};
  struct sbk_rdma_endpoint remote;
  struct sbk_config cfg = {
      .version = SBK_ABI_VERSION,
      .backend = SBK_BACKEND_RDMA,
      .prefetch_workers =
          opts.sb_prefetch_workers ? opts.sb_prefetch_workers : 2,
      .background_workers =
          opts.sb_install_workers ? opts.sb_install_workers : 4,
      .batch_pages = 32,
      .prefetch_enabled = !opts.sb_no_prefetch,
      .test_fail_page = SBK_NO_FAILURE};
  int ret;
  if (sb_kernel_options() || session_fd >= 0)
    return -EINVAL;
  /* Connection creation is coordinator-only, after any previous close. */
  pthread_mutex_lock(&final_gate.lock);
  final_gate.error = 0;
  final_gate.sealed = final_gate.closing = false;
  pthread_mutex_unlock(&final_gate.lock);
  source_final_exposed = false;
  if (cfg.prefetch_workers > 8 || cfg.background_workers > 8)
    return -EINVAL;
  setup.slots[SBK_DEMAND] = opts.sb_fault_workers ? opts.sb_fault_workers : 4;
  setup.slots[SBK_PREFETCH] = cfg.prefetch_workers;
  setup.slots[SBK_BACKGROUND] = cfg.background_workers;
  if (setup.slots[SBK_DEMAND] > 8)
    return -EINVAL;
  snprintf(setup.device, sizeof(setup.device), "%s",
           opts.sb_kernel_device ? opts.sb_kernel_device : "mlx5_1");
  session_fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
  if (session_fd < 0)
    return -errno;
  struct sbk_capabilities caps;
  if (ioctl(session_fd, SBK_IOC_CAPABILITIES, &caps) ||
      caps.version != SBK_ABI_VERSION ||
      (!source && !(caps.features & SBK_FEATURE_ANONYMOUS_PTE))) {
    pr_err("SwiftBaton-K requires the anonymous-PTE kernel bridge on the "
           "destination\n");
    ret = -EOPNOTSUPP;
    goto fail;
  }
  if (source && opts.sb_kernel_export_workers > 1 &&
      !(caps.features & SBK_FEATURE_PARALLEL_EXPORT)) {
    pr_err("SwiftBaton-K parallel export capability unavailable\n");
    ret = -EOPNOTSUPP;
    goto fail;
  }
  if (ioctl(session_fd, SBK_IOC_RDMA_CREATE, &setup)) {
    ret = -errno;
    goto fail;
  }
  if (sync_transfer(socket_fd, &setup.local, sizeof(setup.local), true) ||
      sync_transfer(socket_fd, &remote, sizeof(remote), false)) {
    ret = -EIO;
    goto fail;
  }
  if (ioctl(session_fd, SBK_IOC_RDMA_CONNECT, &remote)) {
    ret = -errno;
    goto fail;
  }
  if (!source) {
    destination = sbk_catalog_create(session_fd, &cfg);
    if (!destination) {
      ret = -errno;
      goto fail;
    }
  }
  struct timeval deadline = {.tv_sec = 300};
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &deadline,
                 sizeof(deadline)) ||
      setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &deadline,
                 sizeof(deadline))) {
    ret = -errno;
    goto fail;
  }
  peer_fd = socket_fd;
  pr_info("SB_KERNEL connected role=%s device=%s PF=%u FT=%u BG=%u "
          "pretransfer=%s\n",
          source ? "source" : "destination", setup.device, setup.slots[0],
          setup.slots[1], setup.slots[2],
          opts.sb_no_pretransfer ? "disabled" : "enabled");
  return 0;
fail:
  sb_kernel_transfer_close();
  return ret;
}
/* Called in the owner parasite while every application thread is frozen. The
 * final MR remains pinned and immutable until the destination drain ACK. */
int sb_kernel_register_final(struct parasite_ctl *ctl, int pid, int vpid,
                             struct sbk_rdma_region *regions, unsigned count) {
  int ret = 0;
  unsigned export_peak = 0, export_span = 1U << 20;
  struct sbk_rdma_region *sparse = NULL;
  struct sbk_hot_range *hot_index = NULL;
  uint64_t begin = kernel_now_ns(), locked = 0, scan_ns = 0, validate_ns = 0, export_ns = 0, hot_ns = 0, stage;
  if (!count)
    return 0;
  if (pid <= 0 || vpid <= 0)
    return -EINVAL;
  ret = sbk_final_enter(&final_gate);
  if (ret)
    return ret;
  locked = kernel_now_ns();
  bool reservation_locked = true;
  if (session_fd < 0) { ret = -EINVAL; goto out; }
  if (count > SBK_MAX_REGIONS - nr_final) {
    ret = -E2BIG;
    goto out;
  }
  if (!final_regions) {
    final_regions = calloc(SBK_MAX_REGIONS, sizeof(*final_regions));
    if (!final_regions) {
      ret = -ENOMEM;
      goto out;
    }
  }
  {
    /* PS uses at most half the catalog. Keep this same bound for final MRs. */
    unsigned capacity = nr_final < SBK_MAX_REGIONS / 2 ? SBK_MAX_REGIONS / 2 - nr_final : 0;
    if (count > capacity) { ret = -E2BIG; goto out; }
    /* Do not consume all descriptors on one fragmented process while other
     * sampled processes still need their final MRs. Never trim source data. */
    unsigned remaining = 0;
    for (int p = 0; p < list_length; p++) {
      unsigned i;
      for (i = 0; i < nr_final; i++)
        if (final_regions[i].record.source_pid == pid_data_list[p].pid) break;
      if (i == nr_final) remaining++;
    }
    if (remaining > 1 && capacity / remaining >= count)
      capacity /= remaining;
    if (!opts.sb_kernel_dense) {
    struct sbk_sparse_stats stats;
    stage = kernel_now_ns();
    ret = sbk_sparse_plan(pid, regions, count, capacity, &sparse, &count, &stats);
    scan_ns = kernel_now_ns() - stage;
    if (ret) goto out;
    regions = sparse;
    pr_info("SB_KERNEL sparse pid=%d virtual=%llu data=%llu registered=%llu skipped=%llu regions=%u control_page=%u\n",
            pid, (unsigned long long)stats.virtual_pages, (unsigned long long)stats.data_pages,
            (unsigned long long)stats.registered_pages,
            (unsigned long long)(stats.virtual_pages - stats.registered_pages), count, stats.control_page);
    }
    if (opts.sb_kernel_export_chunk_mb) {
      struct sbk_rdma_region *chunks;
      unsigned chunk_count;
      ret = sbk_split_plan(regions, count, opts.sb_kernel_export_chunk_mb * 256,
                           capacity, &chunks, &chunk_count, &export_span);
      if (ret) goto out;
      free(sparse); sparse = chunks; regions = chunks; count = chunk_count;
    }
  }
  unsigned base = nr_final;
  /* Publish immutable identity while reserving disjoint slots. Other planners
   * inspect source_pid to share capacity. No whole-record writes after unlock:
   * each worker then owns only its slots' remote/dirty/hot fields. */
  for (unsigned i = 0; i < count; i++) {
    struct sbk_catalog_final *f = &final_regions[nr_final++];
    f->record.source_pid = pid;
    f->record.restore_pid = vpid;
    f->record.address = regions[i].address;
  }
  pthread_mutex_unlock(&final_gate.lock);
  reservation_locked = false;
  /* PS is read-only throughout final registration; close waits for our gate
   * reference before destroying it or closing the RDMA session. */
  /* ib_umem_get pins with FOLL_WRITE even for a read-only MR; registering
   * the final MR can itself set soft-dirty. Validate the application epoch
   * while frozen BEFORE that operation. A GUP COW copies the same bytes. */
  stage = kernel_now_ns();
  for (unsigned int i = 0; i < count; i++) {
    struct sbk_catalog_final *f = &final_regions[base + i];
    uint64_t *dirty;
    ret = sb_kernel_ps_validate(pid, &regions[i], &dirty, &f->dirty_count);
    if (ret)
      goto out;
    f->dirty = dirty;
  }
  validate_ns = kernel_now_ns() - stage;
  stage = kernel_now_ns();
  ret = sb_kernel_export_seized(ctl, session_fd, regions, count,
                                opts.sb_kernel_export_workers, &export_peak);
  export_ns = kernel_now_ns() - stage;
  if (ret)
    goto out;
  stage = kernel_now_ns();
  if (!opts.sb_no_hot_first && count) {
    hot_index = calloc(count, sizeof(*hot_index));
    if (!hot_index) { ret = -ENOMEM; goto out; }
  }
  for (unsigned int i = 0; i < count; i++) {
    struct sbk_catalog_final *f = &final_regions[base + i];
    f->record.remote = regions[i];
    if (opts.sb_no_hot_first)
      continue;
    uint64_t *hot = malloc(regions[i].pages * sizeof(*hot));
    if (!hot) {
      ret = -ENOMEM;
      goto out;
    }
    f->hot = hot;
    hot_index[i] = (struct sbk_hot_range){
        .address = regions[i].address, .pages = regions[i].pages,
        .order = hot, .used = &f->hot_count};
  }
  if (!opts.sb_no_hot_first) {
    ret = sbk_hot_index_prepare(hot_index, count);
    if (ret)
      goto out;
    /* Walk each sampled priority list once. The old region-outer loop visited
     * every heat node once per final MR while the application was stopped. */
    for (int p = 0; p < list_length; p++) {
      if (pid_data_list[p].pid != pid)
        continue;
      for (int score = PRIORITY_QUEUE_LEVEL - 1; score >= 0; score--) {
        volatile struct score_list *s = pid_data_list[p].dirtylist[score];
        for (; s; s = s->next) {
          ret = sbk_hot_index_append(hot_index, count, s->addr);
          if (ret)
            goto out;
        }
      }
    }
  }
  hot_ns = kernel_now_ns() - stage;
out:
  pr_info("SB_KERNEL final_export pid=%d workers=%u chunk_mb=%u effective_pages=%u peak=%u regions=%u result=%d\n",
          pid, opts.sb_kernel_export_workers, opts.sb_kernel_export_chunk_mb,
          export_span, export_peak, count, ret);
  pr_info("SB_KERNEL final_prepare pid=%d regions=%u result=%d lock_wait_us=%llu scan_us=%llu validation_us=%llu export_us=%llu hot_us=%llu total_us=%llu\n",
          pid, count, ret, (unsigned long long)((locked - begin) / 1000),
          (unsigned long long)(scan_ns / 1000), (unsigned long long)(validate_ns / 1000),
          (unsigned long long)(export_ns / 1000), (unsigned long long)(hot_ns / 1000),
          (unsigned long long)((kernel_now_ns() - begin) / 1000));
  free(hot_index);
  free(sparse);
  if (reservation_locked) pthread_mutex_unlock(&final_gate.lock);
  sbk_final_release(&final_gate, ret);
  return ret;
}
/* PS snapshots the running source, leaving original pages unpinned. The
 * receiver caches pages in kernel memory, without publishing any PTE. */
int sb_kernel_send_ps(int socket_fd) {
  unsigned count = 0, sent = 0, skipped = 0;
  uint64_t pages = 0, begin = kernel_now_ns(), first = 0;
  int ret = 0;
  if (!opts.sb_no_pretransfer) {
    sb_trace("kernel.ps_register_begin");
    ret = sb_kernel_ps_begin(session_fd, &count);
    if (ret) {
      pr_err("K PS preparation failed: %d\n", ret);
      return ret;
    }
  }
  /* Version 3 counts planned regions. A fully zero wire record is an explicit
   * skipped region (all candidates raced away). Final wire protocol stays v2. */
  struct sbk_wire_header h = {SBK_WIRE_MAGIC, 3, count, 1}, ack;
  if (sync_transfer(socket_fd, &h, sizeof(h), true)) { ret = -EIO; goto out; }
  for (unsigned i = 0; i < count; i++) {
    struct sbk_ps_region *p;
    struct sbk_wire_region w = {};
    ret = sb_kernel_ps_next(&p);
    if (ret != 1) { if (!ret) ret = -EIO; goto out; }
    ret = 0;
    if (p->count) {
      w.record = p->record;
      w.hot_count = p->count;
      pages += p->count;
      sent++;
    } else skipped++;
    if (sync_transfer(socket_fd, &w, sizeof(w), true) ||
        (p->count && sync_transfer(socket_fd, p->indices, p->count * sizeof(uint64_t), true))) {
      ret = -EIO;
      goto out;
    }
    if (!first) first = kernel_now_ns();
  }
  ret = sb_kernel_ps_finish(false);
  sb_trace("kernel.ps_register_done");
  if (ret) return ret;
  if (sync_transfer(socket_fd, &ack, sizeof(ack), false) || memcmp(&h, &ack, sizeof(h)))
    return -EIO;
  pr_info("SB_KERNEL PS transferred regions=%u planned=%u skipped=%u pages=%llu source_resumed=1 first_send_us=%llu total_us=%llu streaming=1\n",
          sent, count, skipped, (unsigned long long)pages,
          (unsigned long long)(first ? (first - begin) / 1000 : 0),
          (unsigned long long)((kernel_now_ns() - begin) / 1000));
  return 0;
out:
  shutdown(socket_fd, SHUT_RDWR);
  sb_kernel_ps_finish(true); /* Join copies/exports before any source cleanup. */
  return ret;
}
struct ps_receive_job {
  struct sbk_catalog *catalog;
  int socket_fd;
  struct sbk_catalog_record record;
  size_t count;
  uint64_t indices[];
};
static int ps_receive_run(void *arg) {
  struct ps_receive_job *job = arg;
  int ret = sbk_catalog_stage(job->catalog, &job->record, job->indices, job->count);
  if (ret) shutdown(job->socket_fd, SHUT_RDWR);
  return ret;
}
int sb_kernel_receive_ps(int socket_fd) {
  struct sbk_wire_header h;
  struct sbk_work_pool *pool = NULL;
  struct sbk_work_stats stats = {};
  unsigned workers = opts.sb_precopy_workers ? opts.sb_precopy_workers : 4;
  uint64_t pages = 0, begin = kernel_now_ns();
  unsigned skipped = 0;
  int ret = -EPROTO;
  if (!destination || sync_transfer(socket_fd, &h, sizeof(h), false) ||
      h.magic != SBK_WIRE_MAGIC || (h.version != 2 && h.version != 3) || h.reserved != 1 ||
      h.count > SBK_MAX_REGIONS / 2 || (opts.sb_no_pretransfer && h.count))
    return -EPROTO;
  if (h.count) {
    if (workers > h.count) workers = h.count;
    pool = sbk_work_create(workers, workers * 2, ps_receive_run, free);
    if (!pool) return -errno;
  }
  for (unsigned i = 0; i < h.count; i++) {
    struct sbk_wire_region w, empty = {};
    if (sync_transfer(socket_fd, &w, sizeof(w), false)) { ret = -EIO; goto fail; }
    if (h.version == 3 && !memcmp(&w, &empty, sizeof(w))) { skipped++; continue; }
    if (!w.hot_count || w.dirty_count || w.hot_count > w.record.remote.pages ||
        w.record.remote.pages > (1ULL << 20)) { ret = -EPROTO; goto fail; }
    struct ps_receive_job *job = malloc(sizeof(*job) + w.hot_count * sizeof(uint64_t));
    if (!job) { ret = -ENOMEM; goto fail; }
    job->catalog = destination;
    job->socket_fd = socket_fd;
    job->record = w.record;
    job->count = w.hot_count;
    if (sync_transfer(socket_fd, job->indices, w.hot_count * sizeof(uint64_t), false))
      ret = -EIO;
    else ret = sbk_work_submit(pool, job);
    if (ret) { free(job); goto fail; }
    pages += w.hot_count;
  }
  ret = sbk_work_finish(pool, false, &stats);
  if (ret) return ret;
  pr_info("SB_KERNEL PS cached regions=%u planned=%u skipped=%u pages=%llu workers=%u peak=%u queued=%u total_us=%llu streaming=1\n",
          h.count - skipped, h.count, skipped, (unsigned long long)pages,
          h.count ? workers : 0, stats.peak_active, stats.peak_queued,
          (unsigned long long)((kernel_now_ns() - begin) / 1000));
  /* ACK is a barrier for all regions, never just successful queue admission. */
  return sync_transfer(socket_fd, &h, sizeof(h), true) ? -EIO : 0;
fail:
  shutdown(socket_fd, SHUT_RDWR);
  sbk_work_finish(pool, true, NULL); /* No catalog or indices can be freed early. */
  return ret;
}

int sb_kernel_send_final(int socket_fd) {
  int ret = sbk_final_seal(&final_gate);
  if (ret) return ret;
  struct sbk_wire_header h = {SBK_WIRE_MAGIC, 2, nr_final, 0};
  ret = -EIO;
  if (!nr_final) goto out;
  /* A partial send may have reached the peer even if the local write fails.
   * Preserve this fence across close: resuming the source after this point
   * requires an external proof that the destination has been destroyed. */
  source_final_exposed = true;
  if (sync_transfer(socket_fd, &h, sizeof(h), true))
    goto out;
  for (unsigned int i = 0; i < nr_final; i++) {
    struct sbk_wire_region w = {.record = final_regions[i].record,
                                .hot_count = final_regions[i].hot_count,
                                .dirty_count = final_regions[i].dirty_count};
    if (sync_transfer(socket_fd, &w, sizeof(w), true) ||
        sync_transfer(socket_fd, (void *)final_regions[i].hot,
                      w.hot_count * sizeof(uint64_t), true) ||
        sync_transfer(socket_fd, (void *)final_regions[i].dirty,
                      w.dirty_count * sizeof(uint64_t), true))
      goto out;
  }
  ret = 0;
out:
  pthread_mutex_unlock(&final_gate.lock);
  return ret;
}
int sb_kernel_client_receive(int socket_fd) {
  struct sbk_wire_header h;
  int ret = -EPROTO;
  if (!destination || sync_transfer(socket_fd, &h, sizeof(h), false) ||
      h.magic != SBK_WIRE_MAGIC || h.version != 2 || h.reserved || !h.count ||
      h.count > SBK_MAX_REGIONS)
    return -EPROTO;
  final_regions = calloc(h.count, sizeof(*final_regions));
  if (!final_regions)
    return -ENOMEM;
  for (unsigned int i = 0; i < h.count; i++) {
    struct sbk_wire_region w;
    if (sync_transfer(socket_fd, &w, sizeof(w), false) ||
        w.hot_count > w.record.remote.pages ||
        w.dirty_count > w.record.remote.pages || !w.record.remote.pages ||
        w.record.remote.pages > (1ULL << 20))
      goto out;
    struct sbk_catalog_final *f = &final_regions[nr_final++];
    f->record = w.record;
    f->hot_count = w.hot_count;
    f->dirty_count = w.dirty_count;
    if (w.hot_count) {
      f->hot = malloc(w.hot_count * sizeof(uint64_t));
      if (!f->hot) {
        ret = -ENOMEM;
        goto out;
      }
      if (sync_transfer(socket_fd, (void *)f->hot,
                        w.hot_count * sizeof(uint64_t), false))
        goto out;
    }
    if (w.dirty_count) {
      f->dirty = malloc(w.dirty_count * sizeof(uint64_t));
      if (!f->dirty) {
        ret = -ENOMEM;
        goto out;
      }
      if (sync_transfer(socket_fd, (void *)f->dirty,
                        w.dirty_count * sizeof(uint64_t), false))
        goto out;
    }
  }
  ret = sbk_catalog_seal(destination, final_regions, nr_final);
out:
  return ret;
}
int sb_kernel_source_finish(void) {
  struct sbk_wire_header ack;
  int ret = 0;
  if (session_fd < 0 || peer_fd < 0 ||
      sync_transfer(peer_fd, &ack, sizeof(ack), false) ||
      ack.magic != SBK_WIRE_MAGIC || ack.version != 2 ||
      ack.count != nr_final || ack.reserved)
    ret = -EIO;
  /* Revoke even on a lost peer. The dump cleanup keeps the source stopped
   * after final exposure; losing an ACK is not permission to resume it. */
  if (session_fd >= 0 && ioctl(session_fd, SBK_IOC_REVOKE_SOURCE))
    ret = -errno;
  sb_kernel_transfer_close();
  return ret;
}
int sb_kernel_source_exposed(void) { return source_final_exposed; }
int sb_kernel_client_serve(int listen_fd, int socket_fd) {
  int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC), ret = -EIO;
  unsigned processes = 0, served = 0;
  bool finished = false, drained = false;
  struct sbk_wire_header ack = {SBK_WIRE_MAGIC, 2, nr_final, 0};
  struct sbk_stats st;
  close(listen_fd);
  if (client < 0)
    return -errno;
  struct timeval deadline = {.tv_sec = 300};
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  if (setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &deadline,
                 sizeof(deadline)) ||
      setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &deadline, sizeof(deadline)))
    goto out;
  for (unsigned i = 0; i < nr_final; i++) {
    unsigned j;
    for (j = 0; j < i; j++)
      if (final_regions[j].record.restore_pid ==
          final_regions[i].record.restore_pid)
        break;
    if (j == i)
      processes++;
  }
  while (!finished || !drained) {
    struct pollfd p = {.fd = finished ? -1 : client, .events = POLLIN};
    ret = -EIO;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec > 300) {
      ret = -ETIMEDOUT;
      goto out;
    }
    int n = poll(&p, 1, 1);
    if (n < 0 && errno == EINTR)
      continue;
    if (n < 0)
      goto out;
    if (p.revents & POLLIN) {
      if (served < processes) {
        ret = sbk_catalog_serve(destination, client);
        if (ret)
          goto out;
        served++;
        ret = -EIO;
      } else {
        uint32_t fin;
        if (sync_transfer(client, &fin, sizeof(fin), false) ||
            fin != LAZY_PAGES_RESTORE_FINISHED)
          goto out;
        finished = true;
      }
    }
    if ((p.revents & (POLLERR | POLLNVAL | POLLHUP)) && !finished)
      goto out;
    ret = sbk_catalog_poll(destination, 0);
    if (ret < 0)
      goto out;
    drained = ret;
  }
  ret = -EIO;
  if (sbk_catalog_totals(destination, &st))
    goto out;
  pr_info("SB_KERNEL complete regions=%u pages=%llu PF=%llu FT=%llu BG=%llu "
          "errors=%llu fault_ns=%llu fault_max_ns=%llu PS=%llu invalid=%llu "
          "faults=%llu hits=%llu waits=%llu ahead=%llu skipped=%llu batches=%llu\n",
          nr_final, (unsigned long long)st.pages,
          (unsigned long long)st.fetched[0], (unsigned long long)st.fetched[1],
          (unsigned long long)st.fetched[2], (unsigned long long)st.errors,
          (unsigned long long)st.fault_ns, (unsigned long long)st.fault_max_ns,
          (unsigned long long)st.pretransferred,
          (unsigned long long)st.invalidated, (unsigned long long)st.faults,
          (unsigned long long)st.hits, (unsigned long long)st.waits,
          (unsigned long long)st.installed_ahead,
          (unsigned long long)st.skipped_install, (unsigned long long)st.batches);
  struct sbk_dispatch_stats dispatch;
  if (!ioctl(session_fd, SBK_IOC_DISPATCH_STATS, &dispatch)) {
    for (unsigned int lane = 0; lane < 2; lane++) {
      const struct sbk_dispatch_lane_stats *d = &dispatch.lane[lane];
      pr_info("SB_KERNEL dispatch lane=%u workers=%u peak=%u active=%u "
              "submitted=%llu quanta=%llu completed=%llu queued=%llu queue_peak=%llu\n",
              lane + 1, d->workers, d->peak, d->active,
              (unsigned long long)d->submitted, (unsigned long long)d->quanta,
              (unsigned long long)d->completed, (unsigned long long)d->queued,
              (unsigned long long)d->queue_peak);
      if (!d->workers || d->peak > d->workers || d->active || d->queued ||
          d->submitted != d->completed) {
        pr_err("SB_KERNEL session workers did not drain\n");
        ret = -EIO;
        goto out;
      }
    }
  } else if (errno == EOPNOTSUPP || errno == ENOTTY) {
    pr_info("SB_KERNEL dispatch disabled\n");
  } else {
    ret = -errno;
    goto out;
  }
  /* Counters were already collected in the module. Emit after drain, never on
   * the fault path. Bin i is fls64(ns), capped at 31; these are bounds, not
   * exact per-fault percentiles or complete userspace pause samples. */
  for (unsigned int i = 0; i < 32; i++)
    if (st.hist_ns_pow2[i])
      pr_info("SB_KERNEL fault_bin index=%u count=%llu\n", i,
              (unsigned long long)st.hist_ns_pow2[i]);
  if (st.errors) {
    ret = -EIO;
    goto out;
  }
  ret = sync_transfer(socket_fd, &ack, sizeof(ack), true) ? -EIO : 0;
out:
  close(client);
  sb_kernel_transfer_close();
  return ret;
}
void sb_kernel_transfer_close(void) {
  sbk_final_close(&final_gate);
  sbk_catalog_destroy(destination);
  destination = NULL;
  for (unsigned int i = 0; i < nr_final; i++) {
    free((void *)final_regions[i].hot);
    free((void *)final_regions[i].dirty);
  }
  sb_kernel_ps_destroy();
  free(final_regions);
  final_regions = NULL;
  nr_final = 0;
  if (session_fd >= 0)
    close(session_fd);
  session_fd = -1;
  peer_fd = -1;
  pthread_mutex_unlock(&final_gate.lock);
}
