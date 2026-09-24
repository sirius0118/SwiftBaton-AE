/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_RDMA_H
#define SBK_RDMA_H
#include <linux/mm.h>
#include "sbk_uapi.h"
struct sbk_rdma;
struct sbk_rdma_slot;
struct sbk_dispatch;
bool sbk_session_dispatch_enabled(void);
struct sbk_dispatch *sbk_rdma_dispatch(struct sbk_rdma *r, unsigned int lane);
int sbk_rdma_dispatch_stats(struct sbk_rdma *r, struct sbk_dispatch_stats *stats);
struct sbk_rdma *sbk_rdma_create(struct sbk_rdma_setup *setup);
int sbk_rdma_connect(struct sbk_rdma *r, const struct sbk_rdma_endpoint *peer);
bool sbk_rdma_ready(struct sbk_rdma *r, u64 pages);
/* Callbacks run inline without a transport lock. Both must be bounded and must
 * not sleep, wait on DMA, or acquire a QP. page_ready follows successful WC and
 * DMA unmap for that page; transport never accesses the page afterwards. */
struct sbk_rdma_notify {
	void (*posted)(void *);
	void (*page_ready)(void *, unsigned int);
	void *cookie;
};
/* Reserve speculative capacity before claiming a page. Waiting for admission
 * must leave queued pages stealable by demand. A live context retains r; the
 * caller must release its slot on every path, including empty/stolen batches.
 * Never hold a reservation across PTE installation or another slot acquire. */
int sbk_rdma_reserve(struct sbk_rdma *r, unsigned int lane, struct sbk_rdma_slot **slot);
void sbk_rdma_release(struct sbk_rdma *r, unsigned int lane, struct sbk_rdma_slot *slot);
int sbk_rdma_read_reserved(struct sbk_rdma *r, unsigned int lane, struct sbk_rdma_slot *slot,
		  struct page **pages, const unsigned long *indices, unsigned int count,
		  const struct sbk_rdma_region *region, const struct sbk_rdma_notify *notify);
int sbk_rdma_read_notify(struct sbk_rdma *r, unsigned int lane, struct page **pages,
		  const unsigned long *indices, unsigned int count,
		  const struct sbk_rdma_region *region, const struct sbk_rdma_notify *notify);
static inline int sbk_rdma_read(struct sbk_rdma *r, unsigned int lane, struct page **pages,
		  const unsigned long *indices, unsigned int count,
		  const struct sbk_rdma_region *region)
{
	return sbk_rdma_read_notify(r, lane, pages, indices, count, region, NULL);
}
void sbk_rdma_cancel(struct sbk_rdma *r);
int sbk_rdma_revoke_source(struct sbk_rdma *r);
int sbk_rdma_export_region(struct sbk_rdma *r, struct sbk_rdma_region *region);
int sbk_rdma_get_region(struct sbk_rdma *r, const struct sbk_rdma_region *region);
void sbk_rdma_get(struct sbk_rdma *r);
void sbk_rdma_put(struct sbk_rdma *r);
#endif
