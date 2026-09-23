/* Shared priority queues and per-page ownership for SwiftBaton-U. Queueing is
 * not completion: a queued background page can be promoted, while an active
 * page keeps one owner until the receiver acknowledges its installation. */
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include "sb-sched.h"

#define SB_SCHED_MAGIC UINT64_C(0x5342534348454431)
#define STATE_MASK UINT64_C(15)
#define GENERATION_STEP UINT64_C(16)
#define CACHELINE 64U
struct sb_cell { uint64_t sequence; struct sb_job job; };
struct sb_queue {
    uint64_t producer __attribute__((aligned(CACHELINE)));
    uint64_t consumer __attribute__((aligned(CACHELINE)));
    uint64_t cells;
};
struct sb_sched {
    uint64_t magic, length, pages, states;
    unsigned capacity, version;
    struct sb_queue queues[SB_LANES];
    struct sb_sched_stats stats;
};

static size_t align64(size_t n) { return (n + CACHELINE - 1) & ~(size_t)(CACHELINE - 1); }
static uint64_t *page_state(struct sb_sched *s, uint64_t page)
{ return (uint64_t *)((char *)s + s->states) + page; }
static struct sb_cell *cell_at(struct sb_sched *s, unsigned lane, uint64_t position)
{
    return (struct sb_cell *)((char *)s + s->queues[lane].cells) + (position & (s->capacity - 1));
}
static void add(uint64_t *value) { __atomic_fetch_add(value, 1, __ATOMIC_RELAXED); }

size_t sb_sched_size(uint64_t pages, unsigned capacity)
{
    size_t header = align64(sizeof(struct sb_sched)), queues;
    if (!pages || capacity < 2 || capacity > (1U << 20) || (capacity & (capacity - 1))) return 0;
    queues = align64((size_t)SB_LANES * capacity * sizeof(struct sb_cell));
    if (pages > (SIZE_MAX - header - queues) / sizeof(uint64_t)) return 0;
    return header + queues + (size_t)pages * sizeof(uint64_t);
}

struct sb_sched *sb_sched_init(void *memory, size_t length, uint64_t pages, unsigned capacity)
{
    struct sb_sched *s = memory;
    size_t bytes = sb_sched_size(pages, capacity), offset = align64(sizeof(*s));
    unsigned lane, i;
    if (!memory || ((uintptr_t)memory & (CACHELINE - 1)) || !bytes || length < bytes ||
        !__atomic_always_lock_free(sizeof(uint64_t), 0)) return NULL;
    memset(memory, 0, bytes);
    s->length = bytes; s->pages = pages; s->capacity = capacity; s->version = 1;
    s->stats.pages = pages;
    for (lane = 0; lane < SB_LANES; lane++) {
        s->queues[lane].cells = offset;
        for (i = 0; i < capacity; i++) cell_at(s, lane, i)->sequence = i;
        offset += (size_t)capacity * sizeof(struct sb_cell);
    }
    s->states = align64(offset);
    __atomic_store_n(&s->magic, SB_SCHED_MAGIC, __ATOMIC_RELEASE);
    return s;
}

struct sb_sched *sb_sched_attach(void *memory, size_t length)
{
    struct sb_sched *s = memory;
    size_t offset = align64(sizeof(*s));
    unsigned lane;
    if (!memory || ((uintptr_t)memory & (CACHELINE - 1)) || length < sizeof(*s) ||
        __atomic_load_n(&s->magic, __ATOMIC_ACQUIRE) != SB_SCHED_MAGIC || s->version != 1 ||
        !s->length || s->length > length || s->length != sb_sched_size(s->pages, s->capacity)) return NULL;
    for (lane = 0; lane < SB_LANES; lane++) {
        if (s->queues[lane].cells != offset) return NULL;
        offset += (size_t)s->capacity * sizeof(struct sb_cell);
    }
    return s->states == align64(offset) ? s : NULL;
}

