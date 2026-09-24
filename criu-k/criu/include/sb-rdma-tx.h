/* One thread owns each SQ/CQ. Small controls are copied into the WQE; page
 * buffers remain owned by the caller until their completion cookie returns.
 * Remote installation acknowledgements are a separate lifetime requirement. */
#ifndef SB_RDMA_TX_H
#define SB_RDMA_TX_H
#include <errno.h>
#include <infiniband/verbs.h>
#include <stdint.h>
#include <time.h>
#define SB_RDMA_TX_DEPTH 32U
struct sb_rdma_part { struct ibv_mr *mr; const void *data; uint32_t size; uint64_t offset; };
struct sb_rdma_tx {
    struct ibv_qp *qp;
    struct ibv_cq *cq;
    uint64_t remote_addr;
    uint32_t rkey;
    uint64_t posted, completed, cookie[SB_RDMA_TX_DEPTH], started[SB_RDMA_TX_DEPTH];
    unsigned maximum;
};
static inline uint64_t sb_rdma_clock(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
static inline unsigned sb_rdma_tx_pending(const struct sb_rdma_tx *t)
{ return t->posted - t->completed; }
static inline int sb_rdma_tx_post(struct sb_rdma_tx *t, const struct sb_rdma_part *p, unsigned n, uint64_t cookie)
{
    struct ibv_send_wr wr[2] = {{0}}, *bad;
    struct ibv_sge sg[2] = {{0}};
    int rc;
    if (!n || n > 2) return -EINVAL;
    if (sb_rdma_tx_pending(t) == SB_RDMA_TX_DEPTH) return -EAGAIN;
    for (unsigned i = 0; i < n; i++) {
        uintptr_t addr = (uintptr_t)p[i].data;
        if (!p[i].size) return -EINVAL;
        if (p[i].size > 16 && (!p[i].mr || addr < (uintptr_t)p[i].mr->addr ||
            p[i].size > p[i].mr->length || addr - (uintptr_t)p[i].mr->addr > p[i].mr->length - p[i].size))
            return -EINVAL;
        sg[i] = (struct ibv_sge){ .addr=addr, .length=p[i].size, .lkey=p[i].mr ? p[i].mr->lkey : 0 };
        wr[i].wr_id = t->posted + 1;
        wr[i].sg_list = &sg[i]; wr[i].num_sge = 1;
        wr[i].opcode = IBV_WR_RDMA_WRITE;
        wr[i].wr.rdma.remote_addr = t->remote_addr + p[i].offset;
        wr[i].wr.rdma.rkey = t->rkey;
        wr[i].next = i+1 < n ? &wr[i+1] : NULL;
        wr[i].send_flags = (i+1 == n ? IBV_SEND_SIGNALED : 0) | (p[i].size <= 16 ? IBV_SEND_INLINE : 0);
    }
    rc = ibv_post_send(t->qp, wr, &bad);
    if (rc) return -rc; /* A partial post is fatal: the caller must not reuse buffers. */
    unsigned slot = t->posted % SB_RDMA_TX_DEPTH;
    t->cookie[slot] = cookie; t->started[slot] = sb_rdma_clock();
    t->posted++;
    if (sb_rdma_tx_pending(t) > t->maximum) t->maximum = sb_rdma_tx_pending(t);
    return 0;
}
static inline int sb_rdma_tx_poll(struct sb_rdma_tx *t, uint64_t cookies[SB_RDMA_TX_DEPTH])
{
    struct ibv_wc wc[SB_RDMA_TX_DEPTH];
    int n = ibv_poll_cq(t->cq, SB_RDMA_TX_DEPTH, wc);
    if (n < 0) return -EIO;
    for (int i = 0; i < n; i++) {
        if (wc[i].status != IBV_WC_SUCCESS || wc[i].wr_id != t->completed+1 || t->completed == t->posted)
            return -EIO;
        cookies[i] = t->cookie[t->completed % SB_RDMA_TX_DEPTH];
        t->completed++;
    }
    return n;
}
static inline int sb_rdma_tx_expired(const struct sb_rdma_tx *t)
{
    return sb_rdma_tx_pending(t) && sb_rdma_clock() - t->started[t->completed % SB_RDMA_TX_DEPTH] > UINT64_C(15000000000);
}
#endif
