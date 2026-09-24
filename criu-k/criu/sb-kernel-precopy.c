/* SPDX-License-Identifier: GPL-2.0 */
#include "sb-kernel-precopy.h"
#include "cr_options.h"
#include "log.h"
#include "pre-transfer.h"
#include "sb-kernel.h"
#include "sb-kernel-work.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>
#include <unistd.h>

#define PS_PAGE 4096ULL
#define PS_PFN_MASK ((1ULL << 55) - 1)
#define PS_PRESENT (1ULL << 63)
#define PS_SWAPPED (1ULL << 62)
#define PS_DIRTY (1ULL << 55)
extern volatile struct pid_data_list *pid_data_list;
extern int list_length;
static struct sbk_ps_region *ps;
static unsigned ps_count;

struct snapshot_region;
struct snapshot_worker {
  struct snapshot_region *group;
  struct sbk_ps_region *region;
  int pid;
  size_t begin, end, copied;
};
static int compare_index(const void *a, const void *b) {
  uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
  return (x > y) - (x < y);
}
static void *copy_snapshot_part(void *arg) {
  struct snapshot_worker *w = arg;
  struct sbk_ps_region *p = w->region;
  for (size_t j = w->begin; j < w->end;) {
    struct iovec local[128], remote[128];
    size_t n = w->end - j;
    if (n > 128)
      n = 128;
    for (size_t k = 0; k < n; k++) {
      uint64_t index = p->indices[j + k];
      local[k] = (struct iovec){(char *)p->snapshot + index * PS_PAGE, PS_PAGE};
      remote[k] = (struct iovec){(void *)(p->record.address + index * PS_PAGE), PS_PAGE};
    }
    ssize_t got;
    do {
      got = process_vm_readv(w->pid, local, n, remote, n, 0);
    } while (got < 0 && errno == EINTR);
    /* Each worker compacts only its own index partition. Partial or unreadable
     * pages are excluded; final dirty/PFN validation rejects concurrent writes. */
    size_t complete = got > 0 ? (size_t)got / PS_PAGE : 0;
    for (size_t k = 0; k < complete; k++)
      p->indices[w->begin + w->copied++] = p->indices[j + k];
    j += n;
  }
  return NULL;
}
/* One bounded pool serves every process/region, including large-region parts.
 * No nested pools: precopy-workers caps all source copy/register workers. */
struct snapshot_region {
  struct snapshot_worker part[32];
  unsigned remaining, parts, index;
  size_t candidates;
};
static struct {
  pthread_mutex_t lock;
  pthread_cond_t ready;
  struct sbk_work_pool *pool;
  struct snapshot_region *groups;
  unsigned *completed;
  unsigned published, consumed;
  int session, error;
} ps_work = {.lock = PTHREAD_MUTEX_INITIALIZER, .ready = PTHREAD_COND_INITIALIZER};

