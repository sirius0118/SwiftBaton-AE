#ifndef SB_BG_INSTALL_H
#define SB_BG_INSTALL_H
#include <stdbool.h>
#include <stdint.h>

#define SB_BG_SLOTS 10U
#define SB_BG_MAX_PAGES 256U
#define SB_BG_PAGE_SLOTS (SB_BG_SLOTS * SB_BG_MAX_PAGES)
#define SB_BG_ASSIST_SLOTS 4096U
enum { SB_BG_READY=1, SB_BG_OWNED, SB_BG_DONE };
struct sb_bg_owner { uint64_t token; };
struct sb_bg_cursor {
    uint64_t position;
    unsigned count, remaining;
} __attribute__((aligned(64)));
struct sb_bg_assist_queue {
    uint64_t head __attribute__((aligned(64)));
    uint64_t tail __attribute__((aligned(64)));
    uint64_t handles[SB_BG_ASSIST_SLOTS] __attribute__((aligned(64)));
};
static inline uint64_t sb_bg_handle(uint32_t generation,unsigned slot,unsigned page)
{ return (uint64_t)generation*SB_BG_PAGE_SLOTS+slot*SB_BG_MAX_PAGES+page+1; }
static inline unsigned sb_bg_index(uint64_t handle)
{ return (handle-1)%SB_BG_PAGE_SLOTS; }
/* A descriptor's generation makes delayed queue entries harmless after a
 * remote ring slot has been acknowledged and reused. Only the claim winner
 * may access non-atomic metadata/payload, until it publishes completion. */
static inline void sb_bg_publish_owner(struct sb_bg_owner *p,uint64_t handle)
{ __atomic_store_n(&p->token,(handle<<2)|SB_BG_READY,__ATOMIC_RELEASE); }
static inline bool sb_bg_claim(struct sb_bg_owner *p,uint64_t handle)
{
    uint64_t expected=(handle<<2)|SB_BG_READY;
    return __atomic_compare_exchange_n(&p->token,&expected,(handle<<2)|SB_BG_OWNED,
                                      false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE);
}
static inline bool sb_bg_finish(struct sb_bg_owner *p,uint64_t handle)
{
    uint64_t expected=(handle<<2)|SB_BG_OWNED;
    return __atomic_compare_exchange_n(&p->token,&expected,(handle<<2)|SB_BG_DONE,
                                      false,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
}
/* One atomic per original page combines a wanted bit and its arrived-buffer
 * handle. No store/load lost-wakeup race: either the request sees the handle,
 * or publication sees the wanted bit and queues an assist to that PID. */
static inline uint64_t sb_bg_request(uint64_t *directory)
{ return __atomic_fetch_or(directory,UINT64_C(1),__ATOMIC_ACQ_REL)>>1; }
static inline uint64_t sb_bg_bind(uint64_t *directory,uint64_t handle)
{ return __atomic_exchange_n(directory,handle<<1,__ATOMIC_ACQ_REL); }
static inline uint64_t sb_bg_unbind(uint64_t *directory)
{ return __atomic_exchange_n(directory,0,__ATOMIC_ACQ_REL); }
static inline void sb_bg_publish_cursor(struct sb_bg_cursor *c,uint32_t generation,unsigned count)
{
    __atomic_store_n(&c->count,count,__ATOMIC_RELAXED);
    __atomic_store_n(&c->position,(uint64_t)generation<<32,__ATOMIC_RELEASE);
}
static inline bool sb_bg_next(struct sb_bg_cursor *c,unsigned slot,uint64_t *handle)
{
    uint64_t pos=__atomic_load_n(&c->position,__ATOMIC_ACQUIRE);
    for (;;) {
        unsigned page=(uint32_t)pos;
        if (!(pos>>32) || page>=__atomic_load_n(&c->count,__ATOMIC_RELAXED)) return false;
        if (__atomic_compare_exchange_n(&c->position,&pos,pos+1,false,__ATOMIC_ACQ_REL,__ATOMIC_ACQUIRE)) {
            *handle=sb_bg_handle(pos>>32,slot,page);return true;
        }
    }
}
static inline bool sb_bg_assist_push(struct sb_bg_assist_queue *q,uint64_t handle)
{
    uint64_t head=__atomic_load_n(&q->head,__ATOMIC_RELAXED);
    if (head-__atomic_load_n(&q->tail,__ATOMIC_ACQUIRE)==SB_BG_ASSIST_SLOTS) return false;
    q->handles[head&(SB_BG_ASSIST_SLOTS-1)]=handle;
    __atomic_store_n(&q->head,head+1,__ATOMIC_RELEASE);return true;
}
static inline bool sb_bg_assist_pop(struct sb_bg_assist_queue *q,uint64_t *handle)
{
    uint64_t tail=__atomic_load_n(&q->tail,__ATOMIC_RELAXED);
    if (tail==__atomic_load_n(&q->head,__ATOMIC_ACQUIRE)) return false;
    *handle=q->handles[tail&(SB_BG_ASSIST_SLOTS-1)];
    __atomic_store_n(&q->tail,tail+1,__ATOMIC_RELEASE);return true;
}
#endif
