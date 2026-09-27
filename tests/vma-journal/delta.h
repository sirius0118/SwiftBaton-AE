#ifndef SB_VMA_DELTA_H
#define SB_VMA_DELTA_H
#include "journal.h"
#include <stdint.h>
#include <string.h>
enum delta_scope { DELTA_RANGES, DELTA_PROCESS, DELTA_ALL };
struct delta_range { uint64_t start,end; };
struct vma_delta { unsigned pid,scope,count; struct delta_range ranges[3]; };
static inline int delta_add(struct vma_delta *d,uint64_t start,uint64_t bytes)
{
    if(!bytes || start>UINT64_MAX-bytes || start+bytes>UINT64_MAX-4095 || d->count>=3) {
        d->scope=DELTA_PROCESS;d->count=0;return -1;
    }
    d->ranges[d->count++]=(struct delta_range){start&~UINT64_C(4095),(start+bytes+4095)&~UINT64_C(4095)};
    return 0;
}
static inline struct vma_delta decode_delta(const struct journal_event *e)
{
    struct vma_delta d={.pid=e->pid_tgid>>32,.scope=DELTA_RANGES};
    if(e->global_scope){d.scope=DELTA_ALL;return d;}
    switch(e->syscall_nr) {
    case 9: /* mmap: returned VA matters; the requested VA can be a hint. */
        if(e->result<0){d.scope=DELTA_PROCESS;break;}
        delta_add(&d,e->result,e->args[1]);break;
    case 10: case 11: case 28: case 149: case 150: case 325: case 329:
        /* Mark attempted ranges even on errors; some operations can have
         * partial effects. Extra invalidation is safe. */
        delta_add(&d,e->args[0],e->args[1]);break;
    case 25: /* mremap old range plus new returned/fixed destination. */
        if(delta_add(&d,e->args[0],e->args[1]))break;
        if(e->result>=0){delta_add(&d,e->result,e->args[2]);break;}
        if(e->args[3]&2)delta_add(&d,e->args[4],e->args[2]);
        break;
    default: d.scope=DELTA_PROCESS; /* brk/exec/clone/ioctl/unknown: conservative. */
    }
    return d;
}
#endif
