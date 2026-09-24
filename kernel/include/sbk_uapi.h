/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
#ifndef SWIFTBATON_K_UAPI_H
#define SWIFTBATON_K_UAPI_H
#include <linux/types.h>
#include <linux/ioctl.h>

#define SBK_ABI_VERSION 1
#define SBK_BACKEND_LOOPBACK_TEST 1
#define SBK_BACKEND_RDMA 2
#define SBK_LANES 3
#define SBK_DEMAND 0
#define SBK_PREFETCH 1
#define SBK_BACKGROUND 2
#define SBK_PRETRANSFER 3 /* Shares background QPs, runs before the mapping is exposed. */
#define SBK_MAX_BATCH 32
#define SBK_NO_FAILURE (~(__u64)0)
#define SBK_RDMA_MAX_SLOTS 8
#define SBK_RDMA_SOURCE 1
#define SBK_RDMA_DESTINATION 2
#define SBK_MAX_REGIONS 8192

/* Loopback is an explicit VM correctness-test transport, not RDMA evidence. */
struct sbk_config {
    __u32 version, backend;
    __u64 source_address, pages;
    __u32 prefetch_workers, background_workers, batch_pages, prefetch_enabled;
    __u64 test_fail_page;
    __u32 test_delay_us, reserved;
};
struct sbk_hot_list { __u64 indices, count; };
struct sbk_anon_arm { __u64 address; };
/* A catalog session uses RDMA_CREATE.pages=0 and source_address=0. Regions are
 * registered from their owning mm, then exchanged over the control channel.
 * Their bytes must be immutable after the final PS dirty/PFN validation. */
struct sbk_rdma_region { __u64 address, pages; __u32 rkey, id; };
/* A synchronous owner-mm batch. Input completed/peak must be zero. Output
 * completed counts successful descriptors, NOT a contiguous prefix. On ANY
 * error abort the source session: all pinned MRs remain owned by that session,
 * including descriptors lost to copyout errors. IDs need not follow array order.
 * Every helper has exited and detached from the owner's mm before return. */
struct sbk_export_batch {
    __u32 count, workers, completed, peak;
    struct sbk_rdma_region regions[SBK_MAX_BATCH];
};
struct sbk_region_bind {
    __s32 session_fd;
    __u32 reserved;
    struct sbk_rdma_region remote;
};
struct sbk_region_seal {
    struct sbk_rdma_region remote;
    struct sbk_hot_list dirty;
};
#define SBK_FEATURE_ANONYMOUS_PTE (1U << 0)
#define SBK_FEATURE_PARALLEL_PS (1U << 1)
#define SBK_FEATURE_PS_SLICE (1U << 2)
#define SBK_FEATURE_PARALLEL_EXPORT (1U << 3)
#define SBK_FEATURE_SESSION_DISPATCH (1U << 4)
struct sbk_ps_slice {
    __s32 source_fd;
    __u32 reserved;
    __u64 source_offset, destination_offset, pages;
};
/* Actual fixed CPU concurrency, separate from per-region page accounting.
 * Index 0 is FT; index 1 is BG (including pretransfer before ARM). */
struct sbk_dispatch_lane_stats {
    __u64 submitted, quanta, completed, queued, queue_peak;
    __u32 workers, active, peak, reserved;
};
struct sbk_dispatch_stats { struct sbk_dispatch_lane_stats lane[2]; };
struct sbk_capabilities { __u32 version, features, max_regions, max_pages; };
struct sbk_drain_status {
    __u64 pages, retired_tokens;
    __u32 armed, drained;
};
struct sbk_stats {
    /* faults/hits/timing count FAULT_FLAG_USER callbacks, excluding installers.
     * File fixture: callback only, excludes Linux PTE install.
     * ARM_ANON: bridge entry through PTE install and prefetch scheduling.
     * Both exclude x86 fault entry and return to the application. */
    __u64 faults, hits, waits, errors;
    __u64 fetched[SBK_LANES], installed_ahead, skipped_install;
    __u64 fault_ns, fault_max_ns, hist_ns_pow2[32];
    __u64 batches, pages, completed, pretransferred, invalidated;
};
struct sbk_page_info {
    __u64 index, started_ns, completed_ns;
    __u32 state, lane;
};

