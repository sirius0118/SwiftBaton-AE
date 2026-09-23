#ifndef __SB_SCHED_H__
#define __SB_SCHED_H__
#include <stddef.h>
#include <stdint.h>

/* One immutable migration address space. The mapping is pointer-free and may
 * be MAP_SHARED at different virtual addresses in the pager and dumpee.
 * Address lookup, RDMA transport, UFFD lifecycle events and worker creation
 * belong to the caller. A page is committed only after the install ACK. */
enum sb_lane { SB_BACKGROUND, SB_PREFETCH, SB_DEMAND, SB_LANES };
#define SB_LANE_MASK(lane) (1U << (lane))
#define SB_ALL_LANES ((1U << SB_LANES) - 1)
enum sb_page_state {
    SB_PAGE_IDLE = 0,
    SB_PAGE_BG_QUEUED, SB_PAGE_PF_QUEUED, SB_PAGE_DEMAND_QUEUED,
    SB_PAGE_BG_ACTIVE = 5, SB_PAGE_PF_ACTIVE, SB_PAGE_DEMAND_ACTIVE,
    SB_PAGE_COMMITTED, SB_PAGE_FAILED, SB_PAGE_PRECOPY_PENDING
};
struct sb_sched;
struct sb_job { uint64_t page, ticket; };
struct sb_sched_stats {
    uint64_t pages, committed, failed;
    uint64_t queued[SB_LANES], claimed[SB_LANES];
    uint64_t coalesced, promoted, stale, queue_full;
};
size_t sb_sched_size(uint64_t pages, unsigned queue_capacity);
struct sb_sched *sb_sched_init(void *memory, size_t length, uint64_t pages, unsigned capacity);
struct sb_sched *sb_sched_attach(void *memory, size_t length);
/* Before submitting work: reserve validated pages for the local pre-copy or
 * parent-stage path. Reservation does NOT count as installation/completion. */
int sb_sched_seed(struct sb_sched *s, uint64_t page);
/* Acknowledge that the reserved page was installed or actually adopted. */
int sb_sched_seed_commit(struct sb_sched *s, uint64_t page);
/* 1 = enqueued/promoted, 0 = already owned/committed, negative errno.
 * EAGAIN preserves the existing owner and requires the caller to retry. */
int sb_sched_request(struct sb_sched *s, uint64_t page, enum sb_lane lane);
/* Dedicated workers may use one lane; spare background workers may assist all
 * lanes. Demand precedes prefetch, which precedes background within each call.
 * The background producer submits pages in descending heat order. */
int sb_sched_claim(struct sb_sched *s, unsigned lane_mask, struct sb_job *job);
/* The exact job ticket protects against stale and duplicate acknowledgments.
 * Commit only after all bytes have reached the intended UFFD range. */
int sb_sched_commit(struct sb_sched *s, const struct sb_job *job);
int sb_sched_fail(struct sb_sched *s, const struct sb_job *job);
enum sb_page_state sb_sched_state(struct sb_sched *s, uint64_t page);
enum sb_lane sb_job_lane(const struct sb_job *job);
void sb_sched_stats(struct sb_sched *s, struct sb_sched_stats *out);
#endif