/* Reserve capacity before changing page ownership. Otherwise a full queue and
 * two racing priority upgrades can roll back to a ticket that was never
 * published, permanently losing the page. Every reservation is published,
 * including a tombstone when its ownership CAS loses. */
static struct sb_cell *reserve(struct sb_sched *s, unsigned lane, uint64_t *position)
{
    struct sb_queue *q = &s->queues[lane];
    uint64_t pos = __atomic_load_n(&q->producer, __ATOMIC_RELAXED);
    for (;;) {
        struct sb_cell *cell = cell_at(s, lane, pos);
        uint64_t seq = __atomic_load_n(&cell->sequence, __ATOMIC_ACQUIRE);
        int64_t delta = (int64_t)(seq - pos);
        if (!delta) {
            if (__atomic_compare_exchange_n(&q->producer, &pos, pos + 1, 0,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                *position = pos;
                return cell;
            }
        } else if (delta < 0) return NULL;
        else pos = __atomic_load_n(&q->producer, __ATOMIC_RELAXED);
    }
}

static int pop(struct sb_sched *s, unsigned lane, struct sb_job *job)
{
    struct sb_queue *q = &s->queues[lane];
    uint64_t pos = __atomic_load_n(&q->consumer, __ATOMIC_RELAXED);
    for (;;) {
        struct sb_cell *cell = cell_at(s, lane, pos);
        uint64_t seq = __atomic_load_n(&cell->sequence, __ATOMIC_ACQUIRE);
        int64_t delta = (int64_t)(seq - (pos + 1));
        if (!delta) {
            if (__atomic_compare_exchange_n(&q->consumer, &pos, pos + 1, 0,
                                             __ATOMIC_RELAXED, __ATOMIC_RELAXED)) {
                *job = cell->job;
                __atomic_store_n(&cell->sequence, pos + s->capacity, __ATOMIC_RELEASE);
                return 1;
            }
        } else if (delta < 0) return 0;
        else pos = __atomic_load_n(&q->consumer, __ATOMIC_RELAXED);
    }
}

int sb_sched_seed(struct sb_sched *s, uint64_t page)
{
    uint64_t expected = 0;
    if (page >= s->pages) return -EINVAL;
    if (!__atomic_compare_exchange_n(page_state(s, page), &expected, SB_PAGE_PRECOPY_PENDING,
                                     0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return -EALREADY;
    return 0;
}

int sb_sched_seed_commit(struct sb_sched *s, uint64_t page)
{
    uint64_t expected = SB_PAGE_PRECOPY_PENDING;
    if (page >= s->pages) return -EINVAL;
    if (!__atomic_compare_exchange_n(page_state(s, page), &expected, SB_PAGE_COMMITTED,
                                     0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return -ESTALE;
    __atomic_fetch_add(&s->stats.committed, 1, __ATOMIC_RELEASE);
    return 0;
}

int sb_sched_request(struct sb_sched *s, uint64_t page, enum sb_lane lane)
{
    uint64_t word, state, token, pos;
    struct sb_cell *cell;
    if (page >= s->pages || (unsigned)lane >= SB_LANES) return -EINVAL;
    if (__atomic_load_n(&s->stats.failed, __ATOMIC_ACQUIRE)) return -EIO;
    word = __atomic_load_n(page_state(s, page), __ATOMIC_ACQUIRE);
    for (;;) {
        state = word & STATE_MASK;
        if (state == SB_PAGE_FAILED) return -EIO;
        if (state >= SB_PAGE_BG_ACTIVE || state >= (uint64_t)lane + 1) {
            add(&s->stats.coalesced);
            return 0;
        }
        if (word > UINT64_MAX - GENERATION_STEP) return -EOVERFLOW;
        cell = reserve(s, lane, &pos);
        if (!cell) { add(&s->stats.queue_full); return -EAGAIN; }
        token = ((word & ~STATE_MASK) + GENERATION_STEP) | ((uint64_t)lane + 1);
        if (__atomic_compare_exchange_n(page_state(s, page), &word, token, 0,
                                         __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            cell->job = (struct sb_job){ .page = page, .ticket = token };
            __atomic_store_n(&cell->sequence, pos + 1, __ATOMIC_RELEASE);
            add(&s->stats.queued[lane]);
            if (state) add(&s->stats.promoted);
            return 1;
        }
        cell->job = (struct sb_job){ .page = UINT64_MAX, .ticket = 0 };
        __atomic_store_n(&cell->sequence, pos + 1, __ATOMIC_RELEASE);
    }
}

int sb_sched_claim(struct sb_sched *s, unsigned mask, struct sb_job *job)
{
    int lane;
    unsigned attempts = 0;
    if (!job || !mask || (mask & ~SB_ALL_LANES)) return -EINVAL;
    if (__atomic_load_n(&s->stats.failed, __ATOMIC_ACQUIRE)) return -EIO;
restart:
    for (lane = SB_DEMAND; lane >= SB_BACKGROUND; lane--) {
        uint64_t expected;
        if (!(mask & SB_LANE_MASK(lane)) || !pop(s, lane, job)) continue;
        expected = job->ticket;
        if (job->page < s->pages && (expected & STATE_MASK) == (unsigned)lane + 1 &&
            __atomic_compare_exchange_n(page_state(s, job->page), &expected, job->ticket + 4,
                                         0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            add(&s->stats.claimed[lane]);
            return 1;
        }
        add(&s->stats.stale);
        /* Recheck demand after each stale lower-priority entry. */
        if (++attempts < 3 * s->capacity) goto restart;
        return 0;
    }
    return 0;
}

static int finish(struct sb_sched *s, const struct sb_job *job, unsigned state)
{
    uint64_t expected;
    if (!job || job->page >= s->pages || !(job->ticket & STATE_MASK) ||
        (job->ticket & STATE_MASK) > SB_DEMAND + 1) return -EINVAL;
    expected = job->ticket + 4;
    if (!__atomic_compare_exchange_n(page_state(s, job->page), &expected,
            (job->ticket & ~STATE_MASK) | state, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) return -ESTALE;
    __atomic_fetch_add(state == SB_PAGE_COMMITTED ? &s->stats.committed : &s->stats.failed,
                       1, __ATOMIC_RELEASE);
    return 0;
}
int sb_sched_commit(struct sb_sched *s, const struct sb_job *job)
{ return finish(s, job, SB_PAGE_COMMITTED); }
int sb_sched_fail(struct sb_sched *s, const struct sb_job *job)
{ return finish(s, job, SB_PAGE_FAILED); }
enum sb_page_state sb_sched_state(struct sb_sched *s, uint64_t page)
{
    if (page >= s->pages) return SB_PAGE_FAILED;
    return __atomic_load_n(page_state(s, page), __ATOMIC_ACQUIRE) & STATE_MASK;
}
enum sb_lane sb_job_lane(const struct sb_job *job) { return (job->ticket & STATE_MASK) - 1; }
void sb_sched_stats(struct sb_sched *s, struct sb_sched_stats *out)
{
    unsigned lane;
    out->pages = s->pages;
    out->committed = __atomic_load_n(&s->stats.committed, __ATOMIC_ACQUIRE);
    out->failed = __atomic_load_n(&s->stats.failed, __ATOMIC_ACQUIRE);
    for (lane = 0; lane < SB_LANES; lane++) {
        out->queued[lane] = __atomic_load_n(&s->stats.queued[lane], __ATOMIC_RELAXED);
        out->claimed[lane] = __atomic_load_n(&s->stats.claimed[lane], __ATOMIC_RELAXED);
    }
    out->coalesced = __atomic_load_n(&s->stats.coalesced, __ATOMIC_RELAXED);
    out->promoted = __atomic_load_n(&s->stats.promoted, __ATOMIC_RELAXED);
    out->stale = __atomic_load_n(&s->stats.stale, __ATOMIC_RELAXED);
    out->queue_full = __atomic_load_n(&s->stats.queue_full, __ATOMIC_RELAXED);
}
