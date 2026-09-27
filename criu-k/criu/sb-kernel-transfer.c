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
#include "sb-kernel-dma-wire.h"
#include "sb-kernel-layout.h"
#include "sb-kernel-layout-wire.h"
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
/* Remote-fork keeps the source export alive for the entire destination
 * service lifetime. The old five-minute control timeout expired exactly as
 * a 300-second workload entered full-key validation, although data transfer
 * was still active. Keep a bounded failure timeout, but leave room for
 * workload, poststeady sampling, validation and destination shutdown. */
#define SBK_SESSION_TIMEOUT_SEC 3600
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
/* Sampling is complete before sb_kernel_send_ps(). Keep its exact priority
 * order in compact arrays during PS instead of walking cold linked lists
 * while the application is frozen. These are hints, never page validity. */
struct sbk_hot_snapshot {
  int pid;
  uint64_t *addresses;
  size_t count;
  struct sbk_hot_run *runs;
  size_t run_count;
};
static struct sbk_hot_snapshot *hot_snapshots;
static unsigned hot_snapshot_count;
struct sbk_source_prearm {
  int pid;
  struct sbk_rdma_region region;
  uint64_t *pfns;
};
static struct sbk_catalog_layout *source_layout;
static unsigned source_layout_count;
static struct sbk_source_prearm *source_prearm;
static unsigned source_prearm_count;
struct sbk_source_hot {
  int pid;
  uint64_t address, pages;
  uint64_t *order;
  size_t count;
};
static struct sbk_source_hot *source_hot;
static unsigned source_hot_count;
#define SBK_PM_PRESENT (1ULL << 63)
#define SBK_PM_PFN_MASK ((1ULL << 55) - 1)
static int source_pagemap_read(int pid, uint64_t address, uint64_t pages, uint64_t *entries) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -errno;
  size_t bytes = pages * sizeof(*entries), done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, (char *)entries + done, bytes - done,
                      address / 4096 * sizeof(*entries) + done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { int ret = n ? -errno : -EIO; close(fd); return ret; }
    done += n;
  }
  close(fd);
  return 0;
}
enum source_prearm_failure {
  PREARM_CAPTURE_OK, PREARM_CAPTURE_ALLOC, PREARM_CAPTURE_PAGEMAP,
  PREARM_CAPTURE_PFN, PREARM_CAPTURE_NOTIFIER
};
static enum source_prearm_failure source_prearm_capture(struct sbk_source_prearm *p) {
  uint64_t *entries = malloc(p->region.pages * sizeof(*entries));
  if (!entries) return PREARM_CAPTURE_ALLOC;
  int ret = source_pagemap_read(p->pid, p->region.address, p->region.pages, entries);
  if (ret) { free(entries); return PREARM_CAPTURE_PAGEMAP; }
  for (uint64_t j = 0; j < p->region.pages; j++) {
    if (!(entries[j] & SBK_PM_PRESENT) || !(entries[j] & SBK_PM_PFN_MASK)) {
      free(entries);
      return PREARM_CAPTURE_PFN;
    }
    entries[j] &= SBK_PM_PFN_MASK;
  }
  struct sbk_prearm_status status = {.region = p->region};
  if (ioctl(session_fd, SBK_IOC_PREARM_STATUS, &status) || !status.valid) {
    free(entries);
    return PREARM_CAPTURE_NOTIFIER;
  }
  p->pfns = entries;
  return PREARM_CAPTURE_OK;
}
static bool source_pfns_equal(const uint64_t *baseline, const uint64_t *observed,
                              uint64_t pages) {
  if (!baseline || !observed) return false;
  for (uint64_t i = 0; i < pages; i++)
    if (!(observed[i] & SBK_PM_PRESENT) ||
        (observed[i] & SBK_PM_PFN_MASK) != baseline[i]) return false;
  return true;
}
static struct sbk_source_prearm *source_prearm_find(int pid,
                                                   const struct sbk_rdma_region *r) {
  for (unsigned i = 0; i < source_prearm_count; i++) {
    struct sbk_source_prearm *p = &source_prearm[i];
    if (p->pid == pid && p->region.address == r->address &&
        p->region.pages == r->pages && p->pfns) return p;
  }
  return NULL;
}
static int source_range_compare(const void *a, const void *b) {
  const struct sbk_rdma_region *x = a, *y = b;
  return (x->address > y->address) - (x->address < y->address);
}
/* Split CRIU's frozen VMAs around still-valid prearmed MRs. Every omitted
 * page is pinned by a read-only MR whose notifier was installed before GUP;
 * all other pages, including newly faulted gaps, retain the ordinary
 * frozen pagemap scan. PS candidates are still validated separately. Return 0 to keep the old full scan. */
static int source_partition_prearmed(int pid, const struct sbk_rdma_region *input,
                                    unsigned count, unsigned capacity,
                                    struct sbk_rdma_region **scan, unsigned *scan_count,
                                    struct sbk_rdma_region **trusted,
                                    unsigned *trusted_count, uint64_t *virtual_pages) {
  struct sbk_rdma_region *ready = NULL, *remaining = NULL, *stable = NULL;
  unsigned nr_ready = 0, nr_scan = 0, nr_trusted = 0;
  uint64_t previous = 0, pages = 0;
  *scan = *trusted = NULL;
  *scan_count = *trusted_count = 0;
  *virtual_pages = 0;
  if (!source_prearm_count || !count || count > capacity) return 0;
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    if (!r->pages || r->pages > (1U << 20) || (r->address & 4095) ||
        r->address < previous || r->address > UINT64_MAX - r->pages * 4096)
      return 0;
    previous = r->address + r->pages * 4096;
    pages += r->pages;
  }
  ready = calloc(source_prearm_count, sizeof(*ready));
  if (!ready) return 0;
  for (unsigned i = 0; i < source_prearm_count; i++) {
    const struct sbk_source_prearm *p = &source_prearm[i];
    if (p->pid != pid || !p->pfns) continue;
    struct sbk_prearm_status status = {.region = p->region};
    if (ioctl(session_fd, SBK_IOC_PREARM_STATUS, &status) || !status.valid)
      continue;
    ready[nr_ready++] = (struct sbk_rdma_region){
        .address = p->region.address, .pages = p->region.pages};
  }
  if (!nr_ready) goto fallback;
  qsort(ready, nr_ready, sizeof(*ready), source_range_compare);
  for (unsigned i = 1; i < nr_ready; i++)
    if (ready[i - 1].address + ready[i - 1].pages * 4096 > ready[i].address)
      goto fallback;
  remaining = calloc(count + nr_ready, sizeof(*remaining));
  stable = calloc(nr_ready, sizeof(*stable));
  if (!remaining || !stable) goto fallback;
  unsigned first = 0;
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    uint64_t cursor = r->address, end = cursor + r->pages * 4096;
    while (first < nr_ready &&
           ready[first].address + ready[first].pages * 4096 <= cursor) first++;
    for (unsigned j = first; j < nr_ready && ready[j].address < end; j++) {
      uint64_t next = ready[j].address + ready[j].pages * 4096;
      if (ready[j].address < cursor || next > end) continue;
      if (ready[j].address > cursor)
        remaining[nr_scan++] = (struct sbk_rdma_region){
            .address = cursor, .pages = (ready[j].address - cursor) / 4096};
      stable[nr_trusted++] = ready[j];
      cursor = next;
    }
    if (cursor < end)
      remaining[nr_scan++] = (struct sbk_rdma_region){
          .address = cursor, .pages = (end - cursor) / 4096};
  }
  if (!nr_trusted || nr_trusted >= capacity ||
      nr_scan > capacity - nr_trusted) goto fallback;
  free(ready);
  *scan = remaining; *scan_count = nr_scan;
  *trusted = stable; *trusted_count = nr_trusted;
  *virtual_pages = pages;
  return 1;
