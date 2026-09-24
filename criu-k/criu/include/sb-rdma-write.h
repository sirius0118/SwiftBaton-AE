/* Ordered RC writes: data segments precede publication on the same QP.
 * Only the final WR is signalled. Its CQE releases local DMA buffers, not
 * remote ring slots: those still require the receiver's installation ACK. */
#ifndef SB_RDMA_WRITE_H
#define SB_RDMA_WRITE_H
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <string.h>
#define SB_RDMA_WRITE_MAX 64U
struct write_part { struct ibv_mr *mr; const void *address; unsigned length; uint64_t offset; };

static inline int sb_rdma_write_chain(const struct write_part *parts, unsigned count,
    uint64_t remote, uint32_t rkey, uint64_t cookie, struct ibv_send_wr *wr, struct ibv_sge *sg)
{
    if (!count || count > SB_RDMA_WRITE_MAX) return -EINVAL;
    memset(wr, 0, count * sizeof(*wr));
    for (unsigned i = 0; i < count; i++) {
        const struct write_part *p = &parts[i];
        uintptr_t address = (uintptr_t)p->address;
        if (!p->mr || !p->length || address < (uintptr_t)p->mr->addr ||
            p->length > p->mr->length || address - (uintptr_t)p->mr->addr > p->mr->length - p->length ||
            p->offset > UINT64_MAX - remote || p->length > UINT64_MAX - (remote + p->offset))
            return -EINVAL;
        sg[i] = (struct ibv_sge){ .addr=address, .length=p->length, .lkey=p->mr->lkey };
        wr[i].wr_id=cookie; wr[i].sg_list=&sg[i]; wr[i].num_sge=1;
        wr[i].opcode=IBV_WR_RDMA_WRITE;
        wr[i].wr.rdma.remote_addr=remote+p->offset; wr[i].wr.rdma.rkey=rkey;
        wr[i].next=i+1<count ? &wr[i+1] : NULL;
        wr[i].send_flags=i+1==count ? IBV_SEND_SIGNALED : 0;
    }
    return 0;
}

/* Split the complete wire frame, including metadata. Never publish a partial
 * frame. The receiver still sees one batch and consumes the same wire format. */
static inline int sb_rdma_write_ranges(struct write_part *parts, unsigned capacity,
    const struct write_part *ranges, unsigned count, unsigned segment_bytes, struct write_part publish)
{
    unsigned needed=1, used=0;
    if (!count || count >= capacity || capacity > SB_RDMA_WRITE_MAX || capacity < 2) return -EINVAL;
    for (unsigned i=0; i<count; i++) {
        const struct write_part *r=&ranges[i];
        if (!r->length || r->length>UINTPTR_MAX-(uintptr_t)r->address || r->length>UINT64_MAX-r->offset)
            return -EINVAL;
        unsigned n=segment_bytes ? 1+(r->length-1)/segment_bytes : 1;
        if (n>capacity-needed) return -ENOSPC;
        needed+=n;
    }
    for (unsigned i=0; i<count; i++) {
        const struct write_part *r=&ranges[i];
        for (unsigned done=0; done<r->length;) {
            unsigned n=r->length-done;
            if (segment_bytes && n>segment_bytes) n=segment_bytes;
            parts[used++]=(struct write_part){r->mr,(const char *)r->address+done,n,r->offset+done};
            done+=n;
        }
    }
    parts[used++]=publish;
    return used;
}
static inline int sb_rdma_segment_frame(struct write_part *parts, unsigned capacity,
    struct ibv_mr *mr, const void *data, unsigned bytes, uint64_t offset,
    unsigned segment_bytes, struct write_part publish)
{
    if (!segment_bytes) return -EINVAL;
    struct write_part range={mr,data,bytes,offset};
    return sb_rdma_write_ranges(parts,capacity,&range,1,segment_bytes,publish);
}
#endif