static int snapshot_part_run(void *arg) {
  struct snapshot_worker *w = arg;
  struct snapshot_region *g = w->group;
  struct sbk_ps_region *p = w->region;
  int ret = 0;
  copy_snapshot_part(w);
  pthread_mutex_lock(&ps_work.lock);
  bool last = !--g->remaining;
  pthread_mutex_unlock(&ps_work.lock);
  if (!last) return 0;
  /* Every partition's writes happen-before the last decrement. Only this
   * worker compacts across partitions or registers the immutable snapshot. */
  size_t copied = 0;
  for (unsigned i = 0; i < g->parts; i++) {
    memmove(p->indices + copied, p->indices + g->part[i].begin,
            g->part[i].copied * sizeof(*p->indices));
    copied += g->part[i].copied;
  }
  p->count = copied;
  if (copied) {
    p->record.remote.address = (uintptr_t)p->snapshot;
    if (ioctl(ps_work.session, SBK_IOC_EXPORT_REGION, &p->record.remote))
      ret = -errno;
  }
  pr_info("SB_KERNEL PS snapshot pid=%d candidates=%zu copied=%zu parts=%u streamed=1 result=%d address=%llx pages=%llu\n",
          w->pid, g->candidates, copied, g->parts, ret,
          (unsigned long long)p->record.address, (unsigned long long)p->record.remote.pages);
  pthread_mutex_lock(&ps_work.lock);
  if (ret && !ps_work.error) ps_work.error = ret;
  ps_work.completed[ps_work.published++] = g->index;
  pthread_cond_broadcast(&ps_work.ready);
  pthread_mutex_unlock(&ps_work.lock);
  return ret;
}
static void snapshot_part_dispose(void *arg) {
  /* Parts belong to groups; the coordinator frees them only after pool join. */
  (void)arg;
}
int sb_kernel_ps_next(struct sbk_ps_region **record) {
  pthread_mutex_lock(&ps_work.lock);
  while (ps_work.consumed == ps_work.published && !ps_work.error &&
         ps_work.consumed < ps_count)
    pthread_cond_wait(&ps_work.ready, &ps_work.lock);
  int ret = ps_work.error;
  if (!ret && ps_work.consumed < ps_count) {
    *record = &ps[ps_work.completed[ps_work.consumed++]];
    pr_info("SB_KERNEL PS yield pid=%u candidates=%zu ready=%u planned=%u\n",
            (*record)->record.source_pid, (*record)->count, ps_work.published, ps_count);
    ret = 1;
  }
  pthread_mutex_unlock(&ps_work.lock);
  return ret;
}
int sb_kernel_ps_finish(bool cancel) {
  struct sbk_work_stats stats = {};
  int ret = sbk_work_finish(ps_work.pool, cancel, &stats);
  ps_work.pool = NULL;
  free(ps_work.groups);
  free(ps_work.completed);
  ps_work.groups = NULL;
  ps_work.completed = NULL;
  pr_info("SB_KERNEL PS source_pool submitted=%u completed=%u peak=%u queued=%u result=%d\n",
          stats.submitted, stats.completed, stats.peak_active, stats.peak_queued, ret);
  return ret;
}