fallback:
  free(ready); free(remaining); free(stable);
  return 0;
}
static bool source_trusted_exact(const struct sbk_rdma_region *trusted,
                                 unsigned count, const struct sbk_rdma_region *r) {
  for (unsigned i = 0; i < count; i++)
    if (trusted[i].address == r->address && trusted[i].pages == r->pages)
      return true;
  return false;
}
static struct sbk_source_hot *source_hot_find(int pid,
                                              const struct sbk_rdma_region *r) {
  for (unsigned i = 0; i < source_hot_count; i++) {
    struct sbk_source_hot *h = &source_hot[i];
    if (h->pid == pid && h->address == r->address && h->pages == r->pages &&
        h->order) return h;
  }
  return NULL;
}
static struct sbk_source_hot *source_hot_cover(int pid,
                                               const struct sbk_rdma_region *r) {
  uint64_t end = r->address + r->pages * 4096;
  for (unsigned i = 0; i < source_hot_count; i++) {
    struct sbk_source_hot *h = &source_hot[i];
    if (h->pid == pid && h->order && h->address <= r->address &&
        end <= h->address + h->pages * 4096) return h;
  }
  return NULL;
}
static void release_hot_snapshots(void) {
  for (unsigned i = 0; i < hot_snapshot_count; i++) {
    free(hot_snapshots[i].addresses);
    free(hot_snapshots[i].runs);
  }
  free(hot_snapshots);
  hot_snapshots = NULL;
  hot_snapshot_count = 0;
}
static int prepare_hot_snapshots(void) {
  if (opts.sb_no_hot_first) return 0;
  if (hot_snapshots || list_length <= 0 || list_length > MAX_PROCESS)
    return -EINVAL;
  hot_snapshots = calloc(list_length, sizeof(*hot_snapshots));
  if (!hot_snapshots) return -ENOMEM;
  hot_snapshot_count = list_length;
  for (unsigned p = 0; p < hot_snapshot_count; p++) {
    struct sbk_hot_snapshot *h = &hot_snapshots[p];
    size_t capacity = 0;
    h->pid = pid_data_list[p].pid;
    for (int score = PRIORITY_QUEUE_LEVEL - 1; score >= 0; score--)
      for (volatile struct score_list *node = pid_data_list[p].dirtylist[score]; node; node = node->next) {
        if (h->count == capacity) {
          if (capacity > SIZE_MAX / sizeof(*h->addresses) / 2) {
            release_hot_snapshots(); return -EOVERFLOW;
          }
          size_t next = capacity ? capacity * 2 : 4096;
          uint64_t *addresses = realloc(h->addresses, next * sizeof(*addresses));
          if (!addresses) { release_hot_snapshots(); return -ENOMEM; }
          h->addresses = addresses;
          capacity = next;
        }
        h->addresses[h->count++] = node->addr;
      }
    int ret = sbk_hot_runs_prepare(h->addresses, h->count, &h->runs, &h->run_count);
    if (ret) { release_hot_snapshots(); return ret; }
    pr_info("SB_KERNEL hot_ps pid=%d pages=%zu runs=%zu strategy=%s\n",
            h->pid, h->count, h->run_count, h->run_count ? "merge" : "binary");
  }
  return 0;
}
static uint64_t kernel_now_ns(void) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return (uint64_t)now.tv_sec * 1000000000ULL + now.tv_nsec;
}