/* Control plane exchanges this over an authenticated channel. No remote writes. */
struct sbk_rdma_endpoint {
    __u32 version, role;
    __u64 address, pages;
    __u32 rkey, mtu;
    __u8 gid[16], mac[6], reserved[2];
    __u32 slots[SBK_LANES];
    __u32 qpn[SBK_LANES][SBK_RDMA_MAX_SLOTS];
};
struct sbk_rdma_setup {
    char device[64];
    __u32 role, port, gid_index, timeout_ms;
    __u32 slots[SBK_LANES];
    __u32 traffic_class[SBK_LANES];
    __u64 source_address, pages;
    struct sbk_rdma_endpoint local;
};

#define SBK_IOC_CONFIG _IOW('B', 1, struct sbk_config)
#define SBK_IOC_BACKGROUND _IOW('B', 2, struct sbk_hot_list)
#define SBK_IOC_STATS _IOR('B', 3, struct sbk_stats)
#define SBK_IOC_CANCEL _IO('B', 4)
#define SBK_IOC_PAGE _IOWR('B', 5, struct sbk_page_info)
#define SBK_IOC_RDMA_CREATE _IOWR('B', 6, struct sbk_rdma_setup)
#define SBK_IOC_RDMA_CONNECT _IOW('B', 7, struct sbk_rdma_endpoint)
/* PRETRANSFER accepts at most MAX_BATCH indices; SEAL lists all dirty indices. */
#define SBK_IOC_PRETRANSFER _IOW('B', 8, struct sbk_hot_list)
#define SBK_IOC_SEAL _IOW('B', 9, struct sbk_hot_list)
/* Existing, empty MAP_PRIVATE|MAP_ANONYMOUS range in the caller. Requires the
 * audited CONFIG_SWIFTBATON_PTE bridge; stock kernels return EOPNOTSUPP. */
#define SBK_IOC_ARM_ANON _IOW('B', 10, struct sbk_anon_arm)
/* Synchronously revoke remote reads before returning; source sessions only. */
#define SBK_IOC_REVOKE_SOURCE _IO('B', 11)
/* Input address/pages, rkey=id=0; output full descriptor. Source catalog only. */
#define SBK_IOC_EXPORT_REGION _IOWR('B', 12, struct sbk_rdma_region)
/* Fresh destination region fd borrows a connected catalog session's QPs/CQs. */
#define SBK_IOC_BIND_REGION _IOW('B', 13, struct sbk_region_bind)
/* After source quiescence: switch to the final MR and invalidate dirty/PFN-
 * changed PS pages atomically, before ARM_ANON. Caller supplies final validation. */
#define SBK_IOC_SEAL_REGION _IOW('B', 14, struct sbk_region_seal)
/* Register one eventfd for anonymous-PTE retirement. Signaled only after all
 * forked marker references are gone and this region's workers have drained. */
#define SBK_IOC_WATCH_DRAIN _IOW('B', 15, __s32)
#define SBK_IOC_DRAIN_STATUS _IOR('B', 16, struct sbk_drain_status)
/* Read-only feature detection before an experiment stops its source. */
#define SBK_IOC_CAPABILITIES _IOR('B', 17, struct sbk_capabilities)
/* Synchronous PS list, up to the configured region's page count. Validates the
 * entire list before fetching, uses background_workers independent batch
 * workers, and joins all submitted reads before returning. No PTE is exposed.
 * Probe SBK_FEATURE_PARALLEL_PS; legacy PRETRANSFER remains batch-bounded. */
#define SBK_IOC_PRETRANSFER_MANY _IOW('B', 18, struct sbk_hot_list)
/* Move cached PS page ownership between unexposed regions of the same session.
 * No copy/READ is issued. Final SEAL and dirty/PFN validation remain mandatory. */
#define SBK_IOC_IMPORT_PS _IOW('B', 19, struct sbk_ps_slice)
#define SBK_IOC_EXPORT_BATCH _IOWR('B', 20, struct sbk_export_batch)
#define SBK_IOC_DISPATCH_STATS _IOR('B', 21, struct sbk_dispatch_stats)
#endif