static int read_entries(int fd, uint64_t address, uint64_t *p, size_t count) {
  size_t bytes = count * sizeof(*p), done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, (char *)p + done, bytes - done,
                      address / PS_PAGE * sizeof(*p) + done);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      return n ? -errno : -EIO;
    done += n;
  }
  return 0;
}
static int prepare_process(int session, int slot, uint64_t *budget) {
  unsigned begin = ps_count;
  int pid = pid_data_list[slot].pid, pm = -1, ret = -ENOMEM;
  uint64_t *selected = NULL;
  size_t selected_count = 0, cursor = 0;
  uint64_t reserved = 0;
  (void)session;
  char path[64], *line = NULL;
  size_t capacity = 0;
  FILE *maps = NULL;
  /* The heat collector has finished before PS starts. Own one sorted, unique
   * copy: a sparse VMA must not rescan the entire heat list for every chunk. */
  int sampled = pid_data_list[slot].read_hot_num;
  if (sampled < 0) return -EINVAL;
  if (!sampled) return 0;
  if (!pid_data_list[slot].ReadList) return -EINVAL;
  selected = malloc((size_t)sampled * sizeof(*selected));
  if (!selected) return -ENOMEM;
  for (int j = 0; j < sampled; j++) {
    uint64_t address = pid_data_list[slot].ReadList[j];
    if (!(address & (PS_PAGE - 1))) selected[selected_count++] = address;
  }
  qsort(selected, selected_count, sizeof(*selected), compare_index);
  size_t unique = 0;
  for (size_t j = 0; j < selected_count; j++)
    if (!unique || selected[j] != selected[unique - 1]) selected[unique++] = selected[j];
  selected_count = unique;
  snprintf(path, sizeof(path), "/proc/%d/maps", pid);
  maps = fopen(path, "re");
  if (!maps) {
    ret = -errno;
    goto out;
  }
  while (getline(&line, &capacity, maps) >= 0) {
    unsigned long long start, end, offset, inode;
    unsigned major, minor;
    char perms[5];
    int used = 0;
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n", &start, &end, perms,
               &offset, &major, &minor, &inode, &used) != 7) {
      ret = -EINVAL;
      goto out;
    }
    char *name = line + used;
    if (inode || major || minor || perms[0] != 'r' || perms[3] != 'p' ||
        (name[0] && name[0] != '\n' && strncmp(name, "[heap]", 6) &&
         strncmp(name, "[stack]", 7) && strncmp(name, "[anon:", 6)))
      continue;
    while (cursor < selected_count && selected[cursor] < start) cursor++;
    while (cursor < selected_count && selected[cursor] < end) {
      if (!*budget || ps_count == SBK_MAX_REGIONS / 2) break;
      uint64_t address = selected[cursor];
      uint64_t pages = (end - address) / PS_PAGE;
      /* Publish each bounded span as an independent immutable MR. Waiting for
       * every copy part of a multi-GiB VMA starves the receiver between VMAs.
       * Chunk boundaries preserve original address/PFN indexing; the final
       * catalog imports intersecting PS slices and still validates every page.
       * Keep the legacy 4 GiB span available for an otherwise identical ablation. */
      uint64_t chunk_pages = opts.sb_kernel_ps_chunk_mb
          ? (uint64_t)opts.sb_kernel_ps_chunk_mb * 256 : (1ULL << 20);
      if (pages > chunk_pages) pages = chunk_pages;
      /* Large VMAs still contribute within the remaining snapshot budget.
       * Skipping a whole 4 GiB chunk under a 2 GiB budget loses every candidate. */
      if (pages > *budget) pages = *budget;
      size_t next = cursor + 1;
      while (next < selected_count && selected[next] < address + pages * PS_PAGE) {
        /* Keep short gaps to amortize MR/catalog overhead, but never pin a
         * large empty reservation merely because two sampled pages bracket it.
         * PS is optional: the bounded catalog may stop taking candidates; the
         * final frozen-source planner still covers every required data page. */
        if ((selected[next] - selected[next - 1]) / PS_PAGE - 1 > SBK_MAX_BATCH) break;
        next++;
      }
      /* Register only the candidate envelope. Trailing holes otherwise consume
       * snapshot budget and become real DMA-pinned zero pages in the controller.
       * Short interior gaps retain original-address indexing for the PS ABI. */
      pages = (selected[next - 1] - address) / PS_PAGE + 1;
      if (pages <= *budget && ps_count < SBK_MAX_REGIONS / 2) {
        struct sbk_ps_region *p = &ps[ps_count];
        p->indices = malloc((next - cursor) * sizeof(uint64_t));
        p->pfns = malloc(pages * sizeof(uint64_t));
        if (!p->indices || !p->pfns) {
          ret = -ENOMEM;
          ps_count++;
          goto out;
        }
        /* Native PS sampling selected pages rarely written during
         * its observation window. Final validation remains mandatory. */
        for (size_t j = cursor; j < next; j++)
          p->indices[p->count++] = (selected[j] - address) / PS_PAGE;
        if (p->count) {
          p->record.source_pid = pid;
          p->record.address = address;
          p->record.remote =
              (struct sbk_rdma_region){.address = address, .pages = pages};
          ps_count++;
          *budget -= pages;
          reserved += pages;
        } else {
          free(p->indices);
          free(p->pfns);
          memset(p, 0, sizeof(*p));
        }
      }
      cursor = next;
    }
  }
  if (ferror(maps)) {
    ret = -EIO;
    goto out;
  }
  if (begin == ps_count) {
    ret = 0;
    goto out;
  }
  /* ib_umem_get pins with FOLL_WRITE, and Linux deliberately does not clear
   * soft-dirty on DMA-pinned writable pages. Only our immutable snapshot is
   * registered; the original application pages remain trackable. */
  snprintf(path, sizeof(path), "/proc/%d/clear_refs", pid);
  int refs = open(path, O_WRONLY | O_CLOEXEC);
  if (refs < 0) {
    ret = -errno;
    goto out;
  }
  ret = write(refs, "4\n", 2) == 2 ? 0 : -EIO;
  close(refs);
  if (ret)
    goto out;
  snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
  pm = open(path, O_RDONLY | O_CLOEXEC);
  if (pm < 0) {
    ret = -errno;
    goto out;
  }
  for (unsigned i = begin; i < ps_count; i++) {
    struct sbk_ps_region *p = &ps[i];
    ret = read_entries(pm, p->record.address, p->pfns, p->record.remote.pages);
    if (ret)
      goto out;
    for (uint64_t j = 0; j < p->record.remote.pages; j++) {
      uint64_t e = p->pfns[j];
      p->pfns[j] = (e & PS_PRESENT) && !(e & PS_SWAPPED) ? e & PS_PFN_MASK : 0;
    }
    p->snapshot =
        mmap(NULL, p->record.remote.pages * PS_PAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (p->snapshot == MAP_FAILED) {
      p->snapshot = NULL;
      ret = -errno;
      goto out;
    }
    /* Only controller threads access this immutable PS snapshot. Utility
     * children (namespace/iptables helpers) must not inherit it: fork copies
     * DMA-pinned anonymous pages eagerly instead of using ordinary COW.
     * Set this before workers copy/register, so every exported snapshot has
     * the same lifetime and no helper needs to repair inheritance afterwards. */
    if (madvise(p->snapshot, p->record.remote.pages * PS_PAGE, MADV_DONTFORK)) {
      ret = -errno;
      goto out;
    }
    size_t eligible = 0;
    /* The sorted unique planner gives each worker disjoint snapshot pages. */
    for (size_t j = 0; j < p->count; j++)
      if (p->pfns[p->indices[j]] && (!eligible || p->indices[j] != p->indices[eligible-1]))
        p->indices[eligible++] = p->indices[j];
    p->count = eligible;
    /* Copy workers start only after the process epoch and PFNs are fixed. */
  }
  ret = 0;
out:
  pr_info("SB_KERNEL PS plan pid=%d sampled=%d unique=%zu regions=%u reserved_pages=%llu result=%d\n",
          pid, sampled, selected_count, ps_count - begin, (unsigned long long)reserved, ret);
  if (pm >= 0)
    close(pm);
  if (maps)
    fclose(maps);
  free(line);
  free(selected);
  return ret;
}
int sb_kernel_ps_begin(int session, unsigned *nr) {
  uint64_t budget =
      (uint64_t)(opts.sb_precopy_limit_mb ? opts.sb_precopy_limit_mb : 2048) * 256;
  unsigned workers = opts.sb_precopy_workers ? opts.sb_precopy_workers : 4;
  unsigned jobs = 0;
  int ret = -ENOMEM;
  if (ps || ps_work.pool) return -EBUSY;
  *nr = 0;
  if (!workers || workers > 32 || opts.sb_kernel_ps_chunk_mb > 4096) return -EINVAL;
  pr_info("SB_KERNEL PS settings chunk_mb=%u workers=%u budget_pages=%llu\n",
          opts.sb_kernel_ps_chunk_mb, workers, (unsigned long long)budget);
  ps = calloc(SBK_MAX_REGIONS / 2, sizeof(*ps));
  if (!ps) return -ENOMEM;
  for (int i = 0; i < list_length && budget; i++) {
    ret = prepare_process(session, i, &budget);
    if (ret) goto fail;
  }
  if (!ps_count) return 0;
  ps_work.groups = calloc(ps_count, sizeof(*ps_work.groups));
  ps_work.completed = calloc(ps_count, sizeof(*ps_work.completed));
  if (!ps_work.groups || !ps_work.completed) { ret = -ENOMEM; goto fail; }
  ps_work.session = session;
  ps_work.error = 0;
  ps_work.published = ps_work.consumed = 0;
  for (unsigned i = 0; i < ps_count; i++) {
    struct snapshot_region *g = &ps_work.groups[i];
    unsigned parts = (ps[i].count + 127) / 128;
    if (!parts) parts = 1; /* A raced-away region emits a zero-length skip. */
    if (parts > workers) parts = workers;
    g->parts = g->remaining = parts;
    g->index = i;
    g->candidates = ps[i].count;
    for (unsigned j = 0; j < parts; j++)
      g->part[j] = (struct snapshot_worker){.group = g, .region = &ps[i],
          .pid = ps[i].record.source_pid, .begin = ps[i].count * j / parts,
          .end = ps[i].count * (j + 1) / parts};
    jobs += parts;
  }
  /* All tasks fit the bounded region/partition catalog. Submission cannot
   * depend on draining the ready queue; that queue holds at most ps_count IDs. */
  ps_work.pool = sbk_work_create(workers, jobs, snapshot_part_run, snapshot_part_dispose);
  if (!ps_work.pool) { ret = -errno; goto fail; }
  for (unsigned i = 0; i < ps_count; i++)
    for (unsigned j = 0; j < ps_work.groups[i].parts; j++) {
      ret = sbk_work_submit(ps_work.pool, &ps_work.groups[i].part[j]);
      if (ret) goto fail;
    }
  *nr = ps_count;
  return 0;
fail:
  sb_kernel_ps_destroy();
  return ret;
}