int sb_kernel_options(void) {
  if (!opts.sb_kernel_transfer)
    return (opts.sb_kernel_dma_mr || opts.sb_kernel_ps_arm || opts.sb_kernel_ps_mr || opts.sb_kernel_ps_mr_all) ? -EINVAL : 0;
  if (opts.sb_kernel_ps_mr_all && !opts.sb_kernel_ps_mr) return -EINVAL;
  if (opts.sb_kernel_ps_mr && (!opts.sb_kernel_ps_arm || opts.sb_kernel_dma_mr || opts.sb_kernel_dense)) {
    pr_err("K source PS MR requires PS ARM, sparse planning and ordinary MRs\n");
    return -EINVAL;
  }
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
  if (source && opts.sb_kernel_ps_mr && !(caps.features & SBK_FEATURE_REMOTE_PREARM)) {
    pr_err("SwiftBaton-K source PS MR capability unavailable\n");
    ret = -EOPNOTSUPP;
    goto fail;
  }
  if (opts.sb_kernel_dma_mr && !(caps.features & SBK_FEATURE_DMA_MR)) {
    pr_err("SwiftBaton-K DMA MR capability unavailable\n");
    ret = -EOPNOTSUPP;
    goto fail;
  }
  const unsigned prepared_features = SBK_FEATURE_PREPARED_ARM | SBK_FEATURE_UNBOUND_REGION;
  if (!source && opts.sb_kernel_ps_arm && (caps.features & prepared_features) != prepared_features) {
    pr_err("SwiftBaton-K PS ARM requires detached plan and unbound region capabilities\n");
    ret = -EOPNOTSUPP;
    goto fail;
  }
  if (ioctl(session_fd, SBK_IOC_RDMA_CREATE, &setup)) {
    ret = -errno;
    goto fail;
  }
  if (opts.sb_kernel_dma_mr && ioctl(session_fd, SBK_IOC_DMA_ENABLE)) {
    ret = -errno;
    goto fail;
  }
  setup.local.reserved[0] = opts.sb_kernel_dma_mr ? 1 : 0;
  setup.local.reserved[1] = opts.sb_kernel_ps_arm ? 1 : 0;
  if (sync_transfer(socket_fd, &setup.local, sizeof(setup.local), true) ||
      sync_transfer(socket_fd, &remote, sizeof(remote), false)) {
    ret = -EIO;
    goto fail;
  }
  if (!sbk_layout_mode_matches(opts.sb_kernel_dma_mr, opts.sb_kernel_ps_arm, &remote)) {
    pr_err("SwiftBaton-K DMA/PS ARM mode differs between peers\n");
    ret = -EPROTO;
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
    ret = sbk_catalog_prepare_workers(destination,
          opts.sb_kernel_catalog_workers ? opts.sb_kernel_catalog_workers : 1);
    if (ret) goto fail;
  }
  struct timeval deadline = {.tv_sec = SBK_SESSION_TIMEOUT_SEC};
  if (setsockopt(socket_fd, SOL_SOCKET, SO_RCVTIMEO, &deadline,
                 sizeof(deadline)) ||
      setsockopt(socket_fd, SOL_SOCKET, SO_SNDTIMEO, &deadline,
                 sizeof(deadline))) {
    ret = -errno;
    goto fail;
  }
  peer_fd = socket_fd;
  pr_info("SB_KERNEL ps_arm_mode role=%s enabled=%u\n", source ? "source" : "destination", opts.sb_kernel_ps_arm);
  pr_info("SB_KERNEL ps_mr_mode role=%s enabled=%u\n", source ? "source" : "destination", opts.sb_kernel_ps_mr);
  pr_info("SB_KERNEL ps_mr_all_mode role=%s enabled=%u\n", source ? "source" : "destination", opts.sb_kernel_ps_mr_all);
  pr_info("SB_KERNEL connected role=%s device=%s PF=%u FT=%u BG=%u "
          "pretransfer=%s dma_mr=%u\n",
          source ? "source" : "destination", setup.device, setup.slots[0],
          setup.slots[1], setup.slots[2],
          opts.sb_no_pretransfer ? "disabled" : "enabled", opts.sb_kernel_dma_mr);
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
  struct sbk_rdma_region *fallback = NULL;
  unsigned *fallback_index = NULL;
  unsigned fallback_count = 0, prearm_reused = 0, prearm_invalid = 0;
  unsigned prearm_no_snapshot = 0, prearm_pfn_changed = 0, prearm_notifier_changed = 0;
  unsigned hot_cached = 0, hot_derived = 0, hot_fallback = 0;
  struct sbk_pm_snapshot *pagemap = NULL;
  struct sbk_rdma_region *scan_input = NULL, *trusted_regions = NULL, *scan_plan = NULL;
  unsigned scan_input_count = 0, trusted_count = 0, scan_plan_count = 0;
  bool fast_sparse = false;
  uint64_t begin = kernel_now_ns(), locked = 0, scan_ns = 0, pagemap_ns = 0, plan_ns = 0, validate_ns = 0, export_ns = 0, hot_ns = 0, stage;
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
      struct sbk_sparse_stats stats = {};
      uint64_t virtual_pages = 0, trusted_pages = 0;
      stage = kernel_now_ns();
      fast_sparse = opts.sb_kernel_ps_mr &&
          source_partition_prearmed(pid, regions, count, capacity,
                                    &scan_input, &scan_input_count,
                                    &trusted_regions, &trusted_count,
                                    &virtual_pages);
      if (fast_sparse) {
        if (scan_input_count) {
          ret = sbk_pm_snapshot_create(pid, scan_input, scan_input_count,
                     opts.sb_validation_workers ? opts.sb_validation_workers : 1, &pagemap);
          pagemap_ns = kernel_now_ns() - stage;
          if (!ret) ret = sbk_sparse_plan_snapshot(pid, scan_input, scan_input_count,
                      capacity - trusted_count, &scan_plan, &scan_plan_count,
                      &stats, pagemap);
          if (ret) goto out;
          if (stats.control_page) {
            free(scan_plan);
            scan_plan = NULL;
            scan_plan_count = 0;
            stats.registered_pages = 0;
            stats.control_page = 0;
          }
        } else pagemap_ns = kernel_now_ns() - stage;
        sparse = calloc(capacity, sizeof(*sparse));
        if (!sparse) { ret = -ENOMEM; goto out; }
        unsigned a = 0, b = 0, used = 0;
        uint64_t last = 0;
        while (a < trusted_count || b < scan_plan_count) {
          const struct sbk_rdma_region *next;
          if (b == scan_plan_count ||
              (a < trusted_count && trusted_regions[a].address < scan_plan[b].address))
            next = &trusted_regions[a++];
          else next = &scan_plan[b++];
          if (used && next->address < last) { fast_sparse = false; break; }
          sparse[used++] = *next;
          last = next->address + next->pages * 4096;
        }
        unsigned chunk_pages = opts.sb_kernel_export_chunk_mb * 256;
        uint64_t needed = 0;
        if (chunk_pages) for (unsigned i = 0; i < used; i++)
          needed += (sparse[i].pages + chunk_pages - 1) / chunk_pages;
        if (used > capacity || (chunk_pages && needed > capacity)) fast_sparse = false;
        if (fast_sparse) {
          for (unsigned i = 0; i < trusted_count; i++) trusted_pages += trusted_regions[i].pages;
          stats.virtual_pages = virtual_pages;
          stats.data_pages += trusted_pages;
          stats.registered_pages += trusted_pages;
          count = used;
          regions = sparse;
        } else {
          free(sparse); sparse = NULL;
          free(scan_plan); scan_plan = NULL;
          sbk_pm_snapshot_free(pagemap); pagemap = NULL;
          pagemap_ns = 0;
        }
      }
      if (!fast_sparse) {
        ret = sbk_pm_snapshot_create(pid, regions, count,
                   opts.sb_validation_workers ? opts.sb_validation_workers : 1, &pagemap);
        pagemap_ns = kernel_now_ns() - stage;
        if (!ret) ret = sbk_sparse_plan_snapshot(pid, regions, count, capacity,
                                                  &sparse, &count, &stats, pagemap);
        if (ret) goto out;
        regions = sparse;
      }
      scan_ns = kernel_now_ns() - stage;
      plan_ns = scan_ns - pagemap_ns;
      pr_info("SB_KERNEL sparse pid=%d virtual=%llu data=%llu registered=%llu skipped=%llu regions=%u control_page=%u\n",
              pid, (unsigned long long)stats.virtual_pages, (unsigned long long)stats.data_pages,
              (unsigned long long)stats.registered_pages,
              (unsigned long long)(stats.virtual_pages - stats.registered_pages), count, stats.control_page);
      pr_info("SB_KERNEL sparse_prearm pid=%d enabled=%u trusted=%u trusted_pages=%llu residual=%u residual_pages=%llu\n",
              pid, fast_sparse, trusted_count, (unsigned long long)trusted_pages,
              scan_input_count, (unsigned long long)(virtual_pages - trusted_pages));
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
    const uint64_t *observed = pagemap ? sbk_pm_snapshot_find(pagemap, regions[i].address, regions[i].pages) : NULL;
    bool trusted_exact = fast_sparse && source_trusted_exact(trusted_regions, trusted_count, &regions[i]);
    if ((pagemap && !observed && !trusted_exact) ||
        (!opts.sb_kernel_ps_mr_all && trusted_exact &&
         sb_kernel_ps_has_candidates(pid, regions[i].address, regions[i].pages))) {
      ret = -ERANGE;
      goto out;
    }
    ret = sb_kernel_ps_validate_snapshot(pid, &regions[i], observed, &dirty, &f->dirty_count);
    if (ret)
      goto out;
    f->dirty = dirty;
  }
  validate_ns = kernel_now_ns() - stage;
  stage = kernel_now_ns();
  if (opts.sb_kernel_ps_mr && source_prearm_count) {
    fallback = calloc(count, sizeof(*fallback));
    fallback_index = calloc(count, sizeof(*fallback_index));
    if (!fallback || !fallback_index) { ret = -ENOMEM; goto out; }
    for (unsigned i = 0; i < count; i++) {
      struct sbk_catalog_final *f = &final_regions[base + i];
      struct sbk_source_prearm *p = (!opts.sb_kernel_ps_mr_all && f->dirty_count) ?
          NULL : source_prearm_find(pid, &regions[i]);
      const uint64_t *observed = p && pagemap ?
          sbk_pm_snapshot_find(pagemap, regions[i].address, regions[i].pages) : NULL;
      bool trusted_exact = fast_sparse && source_trusted_exact(trusted_regions, trusted_count, &regions[i]);
      struct sbk_prearm_status status = p ? (struct sbk_prearm_status){.region = p->region} :
                                               (struct sbk_prearm_status){};
      bool pfn_ok = p && ((observed && source_pfns_equal(p->pfns, observed, regions[i].pages)) ||
                          (!observed && trusted_exact));
      bool notifier_ok = pfn_ok && !ioctl(session_fd, SBK_IOC_PREARM_STATUS, &status) && status.valid;
      if (notifier_ok) {
        regions[i] = p->region;
        prearm_reused++;
      } else {
        if (p) {
          prearm_invalid++;
          if (!observed && !trusted_exact) prearm_no_snapshot++;
          else if (!pfn_ok) prearm_pfn_changed++;
          else prearm_notifier_changed++;
        }
        fallback_index[fallback_count] = i;
        fallback[fallback_count++] = regions[i];
      }
    }
  } else {
    fallback = regions;
    fallback_count = count;
  }
  if (fallback_count)
    ret = sb_kernel_export_seized(ctl, session_fd, fallback, fallback_count,
                                  opts.sb_kernel_export_workers, &export_peak);
  if (!ret && fallback != regions)
    for (unsigned i = 0; i < fallback_count; i++) regions[fallback_index[i]] = fallback[i];
  export_ns = kernel_now_ns() - stage;
  if (ret)
    goto out;
  if (fast_sparse) for (unsigned i = 0; i < trusted_count; i++) {
    struct sbk_source_prearm *p = source_prearm_find(pid, &trusted_regions[i]);
    struct sbk_prearm_status status = p ?
        (struct sbk_prearm_status){.region = p->region} : (struct sbk_prearm_status){};
    if (!p || ioctl(session_fd, SBK_IOC_PREARM_STATUS, &status) || !status.valid) {
      /* The trusted sparse plan omitted these pagemap reads. Do not publish
       * that plan after any intervening map invalidation. */
      ret = -EAGAIN;
      goto out;
    }
  }
  stage = kernel_now_ns();
  for (unsigned int i = 0; i < count; i++) {
    struct sbk_catalog_final *f = &final_regions[base + i];
    f->record.remote = regions[i];
    if (opts.sb_no_hot_first)
      continue;
    struct sbk_source_hot *prepared = source_hot_find(pid, &regions[i]);
    if (prepared) {
      f->hot = prepared->order;
      f->hot_count = prepared->count;
      prepared->order = NULL; /* Final record owns this allocation now. */
      hot_cached++;
      continue;
    }
    struct sbk_source_hot *cover = source_hot_cover(pid, &regions[i]);
    if (cover) {
      uint64_t *hot = malloc(regions[i].pages * sizeof(*hot));
      if (!hot) {
        ret = -ENOMEM;
        goto out;
      }
      f->hot = hot;
      uint64_t offset = (regions[i].address - cover->address) / 4096;
      for (size_t j = 0; j < cover->count; j++) {
        uint64_t source_index = cover->order[j];
        if (source_index >= offset && source_index - offset < regions[i].pages)
          hot[f->hot_count++] = source_index - offset;
      }
      hot_derived++;
      continue;
    }
    /* A range absent from the PS layout has no prepared heat order. It is
     * still installed by ordinary K background/demand paths; an empty hint
     * avoids a frozen-stage scan of the entire sampled heat list. */
    hot_fallback++;
  }
  hot_ns = kernel_now_ns() - stage;
