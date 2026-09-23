#ifndef __SB_FAST_COPY_H__
#define __SB_FAST_COPY_H__
#include <stdint.h>
#include <stdbool.h>

/* One producer (the lane coordinator), multiple dumpee copying workers.
 * The coordinator owns a slot until local RDMA completion, independently of
 * the scheduler's page ownership, which lasts through target install ACK.
 * Each process has distinct queues, so equal virtual addresses cannot alias. */
#define SB_FAST_SLOTS 64U
#define SB_FAST_LANES 2U
#define SB_FAST_WORKERS 32U
enum sb_fast_state { SB_FAST_FREE, SB_FAST_REQUEST, SB_FAST_COPYING, SB_FAST_READY };
struct sb_fast_page { uint64_t pid, address; unsigned char data[4096]; };
struct sb_fast_slot {
    unsigned state;
    unsigned char pad[60];
    struct sb_fast_page page;
} __attribute__((aligned(64)));
struct sb_fast_queue {
    unsigned stop;
    unsigned char pad[60];
    uint64_t completed[SB_FAST_WORKERS];
    struct sb_fast_slot slots[SB_FAST_SLOTS];
};
static inline int sb_fast_submit(volatile struct sb_fast_queue *q, unsigned slot,
                                 uint64_t pid, uint64_t address)
{
    volatile struct sb_fast_slot *s = &q->slots[slot];
    if (__atomic_load_n(&s->state, __ATOMIC_ACQUIRE) != SB_FAST_FREE) return 0;
    s->page.pid = pid; s->page.address = address;
    __atomic_store_n(&s->state, SB_FAST_REQUEST, __ATOMIC_RELEASE);
    return 1;
}
static inline int sb_fast_copy_one(volatile struct sb_fast_queue *q, unsigned *cursor, unsigned worker)
{
    for (unsigned i = 0; i < SB_FAST_SLOTS; i++) {
        unsigned slot = (*cursor + i) % SB_FAST_SLOTS, expected = SB_FAST_REQUEST;
        volatile struct sb_fast_slot *s = &q->slots[slot];
        if (!__atomic_compare_exchange_n(&s->state, &expected, SB_FAST_COPYING,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) continue;
        memcpy((void *)s->page.data, (const void *)(uintptr_t)s->page.address, 4096);
        __atomic_fetch_add(&q->completed[worker], 1, __ATOMIC_RELAXED);
        __atomic_store_n(&s->state, SB_FAST_READY, __ATOMIC_RELEASE);
        *cursor = (slot + 1) % SB_FAST_SLOTS;
        return 1;
    }
    return 0;
}
static inline int sb_fast_ready(volatile struct sb_fast_queue *q, unsigned slot)
{ return __atomic_load_n(&q->slots[slot].state, __ATOMIC_ACQUIRE) == SB_FAST_READY; }
/* Only the coordinator touches busy/dma/cursor. A selected slot must be marked
 * dma (or released) before another selection. Rotate across completed slots:
 * rescanning from zero can starve old high slots while low slots are refilled. */
static inline int sb_fast_pick_ready(volatile struct sb_fast_queue *q, const bool *busy,
                                    const bool *dma, unsigned *cursor)
{
    for (unsigned i=0;i<SB_FAST_SLOTS;i++) {
        unsigned c=(*cursor+i)%SB_FAST_SLOTS;
        if (!busy[c] || dma[c] || !sb_fast_ready(q,c)) continue;
        *cursor=(c+1)%SB_FAST_SLOTS;
        return c;
    }
    return -1;
}
static inline void sb_fast_release(volatile struct sb_fast_queue *q, unsigned slot)
{ __atomic_store_n(&q->slots[slot].state, SB_FAST_FREE, __ATOMIC_RELEASE); }
#endif
