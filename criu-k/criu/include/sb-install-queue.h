#ifndef SB_INSTALL_QUEUE_H
#define SB_INSTALL_QUEUE_H
#include <stdbool.h>
#include <stdint.h>

/* Target-only SPSC descriptors. Queue retirement is not remote-buffer credit:
 * only installation completion lets the coordinator acknowledge a ring slot.
 * Each queue has one dispatcher and exactly one installer. */
#define SB_INSTALL_QUEUE_SLOTS 100U
struct sb_install_queue {
    uint64_t head __attribute__((aligned(64)));
    uint64_t tail __attribute__((aligned(64)));
    unsigned slots[SB_INSTALL_QUEUE_SLOTS] __attribute__((aligned(64)));
};
struct sb_install_completion { unsigned done; } __attribute__((aligned(64)));
static inline bool sb_install_push(struct sb_install_queue *q,unsigned slot)
{
    uint64_t head=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    uint64_t tail=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);
    if (head-tail==SB_INSTALL_QUEUE_SLOTS) return false;
    q->slots[head%SB_INSTALL_QUEUE_SLOTS]=slot;
    __atomic_store_n(&q->head,head+1,__ATOMIC_RELEASE);
    return true;
}
static inline bool sb_install_pop(struct sb_install_queue *q,unsigned *slot)
{
    uint64_t tail=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
    if (tail==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE)) return false;
    *slot=q->slots[tail%SB_INSTALL_QUEUE_SLOTS];
    __atomic_store_n(&q->tail,tail+1,__ATOMIC_RELEASE);
    return true;
}
static inline bool sb_install_done(struct sb_install_completion *c)
{ return !__atomic_exchange_n(&c->done,1,__ATOMIC_RELEASE); }
static inline bool sb_install_retire(struct sb_install_completion *c)
{
    if (!__atomic_load_n(&c->done,__ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&c->done,0,__ATOMIC_RELAXED);
    return true;
}
#endif