out:
  pr_info("SB_KERNEL final_export pid=%d workers=%u chunk_mb=%u effective_pages=%u peak=%u regions=%u result=%d\n",
          pid, opts.sb_kernel_export_workers, opts.sb_kernel_export_chunk_mb,
          export_span, export_peak, count, ret);
  pr_info("SB_KERNEL final_prearm pid=%d enabled=%u reused=%u fallback=%u invalid=%u result=%d\n",
          pid, opts.sb_kernel_ps_mr, prearm_reused, fallback_count, prearm_invalid, ret);
  pr_info("SB_KERNEL final_prearm_reason pid=%d no_snapshot=%u pfn_changed=%u notifier_changed=%u\n",
          pid, prearm_no_snapshot, prearm_pfn_changed, prearm_notifier_changed);
  pr_info("SB_KERNEL final_hot_cache pid=%d cached=%u derived=%u fallback=%u result=%d\n",
          pid, hot_cached, hot_derived, hot_fallback, ret);
  pr_info("SB_KERNEL final_scan pid=%d pagemap_us=%llu planning_us=%llu result=%d\n",
          pid, (unsigned long long)(pagemap_ns / 1000),
          (unsigned long long)(plan_ns / 1000), ret);
  pr_info("SB_KERNEL final_prepare pid=%d regions=%u result=%d lock_wait_us=%llu scan_us=%llu validation_us=%llu export_us=%llu hot_us=%llu total_us=%llu\n",
          pid, count, ret, (unsigned long long)((locked - begin) / 1000),
          (unsigned long long)(scan_ns / 1000), (unsigned long long)(validate_ns / 1000),
          (unsigned long long)(export_ns / 1000), (unsigned long long)(hot_ns / 1000),
          (unsigned long long)((kernel_now_ns() - begin) / 1000));
  if (fallback != regions) free(fallback);
  free(fallback_index);
  free(sparse);
  free(scan_input);
  free(scan_plan);
  free(trusted_regions);
  sbk_pm_snapshot_free(pagemap);
  if (reservation_locked) pthread_mutex_unlock(&final_gate.lock);
  sbk_final_release(&final_gate, ret);
  return ret;
}
/* PS snapshots the running source, leaving original pages unpinned. The
 * receiver caches pages in kernel memory, without publishing any PTE. */
