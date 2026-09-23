#ifndef __SB_LIFECYCLE_GATE_H__
#define __SB_LIFECYCLE_GATE_H__
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
/* Experimental busy-wait alternative. The same gate still covers the entire
 * UFFD event acknowledgement + map/bitmap update, or COPY + publication.
 * No pointers escape the gate. Nonrecursive readers; announce writers before
 * waiting so continuous background copies cannot indefinitely admit readers.
 * Busy waiting is opt-in: a preempted owner can make it expensive. */
struct sb_lifecycle_gate {
    pthread_rwlock_t sleep;
    unsigned state, writers;
    bool spin;
};
#define SB_GATE_WRITER (1U << 31)
static inline void sb_gate_pause(void) { __asm__ volatile("pause" ::: "memory"); }
static inline void sb_gate_wrlock(struct sb_lifecycle_gate *g)
{
    if (!g->spin) { pthread_rwlock_wrlock(&g->sleep); return; }
    __atomic_fetch_add(&g->writers,1,__ATOMIC_ACQ_REL);
    for (;;) {
        unsigned empty=0;
        if (__atomic_compare_exchange_n(&g->state,&empty,SB_GATE_WRITER,false,
                                        __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) break;
        while (__atomic_load_n(&g->state,__ATOMIC_RELAXED)) sb_gate_pause();
    }
    __atomic_fetch_sub(&g->writers,1,__ATOMIC_RELEASE);
}
static inline void sb_gate_rdlock(struct sb_lifecycle_gate *g)
{
    if (!g->spin) { pthread_rwlock_rdlock(&g->sleep); return; }
    for (;;) {
        while (__atomic_load_n(&g->writers,__ATOMIC_ACQUIRE)) sb_gate_pause();
        unsigned value=__atomic_load_n(&g->state,__ATOMIC_RELAXED);
        if (!(value&SB_GATE_WRITER) && value<SB_GATE_WRITER-1 &&
            __atomic_compare_exchange_n(&g->state,&value,value+1,false,
                                        __ATOMIC_ACQUIRE,__ATOMIC_RELAXED)) return;
        sb_gate_pause();
    }
}
static inline void sb_gate_unlock(struct sb_lifecycle_gate *g)
{
    if (!g->spin) { pthread_rwlock_unlock(&g->sleep); return; }
    /* The current holder prevents a reader/writer mode change until release. */
    if (__atomic_load_n(&g->state,__ATOMIC_RELAXED)&SB_GATE_WRITER)
        __atomic_store_n(&g->state,0,__ATOMIC_RELEASE);
    else __atomic_fetch_sub(&g->state,1,__ATOMIC_RELEASE);
}
#endif