int sb_kernel_ps_validate(int pid, const struct sbk_rdma_region *r,
                          uint64_t **dirty, size_t *nr) {
  uint64_t *entries = NULL, *invalid = NULL;
  int ret = 0;
  *dirty = NULL;
  *nr = 0;
  uint64_t end = r->address + r->pages * PS_PAGE;
  size_t candidates = 0;
  for (unsigned i = 0; i < ps_count; i++) {
    struct sbk_ps_region *p = &ps[i];
    uint64_t p_end = p->record.address + p->record.remote.pages * PS_PAGE;
    if (p->record.source_pid != (uint32_t)pid || p->record.address >= end || p_end <= r->address)
      continue;
    if (!entries) {
      entries = malloc(r->pages * sizeof(uint64_t));
      invalid = malloc(r->pages * sizeof(uint64_t));
      if (!entries || !invalid) {
        ret = -ENOMEM;
        goto out;
      }
      char path[64];
      snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
      int fd = open(path, O_RDONLY | O_CLOEXEC);
      ret = fd < 0 ? -errno : read_entries(fd, r->address, entries, r->pages);
      if (fd >= 0)
        close(fd);
      if (ret)
        goto out;
    }
    for (size_t j = 0; j < p->count; j++) {
      uint64_t source_index = p->indices[j];
      uint64_t address = p->record.address + source_index * PS_PAGE;
      if (address < r->address || address >= end)
        continue;
      uint64_t k = (address - r->address) / PS_PAGE, e = entries[k];
      candidates++;
      if (!(e & PS_PRESENT) || (e & (PS_SWAPPED | PS_DIRTY)) ||
          (e & PS_PFN_MASK) != p->pfns[source_index]) {
        if (*nr == r->pages) { ret = -EOVERFLOW; goto out; }
        invalid[(*nr)++] = k;
      }
    }
  }
  *dirty = invalid;
  invalid = NULL;
  pr_info("SB_KERNEL PS validated pid=%d address=%llx candidates=%zu invalid=%zu\n",
          pid, (unsigned long long)r->address, candidates, *nr);
out:
  free(entries);
  free(invalid);
  return ret;
}
void sb_kernel_ps_destroy(void) {
  if (ps_work.pool || ps_work.groups || ps_work.completed)
    sb_kernel_ps_finish(true);
  for (unsigned i = 0; i < ps_count; i++) {
    free(ps[i].indices);
    free(ps[i].pfns);
    if (ps[i].snapshot)
      munmap(ps[i].snapshot, ps[i].record.remote.pages * PS_PAGE);
  }
  free(ps);
  ps = NULL;
  ps_count = 0;
}