/* Capacity hint only: virtual size may overestimate lazy pages. Dynamic maps,
 * sparse holes, and the cap never alter final coverage or page correctness. */
static uint64_t token_pool_hint(void) {
  uint64_t pages = 0;
  for (int i = 0; i < list_length; i++) {
    char path[64]; unsigned long long size;
    snprintf(path, sizeof(path), "/proc/%d/statm", pid_data_list[i].pid);
    FILE *file = fopen(path, "re");
    if (!file) continue;
    int found = fscanf(file, "%llu", &size);
    fclose(file);
    if (found != 1) continue;
    if (size >= SBK_TOKEN_POOL_MAX_PAGES - pages) return SBK_TOKEN_POOL_MAX_PAGES;
    pages += size;
  }
  return pages;
}
struct dma_phase_stats { uint64_t pages, ns; unsigned regions; };
static int dma_record(int socket_fd, const struct sbk_rdma_region *region, bool sending,
                      struct dma_phase_stats *stats) {
  uint64_t begin = kernel_now_ns();
  int ret = sbk_dma_wire_transfer(socket_fd, session_fd, region, sending,
                                 opts.sb_kernel_dma_mr, sync_transfer);
  if (!ret && opts.sb_kernel_dma_mr) {
    stats->regions++; stats->pages += region->pages; stats->ns += kernel_now_ns() - begin;
  }
  return ret;
}
static void report_dma_phase(const char *phase, bool sending, const struct dma_phase_stats *stats) {
  if (opts.sb_kernel_dma_mr)
    pr_info("SB_KERNEL dma_maps phase=%s role=%s regions=%u pages=%llu bytes=%llu elapsed_us=%llu\n",
            phase, sending ? "source" : "destination", stats->regions,
            (unsigned long long)stats->pages, (unsigned long long)(stats->pages * sizeof(uint64_t)),
            (unsigned long long)(stats->ns / 1000));
}
static int send_ps_layout(int socket_fd) {
  if (!opts.sb_kernel_ps_arm) return 0;
  if (list_length < 0 || list_length > 4096) return -EINVAL;
  uint32_t *pids = calloc(list_length ? list_length : 1, sizeof(*pids));
  struct sbk_catalog_layout *layout = NULL;
  struct sbk_layout_stats stats;
  unsigned count = 0;
  uint64_t begin = kernel_now_ns();
  if (!pids) return -ENOMEM;
  for (int i = 0; i < list_length; i++) pids[i] = pid_data_list[i].pid;
  unsigned chunk_pages = (opts.sb_kernel_export_chunk_mb ? opts.sb_kernel_export_chunk_mb : 4096) * 256;
  int ret = sbk_layout_collect(pids, list_length, chunk_pages, &layout, &count, &stats);
  free(pids);
  if (!ret) ret = sbk_layout_wire_transfer(socket_fd, true, &layout, &count, sync_transfer);
  if (!ret) pr_info("SB_KERNEL ps_layout role=source regions=%u pages=%llu pids=%u skipped_pids=%u skipped_ranges=%u elapsed_us=%llu\n",
      count, (unsigned long long)stats.pages, stats.pids, stats.skipped_pids, stats.skipped_ranges,
      (unsigned long long)((kernel_now_ns() - begin) / 1000));
  if (!ret && opts.sb_kernel_ps_mr) {
    source_layout = layout;
    source_layout_count = count;
    layout = NULL;
  }
  free(layout);
  return ret;
}
/* PS pre-registration is a hint. Writable GUP may invalidate copied PS candidates
 * through soft-dirty; final validation marks each such page dirty. The final frozen
 * plan plus exact PFNs and notifier status decide every actual reuse. */
