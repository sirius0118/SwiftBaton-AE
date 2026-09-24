#ifndef SB_PF_INSTALL_H
#define SB_PF_INSTALL_H
#include <stdbool.h>
#include <stdint.h>

#define SB_PF_SLOTS 100U
#define SB_PF_AUX_SLOTS 512U
enum { SB_PF_READY=1, SB_PF_OWNED=2, SB_PF_DONE=3 };
struct sb_pf_slot { uint64_t token; } __attribute__((aligned(64)));
/* Sole response coordinator publishes and retires, multiple installers claim.
 * Publication pins the existing RDMA payload. Only contiguous DONE slots can
 * return remote credit. Absolute generations reject stale/duplicate finishes. */
struct sb_pf_queue {
    uint64_t published __attribute__((aligned(64)));
    uint64_t claimed __attribute__((aligned(64)));
    uint64_t retired __attribute__((aligned(64)));
    struct sb_pf_slot slots[SB_PF_SLOTS];
};
static inline bool sb_pf_publish(struct sb_pf_queue *q)
{
    uint64_t n=__atomic_load_n(&q->published,__ATOMIC_RELAXED);
    if (n-__atomic_load_n(&q->retired,__ATOMIC_ACQUIRE)>=SB_PF_SLOTS-1 || n>=UINT64_MAX/4-1) return false;
    if (__atomic_load_n(&q->slots[n%SB_PF_SLOTS].token,__ATOMIC_ACQUIRE)) return false;
    __atomic_store_n(&q->slots[n%SB_PF_SLOTS].token,((n+1)<<2)|SB_PF_READY,__ATOMIC_RELEASE);
    __atomic_store_n(&q->published,n+1,__ATOMIC_RELEASE);
    return true;
}
static inline int sb_pf_claim(struct sb_pf_queue *q,uint64_t *sequence)
{
    uint64_t n=__atomic_load_n(&q->claimed,__ATOMIC_RELAXED);
    for (;;) {
        if (n==__atomic_load_n(&q->published,__ATOMIC_ACQUIRE)) return 0;
        if (__atomic_compare_exchange_n(&q->claimed,&n,n+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            uint64_t expected=((n+1)<<2)|SB_PF_READY;
            if (!__atomic_compare_exchange_n(&q->slots[n%SB_PF_SLOTS].token,&expected,
                    ((n+1)<<2)|SB_PF_OWNED,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) return -1;
            *sequence=n;return 1;
        }
    }
}
static inline bool sb_pf_complete(struct sb_pf_queue *q,uint64_t sequence)
{
    uint64_t expected=((sequence+1)<<2)|SB_PF_OWNED;
    return __atomic_compare_exchange_n(&q->slots[sequence%SB_PF_SLOTS].token,&expected,
        ((sequence+1)<<2)|SB_PF_DONE,false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
static inline bool sb_pf_retire(struct sb_pf_queue *q)
{
    uint64_t n=__atomic_load_n(&q->retired,__ATOMIC_RELAXED);
    if (n==__atomic_load_n(&q->published,__ATOMIC_ACQUIRE)) return false;
    if (__atomic_load_n(&q->slots[n%SB_PF_SLOTS].token,__ATOMIC_ACQUIRE)!=(((n+1)<<2)|SB_PF_DONE)) return false;
    __atomic_store_n(&q->slots[n%SB_PF_SLOTS].token,0,__ATOMIC_RELEASE);
    __atomic_store_n(&q->retired,n+1,__ATOMIC_RELEASE);return true;
}
/* Auxiliary work remains SPSC per installer: its PID event reader is the sole
 * producer. Payload is copied before the consumer releases the queue cell. */
enum { SB_PF_AUX_PRECOPY=1, SB_PF_AUX_BG_DIRECT=2, SB_PF_AUX_BG_QUEUED=3 };
struct sb_pf_aux_job { uint64_t value; unsigned kind; };
struct sb_pf_aux_queue {
    uint64_t head __attribute__((aligned(64)));
    uint64_t tail __attribute__((aligned(64)));
    struct sb_pf_aux_job jobs[SB_PF_AUX_SLOTS] __attribute__((aligned(64)));
};
static inline uint64_t sb_pf_aux_pending(struct sb_pf_aux_queue *q)
{
    uint64_t head=__atomic_load_n(&q->head,__ATOMIC_ACQUIRE);
    uint64_t tail=__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE);
    return head-tail;
}
static inline bool sb_pf_aux_push(struct sb_pf_aux_queue *q,struct sb_pf_aux_job job)
{
    uint64_t head=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    if (head-__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE)==SB_PF_AUX_SLOTS) return false;
    q->jobs[head%SB_PF_AUX_SLOTS]=job;
    __atomic_store_n(&q->head,head+1,__ATOMIC_RELEASE);return true;
}
static inline bool sb_pf_aux_pop(struct sb_pf_aux_queue *q,struct sb_pf_aux_job *job)
{
    uint64_t tail=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
    if (tail==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE)) return false;
    *job=q->jobs[tail%SB_PF_AUX_SLOTS];
    __atomic_store_n(&q->tail,tail+1,__ATOMIC_RELEASE);return true;
}
#endif