static void source_prearm_ps(void) {
  if (!opts.sb_kernel_ps_mr || !source_layout_count || session_fd < 0) return;
  source_prearm = calloc(source_layout_count, sizeof(*source_prearm));
  if (!source_prearm) return;
  unsigned eligible = 0, registered = 0, valid = 0, skipped_ps = 0;
  unsigned failures[PREARM_CAPTURE_NOTIFIER + 1] = {};
  unsigned retried = 0, retry_valid = 0;
  uint64_t start = kernel_now_ns();
  for (unsigned cursor = 0; cursor < source_layout_count;) {
    struct sbk_prearm_batch request = {};
    unsigned slots[SBK_MAX_BATCH], n = 0;
    int pid = source_layout[cursor].source_pid;
    request.pid = pid;
    request.batch.workers = opts.sb_kernel_export_workers;
    while (cursor < source_layout_count &&
           source_layout[cursor].source_pid == (uint32_t)pid && n < SBK_MAX_BATCH) {
      const struct sbk_catalog_layout *l = &source_layout[cursor++];
      if (!opts.sb_kernel_ps_mr_all &&
          sb_kernel_ps_has_candidates(pid, l->address, l->pages)) {
        skipped_ps++;
        continue;
      }
      /* In all-range mode, final validation invalidates any copied PS page
       * marked soft-dirty by the writable GUP before reusing this MR. */
      slots[n] = source_prearm_count;
      source_prearm[source_prearm_count++] = (struct sbk_source_prearm){
          .pid = pid, .region = {.address = l->address, .pages = l->pages}};
      request.batch.regions[n] = source_prearm[slots[n]].region;
      n++;
    }
    if (!n) continue;
    eligible += n;
    request.batch.count = n;
    if (ioctl(session_fd, SBK_IOC_PREARM_BATCH, &request)) {
      pr_warn("SB_KERNEL source_prearm pid=%d batch=%u ioctl=%d fallback=1\n",
              pid, n, errno);
      continue; /* The session still owns any successful partial MRs. */
    }
    registered += n;
    for (unsigned i = 0; i < n; i++) {
      struct sbk_source_prearm *p = &source_prearm[slots[i]];
      p->region = request.batch.regions[i];
      enum source_prearm_failure reason = source_prearm_capture(p);
      failures[reason]++;
      if (reason == PREARM_CAPTURE_NOTIFIER) {
        /* A writable GUP can COW its own first registration and invalidate
         * the notifier. Keep that old MR owned until revoke, and register a
         * fresh one under a new notifier now that COW has settled. Only the
         * fresh descriptor and freshly observed PFNs may be reused later. */
        struct sbk_prearm_batch retry = {.pid = pid,
                                         .batch = {.count = 1, .workers = 1}};
        retry.batch.regions[0] = (struct sbk_rdma_region){
            .address = p->region.address, .pages = p->region.pages};
        retried++;
        if (!ioctl(session_fd, SBK_IOC_PREARM_BATCH, &retry)) {
          p->region = retry.batch.regions[0];
          reason = source_prearm_capture(p);
          if (reason == PREARM_CAPTURE_OK) retry_valid++;
        }
      }
      if (p->pfns) valid++;
    }
  }
  pr_info("SB_KERNEL source_prearm_ps layout=%u eligible=%u registered=%u valid=%u skipped_ps=%u elapsed_us=%llu\n",
          source_layout_count, eligible, registered, valid, skipped_ps,
          (unsigned long long)((kernel_now_ns() - start) / 1000));
  pr_info("SB_KERNEL source_prearm_retry alloc=%u pagemap=%u pfn=%u notifier=%u retried=%u recovered=%u\n",
          failures[PREARM_CAPTURE_ALLOC], failures[PREARM_CAPTURE_PAGEMAP],
          failures[PREARM_CAPTURE_PFN], failures[PREARM_CAPTURE_NOTIFIER],
          retried, retry_valid);
}
/* Heat order is only a scheduling hint. Build it from the PS layout while
 * Redis still serves requests. Exact final ranges adopt these arrays; a
 * changed shape derives its subset or uses the original frozen fallback. */
static void source_hot_prepare(void) {
  if (!opts.sb_kernel_ps_mr || opts.sb_no_hot_first || !source_layout_count ||
      source_hot) return;
  uint64_t begin = kernel_now_ns();
  int ret = 0;
  source_hot = calloc(source_layout_count, sizeof(*source_hot));
  if (!source_hot) return;
  source_hot_count = source_layout_count;
  for (unsigned first = 0; first < source_layout_count;) {
    unsigned end = first + 1;
    int pid = source_layout[first].source_pid;
    while (end < source_layout_count && source_layout[end].source_pid == (uint32_t)pid)
      end++;
    struct sbk_hot_range *index = calloc(end - first, sizeof(*index));
    if (!index) { ret = -ENOMEM; break; }
    for (unsigned i = first; i < end; i++) {
      const struct sbk_catalog_layout *l = &source_layout[i];
      struct sbk_source_hot *h = &source_hot[i];
      h->pid = pid;
      h->address = l->address;
      h->pages = l->pages;
      h->order = malloc(l->pages * sizeof(*h->order));
      if (!h->order) { ret = -ENOMEM; break; }
      index[i - first] = (struct sbk_hot_range){
          .address = h->address, .pages = h->pages,
          .order = h->order, .used = &h->count};
    }
    if (!ret) ret = sbk_hot_index_prepare(index, end - first);
    if (!ret) for (unsigned p = 0; p < hot_snapshot_count; p++) {
      const struct sbk_hot_snapshot *h = &hot_snapshots[p];
      if (h->pid != pid) continue;
      ret = sbk_hot_index_append_runs(index, end - first, h->addresses,
                                      h->count, h->runs, h->run_count);
      if (ret) break;
    }
    free(index);
    if (ret) break;
    first = end;
  }
  if (ret) {
    for (unsigned i = 0; i < source_hot_count; i++) free(source_hot[i].order);
    free(source_hot);
    source_hot = NULL;
    source_hot_count = 0;
  }
  pr_info("SB_KERNEL source_hot_prepare layout=%u cached=%u elapsed_us=%llu result=%d\n",
          source_layout_count, source_hot_count,
          (unsigned long long)((kernel_now_ns() - begin) / 1000), ret);
}
static int receive_ps_layout(int socket_fd) {
  if (!opts.sb_kernel_ps_arm) return 0;
  struct sbk_catalog_layout *layout = NULL;
  unsigned count = 0;
  uint64_t begin = kernel_now_ns();
  int ret = sbk_layout_wire_transfer(socket_fd, false, &layout, &count, sync_transfer);
  if (!ret) ret = sbk_catalog_prepare_layout(destination, layout, count);
  if (!ret) {
    struct sbk_catalog_timing timing = {};
    sbk_catalog_get_timing(destination, &timing);
    pr_info("SB_KERNEL ps_layout role=destination regions=%u pages=%llu elapsed_us=%llu\n",
        count, (unsigned long long)timing.ps_plan_pages,
        (unsigned long long)((kernel_now_ns() - begin) / 1000));
  }
  free(layout);
  return ret;
}
int sb_kernel_send_ps(int socket_fd) {
  struct dma_phase_stats dma = {};
  unsigned count = 0, sent = 0, skipped = 0;
  uint64_t pages = 0, begin = kernel_now_ns(), first = 0;
  int ret = 0;
  sb_trace("kernel.hot_ps_begin");
  ret = prepare_hot_snapshots();
  if (ret) return ret;
  sb_trace("kernel.hot_ps_done");
  if (!opts.sb_no_pretransfer) {
    sb_trace("kernel.ps_register_begin");
    ret = sb_kernel_ps_begin(session_fd, &count);
    if (ret) {
      pr_err("K PS preparation failed: %d\n", ret);
      return ret;
    }
  }
  /* Version 4 adds a bounded resource hint before the existing PS records.
   * Version 5 additionally carries immutable per-region device DMA vectors. */
  struct sbk_wire_header h = {SBK_WIRE_MAGIC, sbk_layout_ps_version(opts.sb_kernel_dma_mr, opts.sb_kernel_ps_arm), count, 1}, ack;
  uint64_t tokens = token_pool_hint();
  if (sync_transfer(socket_fd, &h, sizeof(h), true) ||
      sync_transfer(socket_fd, &tokens, sizeof(tokens), true)) { ret = -EIO; goto out; }
  ret = send_ps_layout(socket_fd);
  if (ret) goto out;
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
    if (p->count && (ret = dma_record(socket_fd, &w.record.remote, true, &dma))) goto out;
    if (!first) first = kernel_now_ns();
  }
  report_dma_phase("ps", true, &dma);
  ret = sb_kernel_ps_finish(false);
  sb_trace("kernel.ps_register_done");
  if (ret) return ret;
  if (sync_transfer(socket_fd, &ack, sizeof(ack), false) || memcmp(&h, &ack, sizeof(h)))
    return -EIO;
  source_prearm_ps();
  source_hot_prepare();
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
  struct dma_phase_stats dma = {};
  struct sbk_wire_header h;
  struct sbk_work_pool *pool = NULL;
  struct sbk_work_stats stats = {};
  unsigned workers = opts.sb_precopy_workers ? opts.sb_precopy_workers : 4;
  uint64_t pages = 0, begin = kernel_now_ns();
  unsigned skipped = 0;
  int ret = -EPROTO;
  if (!destination || sync_transfer(socket_fd, &h, sizeof(h), false) ||
      h.magic != SBK_WIRE_MAGIC || !sbk_layout_ps_version_valid(opts.sb_kernel_dma_mr, opts.sb_kernel_ps_arm, h.version) || h.reserved != 1 ||
      h.count > SBK_MAX_REGIONS / 2 || (opts.sb_no_pretransfer && h.count))
    return -EPROTO;
  if (h.version >= 4) {
    uint64_t tokens;
    struct sbk_capabilities caps;
    if (sync_transfer(socket_fd, &tokens, sizeof(tokens), false) ||
        tokens > SBK_TOKEN_POOL_MAX_PAGES) return -EPROTO;
    if (ioctl(session_fd, SBK_IOC_CAPABILITIES, &caps)) return -errno;
    uint64_t started = kernel_now_ns();
    bool supported = !!(caps.features & SBK_FEATURE_TOKEN_POOL);
    if (tokens && supported && ioctl(session_fd, SBK_IOC_TOKEN_RESERVE, &tokens))
      return -errno;
    pr_info("SB_KERNEL token_pool_ps pages=%llu supported=%u elapsed_us=%llu\n",
            (unsigned long long)tokens, supported,
            (unsigned long long)((kernel_now_ns() - started) / 1000));
  }
  ret = receive_ps_layout(socket_fd);
  if (ret) goto fail;
  if (h.count) {
    if (workers > h.count) workers = h.count;
    pool = sbk_work_create(workers, workers * 2, ps_receive_run, free);
    if (!pool) return -errno;
  }
  for (unsigned i = 0; i < h.count; i++) {
    struct sbk_wire_region w, empty = {};
    if (sync_transfer(socket_fd, &w, sizeof(w), false)) { ret = -EIO; goto fail; }
    if (h.version >= 3 && !memcmp(&w, &empty, sizeof(w))) { skipped++; continue; }
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
    else {
      ret = dma_record(socket_fd, &w.record.remote, false, &dma);
      if (!ret) ret = sbk_work_submit(pool, job);
    }
    if (ret) { free(job); goto fail; }
    pages += w.hot_count;
  }
  report_dma_phase("ps", false, &dma);
  ret = sbk_work_finish(pool, false, &stats);
  if (ret) return ret;
  if (opts.sb_kernel_ps_mr) {
    uint64_t import_begin = kernel_now_ns();
    ret = sbk_catalog_import_ps_early(destination);
    pr_info("SB_KERNEL ps_import_early elapsed_us=%llu result=%d\n",
            (unsigned long long)((kernel_now_ns() - import_begin) / 1000), ret);
    if (ret) return ret;
  }
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
  struct dma_phase_stats dma = {};
  int ret = sbk_final_seal(&final_gate);
  if (ret) return ret;
  struct sbk_wire_header h = {SBK_WIRE_MAGIC, sbk_dma_final_version(opts.sb_kernel_dma_mr), nr_final, 0};
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
    int map_ret = dma_record(socket_fd, &w.record.remote, true, &dma);
    if (map_ret) { ret = map_ret; goto out; }
  }
  report_dma_phase("final", true, &dma);
  ret = 0;
out:
  pthread_mutex_unlock(&final_gate.lock);
  return ret;
}
int sb_kernel_client_receive(int socket_fd) {
  struct dma_phase_stats dma = {};
  struct sbk_wire_header h;
  int ret = -EPROTO;
  if (!destination || sync_transfer(socket_fd, &h, sizeof(h), false) ||
      h.magic != SBK_WIRE_MAGIC || h.version != sbk_dma_final_version(opts.sb_kernel_dma_mr) || h.reserved || !h.count ||
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
    int map_ret = dma_record(socket_fd, &w.record.remote, false, &dma);
    if (map_ret) { ret = map_ret; goto out; }
  }
  report_dma_phase("final", false, &dma);
  ret = sbk_catalog_seal(destination, final_regions, nr_final);
  struct sbk_catalog_timing timing = {0};
  sbk_catalog_get_timing(destination, &timing);
  if (opts.sb_kernel_ps_arm)
    pr_info("SB_KERNEL ps_arm_reuse prepared=%u prepared_pages=%llu reused=%u reused_pages=%llu discarded=%u discarded_pages=%llu final=%u result=%d\n",
        timing.ps_plans, (unsigned long long)timing.ps_plan_pages,
        timing.reused_plans, (unsigned long long)timing.reused_plan_pages,
        timing.discarded_plans, (unsigned long long)timing.discarded_plan_pages, nr_final, ret);
  pr_info("SB_KERNEL catalog_allocation started=%u peak=%u launch_us=%llu sum_us=%llu max_us=%llu\n",
          timing.workers_started, timing.allocation_peak,
          (unsigned long long)(timing.launch_ns / 1000),
          (unsigned long long)(timing.allocation_sum_ns / 1000),
          (unsigned long long)(timing.allocation_max_ns / 1000));
  pr_info("SB_KERNEL catalog_seal import_started=%u import_peak=%u seal_started=%u seal_peak=%u import_us=%llu io_us=%llu\n",
          timing.import_workers_started, timing.import_peak,
          timing.seal_workers_started, timing.seal_peak,
          (unsigned long long)(timing.seal_import_ns / 1000),
          (unsigned long long)(timing.seal_io_ns / 1000));
  pr_info("SB_KERNEL final_catalog regions=%u workers=%u validation_us=%llu prepare_us=%llu seal_us=%llu result=%d\n",
          nr_final, opts.sb_kernel_catalog_workers ? opts.sb_kernel_catalog_workers : 1,
          (unsigned long long)(timing.validate_ns / 1000),
          (unsigned long long)(timing.prepare_ns / 1000),
          (unsigned long long)(timing.seal_ns / 1000), ret);
out:
  return ret;
}
int sb_kernel_source_finish(void) {
  struct sbk_wire_header ack;
  int ret = 0;
  if (session_fd < 0 || peer_fd < 0 ||
      sync_transfer(peer_fd, &ack, sizeof(ack), false) ||
      ack.magic != SBK_WIRE_MAGIC || ack.version != sbk_dma_final_version(opts.sb_kernel_dma_mr) ||
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
static void report_catalog_deficit(const struct sbk_catalog_audit *a) {
  pr_info("SB_KERNEL deficit pid=%u address=%llx pages=%llu completed=%llu retired=%llu drained=%u "
          "PF=%llu FT=%llu BG=%llu PS=%llu invalid=%llu state0=%llu state1=%llu state2=%llu state3=%llu state4=%llu state5=%llu state6=%llu unknown=%llu\n",
          a->record.restore_pid, (unsigned long long)a->record.address,
          (unsigned long long)a->stats.pages, (unsigned long long)a->stats.completed,
          (unsigned long long)a->drain.retired_tokens, a->drain.drained,
          (unsigned long long)a->stats.fetched[0], (unsigned long long)a->stats.fetched[1],
          (unsigned long long)a->stats.fetched[2], (unsigned long long)a->stats.pretransferred,
          (unsigned long long)a->stats.invalidated,
          (unsigned long long)a->states[0], (unsigned long long)a->states[1],
          (unsigned long long)a->states[2], (unsigned long long)a->states[3],
          (unsigned long long)a->states[4], (unsigned long long)a->states[5],
          (unsigned long long)a->states[6], (unsigned long long)a->states[7]);
}
int sb_kernel_client_serve(int listen_fd, int socket_fd) {
  int client = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC), ret = -EIO;
  unsigned processes = 0, served = 0;
  bool finished = false, drained = false;
  struct sbk_wire_header ack = {SBK_WIRE_MAGIC, sbk_dma_final_version(opts.sb_kernel_dma_mr), nr_final, 0};
  struct sbk_stats st;
  close(listen_fd);
  if (client < 0)
    return -errno;
  struct timeval deadline = {.tv_sec = SBK_SESSION_TIMEOUT_SEC};
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
    if (now.tv_sec - start.tv_sec > SBK_SESSION_TIMEOUT_SEC) {
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
  uint64_t retired_unfetched = 0;
  if (st.invalidated > st.pretransferred) { ret = -EPROTO; goto out; }
  uint64_t accounted = st.pretransferred - st.invalidated;
  for (unsigned lane = 0; lane < SBK_LANES; lane++) accounted += st.fetched[lane];
  if (st.completed != st.pages || accounted != st.pages) {
    int audit = sbk_catalog_audit_deficits(destination, report_catalog_deficit,
                                           &retired_unfetched);
    pr_info("SB_KERNEL deficit_audit result=%d completed=%llu pages=%llu retired_unfetched=%llu\n",
            audit, (unsigned long long)st.completed, (unsigned long long)st.pages,
            (unsigned long long)retired_unfetched);
    if (audit || st.completed + retired_unfetched != st.pages ||
        accounted + retired_unfetched != st.pages) { ret = -EPROTO; goto out; }
  }
  pr_info("SB_KERNEL complete regions=%u pages=%llu PF=%llu FT=%llu BG=%llu "
          "errors=%llu fault_ns=%llu fault_max_ns=%llu PS=%llu invalid=%llu "
          "faults=%llu hits=%llu waits=%llu ahead=%llu skipped=%llu batches=%llu "
          "retired_unfetched=%llu\n",
          nr_final, (unsigned long long)st.pages,
          (unsigned long long)st.fetched[0], (unsigned long long)st.fetched[1],
          (unsigned long long)st.fetched[2], (unsigned long long)st.errors,
          (unsigned long long)st.fault_ns, (unsigned long long)st.fault_max_ns,
          (unsigned long long)st.pretransferred,
          (unsigned long long)st.invalidated, (unsigned long long)st.faults,
          (unsigned long long)st.hits, (unsigned long long)st.waits,
          (unsigned long long)st.installed_ahead,
          (unsigned long long)st.skipped_install, (unsigned long long)st.batches,
          (unsigned long long)retired_unfetched);
  struct sbk_token_pool_stats tokens;
  if (!ioctl(session_fd, SBK_IOC_TOKEN_POOL_STATS, &tokens)) {
    struct sbk_catalog_timing catalog_timing = {};
    uint64_t discarded_pages = 0;
    if (opts.sb_kernel_ps_arm) {
      sbk_catalog_get_timing(destination, &catalog_timing);
      discarded_pages = catalog_timing.discarded_plan_pages;
    }
    pr_info("SB_KERNEL token_pool_complete prepared=%llu claimed=%llu available=%llu fallback=%llu sealed=%u\n",
            (unsigned long long)tokens.prepared, (unsigned long long)tokens.claimed,
            (unsigned long long)tokens.available, (unsigned long long)tokens.fallback, tokens.sealed);
    if (discarded_pages)
      pr_info("SB_KERNEL token_pool_discarded_ps_pages=%llu final_pages=%llu\n",
              (unsigned long long)discarded_pages, (unsigned long long)st.pages);
    if (tokens.prepared != tokens.claimed + tokens.available ||
        tokens.claimed + tokens.fallback != st.pages + discarded_pages) {
      ret = -EPROTO; goto out;
    }
  } else if (errno != ENOTTY && errno != EOPNOTSUPP) { ret = -errno; goto out; }
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
  for (unsigned i = 0; i < source_prearm_count; i++) free(source_prearm[i].pfns);
  free(source_prearm);
  source_prearm = NULL;
  source_prearm_count = 0;
  for (unsigned i = 0; i < source_hot_count; i++) free(source_hot[i].order);
  free(source_hot);
  source_hot = NULL;
  source_hot_count = 0;
  free(source_layout);
  source_layout = NULL;
  source_layout_count = 0;
  release_hot_snapshots();
  free(final_regions);
  final_regions = NULL;
  nr_final = 0;
  if (session_fd >= 0)
    close(session_fd);
  session_fd = -1;
  peer_fd = -1;
  pthread_mutex_unlock(&final_gate.lock);
}
