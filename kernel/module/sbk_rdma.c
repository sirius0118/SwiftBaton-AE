// SPDX-License-Identifier: GPL-2.0
/* RC read transport. Each lane has independent exclusive QP/CQ slots.
 * No page is published or DMA-unmapped while a WR can still access it.
 */
#include <linux/module.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/overflow.h>
#include <linux/kref.h>
#include <linux/cred.h>
#include <linux/rwsem.h>
#include <rdma/ib_verbs.h>
#include <rdma/ib_cache.h>
#include "sbk_rdma.h"
#include "sbk_dispatch.h"

static unsigned int rdma_ack_timeout = 12;
static unsigned int rdma_retry_count = 3;
static bool rdma_debug;
static bool session_dispatch = true;
module_param(session_dispatch, bool, 0444);
MODULE_PARM_DESC(session_dispatch, "Fixed FT/BG CPU workers per destination session; false uses legacy per-region workqueues");
bool sbk_session_dispatch_enabled(void) { return session_dispatch; }
module_param(rdma_ack_timeout, uint, 0444);
module_param(rdma_retry_count, uint, 0444);
module_param(rdma_debug, bool, 0444);
MODULE_PARM_DESC(rdma_ack_timeout, "RC ACK timeout exponent, 0..31; default 12");
MODULE_PARM_DESC(rdma_retry_count, "RC retry count, 0..7; default 3");

struct sbk_rdma;
struct sbk_exported_region {
	struct list_head link;
	struct ib_mr *mr;
};
struct sbk_rdma_slot {
	struct sbk_rdma *owner;
	struct ib_cq *cq;
	struct ib_qp *qp;
	atomic_t busy, failed;
	u64 sequence;
	/* Slot-local arrays keep batch posting off the small kernel stack. */
	struct ib_rdma_wr wr[SBK_MAX_BATCH];
	struct ib_sge sge[SBK_MAX_BATCH];
	dma_addr_t dma[SBK_MAX_BATCH];
	struct page *retained[SBK_MAX_BATCH];
};
struct sbk_slot_waiter {
	struct list_head link;
	struct sbk_rdma_slot *slot;
};
struct sbk_rdma {
	struct kref refs;
	struct sbk_dispatch *dispatch[SBK_LANES];
	struct list_head exports;
	/* Readers may pin independent MRs concurrently; revocation waits for all. */
	struct rw_semaphore exports_sem;
	struct mutex exports_lock; /* only ID reservation/list publication */
	u32 next_region, exporting, export_peak;
	/* Source controller delegates only MR registration through its fd. */
	const struct cred *registration_cred;
	struct ib_device *dev;
	struct ib_pd *pd;
	struct ib_mr *mr;
	const struct ib_gid_attr *gid;
	struct sbk_rdma_setup cfg;
	struct sbk_rdma_endpoint peer;
	struct sbk_rdma_slot slot[SBK_LANES][SBK_RDMA_MAX_SLOTS];
	wait_queue_head_t available[SBK_LANES];
	spinlock_t slot_lock[SBK_LANES];
	struct list_head slot_waiters[SBK_LANES];
	u64 slot_progress[SBK_LANES];
	atomic_t stopped, quarantined;
	bool ready, connect_attempted;
};

static void sbk_cq_handler(struct ib_cq *cq, void *data) { }
static void sbk_async_event(struct ib_event *event, void *data)
{
	struct sbk_rdma_slot *s = data;
	atomic_set(&s->failed, 1);
}

/* Caller has exclusive slot ownership. Destroy synchronously before unmap. */
static void sbk_quarantine(struct sbk_rdma *r, int error)
{
	if (atomic_cmpxchg(&r->quarantined, 0, 1) == 0) {
		/* Fail closed: retain resources/code rather than risk late DMA or callbacks. */
		__module_get(THIS_MODULE);
		pr_err("swiftbaton_k: RDMA teardown error %d; session quarantined, resources retained\n", error);
	}
	sbk_rdma_cancel(r);
}

static int sbk_slot_quiesce(struct sbk_rdma_slot *s)
{
	int ret;
	atomic_set(&s->failed, 1);
	if (s->qp) {
		struct ib_qp_attr a = {.qp_state = IB_QPS_ERR};
		ib_modify_qp(s->qp, &a, IB_QP_STATE);
		/* No shared SRQ/MW/users; destruction synchronizes device access. */
		ret = ib_destroy_qp(s->qp);
		if (ret) {
			sbk_quarantine(s->owner, ret);
			return ret;
		}
		s->qp = NULL;
	}
	return 0;
}

void sbk_rdma_cancel(struct sbk_rdma *r)
{
	int lane;
	if (!r)
		return;
	atomic_set(&r->stopped, 1);
	for (lane = 0; lane < SBK_LANES; lane++)
		wake_up_all(&r->available[lane]);
}

int sbk_rdma_revoke_source(struct sbk_rdma *r)
{
	struct sbk_exported_region *region;
	int ret = 0;
	if (!r || r->cfg.role != SBK_RDMA_SOURCE)
		return -EINVAL;
	/* Close admission before waiting for in-flight ib_reg_user_mr calls.
	 * Each export retains the transport and holds the read side through list
	 * publication, so every successful registration is covered by this revoke. */
	sbk_rdma_cancel(r);
	down_write(&r->exports_sem);
	if (r->mr) {
		ret = ib_dereg_mr(r->mr);
		if (ret)
			goto out;
		r->mr = NULL;
	}
	list_for_each_entry(region, &r->exports, link) {
		if (!region->mr)
			continue;
		ret = ib_dereg_mr(region->mr);
		if (ret)
			goto out;
		region->mr = NULL;
	}
out:
	up_write(&r->exports_sem);
	return ret;
}

static void sbk_rdma_destroy(struct kref *refs)
{
	struct sbk_rdma *r = container_of(refs, struct sbk_rdma, refs);
	struct sbk_exported_region *region, *next;
	int lane, i, ret;
	sbk_rdma_cancel(r);
	if (atomic_read(&r->quarantined))
		return;
	if (rdma_debug && r->cfg.role == SBK_RDMA_SOURCE)
		pr_info("swiftbaton_k: export_summary regions=%u peak=%u\n",
			r->next_region, r->export_peak);
	/* Page engine has joined only its own region jobs before dropping each
	 * reference. The last release joins fixed workers before freeing r. */
	for (lane = SBK_PREFETCH; lane <= SBK_BACKGROUND; lane++)
		sbk_dispatch_destroy(r->dispatch[lane]);
	/* Page engine has joined all users before this is called. */
	for (lane = 0; lane < SBK_LANES; lane++)
		for (i = 0; i < SBK_RDMA_MAX_SLOTS; i++) {
			struct sbk_rdma_slot *s = &r->slot[lane][i];
			if (sbk_slot_quiesce(s))
				return;
			if (s->cq) {
				ret = ib_destroy_cq_user(s->cq, NULL);
				if (ret)
					goto quarantine;
				s->cq = NULL;
			}
		}
	if (r->mr) {
		ret = ib_dereg_mr(r->mr);
		if (ret)
			goto quarantine;
		r->mr = NULL;
	}
	list_for_each_entry_safe(region, next, &r->exports, link) {
		if (region->mr) {
			ret = ib_dereg_mr(region->mr);
			if (ret)
				goto quarantine;
		}
		list_del(&region->link);
		kfree(region);
	}
	if (r->pd) {
		ret = ib_dealloc_pd_user(r->pd, NULL);
		if (ret)
			goto quarantine;
		r->pd = NULL;
	}
	if (r->gid)
		rdma_put_gid_attr(r->gid);
	if (r->dev)
		ib_device_put(r->dev);
	if (r->registration_cred)
		put_cred(r->registration_cred);
	kvfree(r);
	return;
quarantine:
	sbk_quarantine(r, ret);
}

/* Caller already owns a live reference, normally under context control. */
void sbk_rdma_get(struct sbk_rdma *r)
{
	kref_get(&r->refs);
}

void sbk_rdma_put(struct sbk_rdma *r)
{
	if (r)
		kref_put(&r->refs, sbk_rdma_destroy);
}

static bool sbk_region_valid(const struct sbk_rdma_region *region)
{
	u64 bytes, end;
	return region && region->pages && region->pages <= (1ULL << 20) &&
		!(region->address & ~PAGE_MASK) &&
		!check_mul_overflow(region->pages, (u64)PAGE_SIZE, &bytes) &&
		!check_add_overflow(region->address, bytes, &end);
}

/* Caller owns a transport reference; current->mm is the region's owner.
 * No context control lock is held while ib_reg_user_mr faults/pins pages. */
int sbk_rdma_export_region(struct sbk_rdma *r, struct sbk_rdma_region *desc)
{
	struct sbk_exported_region *region;
	const struct cred *saved;
	int ret = 0;
	if (!r || r->cfg.role != SBK_RDMA_SOURCE || r->cfg.pages ||
	    !sbk_region_valid(desc) || desc->id || desc->rkey)
		return -EINVAL;
	down_read(&r->exports_sem);
	if (atomic_read(&r->stopped)) {
		ret = -ECANCELED;
		goto unlock;
	}
	region = kzalloc(sizeof(*region), GFP_KERNEL);
	if (!region) {
		ret = -ENOMEM;
		goto unlock;
	}
	mutex_lock(&r->exports_lock);
	if (r->next_region + r->exporting >= SBK_MAX_REGIONS) {
		mutex_unlock(&r->exports_lock);
		kfree(region);
		ret = -ENOSPC;
		goto unlock;
	}
	r->exporting++;
	r->export_peak = max(r->export_peak, r->exporting);
	mutex_unlock(&r->exports_lock);
	/* Only delegate the controller's mlock authority, preserving the calling
	 * process's mm and pinned_vm accounting. No persistent credential change. */
	saved = override_creds(r->registration_cred);
	region->mr = ib_reg_user_mr(r->pd, desc->address, desc->pages << PAGE_SHIFT,
				  desc->address, IB_ACCESS_REMOTE_READ);
	revert_creds(saved);
	mutex_lock(&r->exports_lock);
	r->exporting--;
	if (IS_ERR(region->mr)) {
		ret = PTR_ERR(region->mr);
		kfree(region);
	} else {
		/* Keep even a concurrently cancelled registration on the ownership
		 * list. The revoker waits for this publication and deregisters it.
		 * A failed copyout is likewise owned until revoke/close. */
		list_add_tail(&region->link, &r->exports);
		r->next_region++;
		if (atomic_read(&r->stopped))
			ret = -ECANCELED;
		else {
			desc->rkey = region->mr->rkey;
			desc->id = r->next_region;
		}
	}
	mutex_unlock(&r->exports_lock);
unlock:
	up_read(&r->exports_sem);
	return ret;
}

/* Parent file is pinned and control-locked; the returned reference survives
 * closing that file and every sibling view owns an independent reference. */
int sbk_rdma_get_region(struct sbk_rdma *r, const struct sbk_rdma_region *region)
{
	if (!sbk_rdma_ready(r, 0) || !sbk_region_valid(region) ||
	    !region->id || region->id > SBK_MAX_REGIONS)
		return -EINVAL;
	kref_get(&r->refs);
	return 0;
}

static int sbk_slot_create(struct sbk_rdma *r, unsigned int lane, unsigned int i)
{
	struct sbk_rdma_slot *s = &r->slot[lane][i];
	struct ib_cq_init_attr ca = {.cqe = SBK_MAX_BATCH + 4};
	struct ib_qp_init_attr qa = {};
	struct ib_qp_attr a = {};
	int err;
	s->owner = r;
	s->cq = ib_create_cq(r->dev, sbk_cq_handler, sbk_async_event, s, &ca);
	if (IS_ERR(s->cq)) {
		err = PTR_ERR(s->cq);
		s->cq = NULL;
		return err;
	}
	qa.event_handler = sbk_async_event;
	qa.qp_context = s;
	qa.send_cq = qa.recv_cq = s->cq;
	qa.cap.max_send_wr = SBK_MAX_BATCH;
	qa.cap.max_recv_wr = 1;
	qa.cap.max_send_sge = qa.cap.max_recv_sge = 1;
	qa.qp_type = IB_QPT_RC;
	qa.sq_sig_type = IB_SIGNAL_REQ_WR;
	s->qp = ib_create_qp(r->pd, &qa);
	if (IS_ERR(s->qp)) {
		err = PTR_ERR(s->qp);
		s->qp = NULL;
		return err;
	}
	a.qp_state = IB_QPS_INIT;
	a.port_num = r->cfg.port;
	a.qp_access_flags = IB_ACCESS_REMOTE_READ;
	err = ib_modify_qp(s->qp, &a, IB_QP_STATE | IB_QP_PORT | IB_QP_PKEY_INDEX | IB_QP_ACCESS_FLAGS);
	if (!err)
		r->cfg.local.qpn[lane][i] = s->qp->qp_num;
	return err;
}

struct sbk_rdma *sbk_rdma_create(struct sbk_rdma_setup *setup)
{
	struct sbk_rdma *r;
	struct ib_port_attr pa;
	struct net_device *ndev;
	u64 bytes, end;
	int lane, i, err;
	if (rdma_ack_timeout > 31 || rdma_retry_count > 7 ||
	    !memchr(setup->device, 0, sizeof(setup->device)) ||
	    (setup->role != SBK_RDMA_SOURCE && setup->role != SBK_RDMA_DESTINATION) ||
	    !setup->port || setup->port > 255 || setup->gid_index > 65535 ||
	    setup->timeout_ms < 10 || setup->timeout_ms > 30000 ||
	    setup->pages > (1ULL << 20) || (!setup->pages && setup->source_address) ||
	    (setup->source_address & ~PAGE_MASK) ||
	    check_mul_overflow(setup->pages, (u64)PAGE_SIZE, &bytes) ||
	    check_add_overflow(setup->source_address, bytes, &end))
		return ERR_PTR(-EINVAL);
	for (lane = 0; lane < SBK_LANES; lane++)
		if (!setup->slots[lane] || setup->slots[lane] > SBK_RDMA_MAX_SLOTS ||
		    setup->traffic_class[lane] > 255)
			return ERR_PTR(-EINVAL);
	r = kvzalloc(sizeof(*r), GFP_KERNEL);
	if (!r)
		return ERR_PTR(-ENOMEM);
	kref_init(&r->refs);
	INIT_LIST_HEAD(&r->exports);
	init_rwsem(&r->exports_sem);
	mutex_init(&r->exports_lock);
	r->cfg = *setup;
	if (setup->role == SBK_RDMA_SOURCE)
		r->registration_cred = get_current_cred();
	memset(&r->cfg.local, 0, sizeof(r->cfg.local));
	for (lane = 0; lane < SBK_LANES; lane++) {
		init_waitqueue_head(&r->available[lane]);
		spin_lock_init(&r->slot_lock[lane]);
		INIT_LIST_HEAD(&r->slot_waiters[lane]);
	}
	r->dev = ib_device_get_by_name(setup->device, RDMA_DRIVER_UNKNOWN);
	if (!r->dev) {
		err = -ENODEV;
		goto fail;
	}
	err = ib_query_port(r->dev, setup->port, &pa);
	if (err)
		goto fail;
	if (rdma_port_get_link_layer(r->dev, setup->port) != IB_LINK_LAYER_ETHERNET ||
	    pa.state != IB_PORT_ACTIVE) {
		err = -ENETDOWN;
		goto fail;
	}
	r->gid = rdma_get_gid_attr(r->dev, setup->port, setup->gid_index);
	if (IS_ERR(r->gid)) {
		err = PTR_ERR(r->gid);
		r->gid = NULL;
		goto fail;
	}
	if (r->gid->gid_type != IB_GID_TYPE_ROCE_UDP_ENCAP) {
		err = -EPROTONOSUPPORT;
		goto fail;
	}
	rcu_read_lock();
	ndev = rdma_read_gid_attr_ndev_rcu(r->gid);
	if (!IS_ERR(ndev))
		memcpy(r->cfg.local.mac, ndev->dev_addr, ETH_ALEN);
	rcu_read_unlock();
	if (IS_ERR(ndev)) {
		err = PTR_ERR(ndev);
		goto fail;
	}
	r->pd = ib_alloc_pd(r->dev, 0);
	if (IS_ERR(r->pd)) {
		err = PTR_ERR(r->pd);
		r->pd = NULL;
		goto fail;
	}
	if (setup->role == SBK_RDMA_SOURCE && setup->pages) {
		/* Caller owns current->mm; pages must be immutable until session close. */
		r->mr = ib_reg_user_mr(r->pd, setup->source_address, bytes,
				       setup->source_address, IB_ACCESS_REMOTE_READ);
		if (IS_ERR(r->mr)) {
			err = PTR_ERR(r->mr);
			r->mr = NULL;
			goto fail;
		}
		r->cfg.local.address = setup->source_address;
		r->cfg.local.rkey = r->mr->rkey;
	}
	r->cfg.local.version = SBK_ABI_VERSION;
	r->cfg.local.role = setup->role;
	r->cfg.local.pages = setup->pages;
	r->cfg.local.mtu = pa.active_mtu;
	memcpy(r->cfg.local.gid, r->gid->gid.raw, 16);
	memcpy(r->cfg.local.slots, setup->slots, sizeof(setup->slots));
	for (lane = 0; lane < SBK_LANES; lane++)
		for (i = 0; i < setup->slots[lane]; i++) {
			err = sbk_slot_create(r, lane, i);
			if (err)
				goto fail;
		}
	if (session_dispatch && setup->role == SBK_RDMA_DESTINATION) {
		for (lane = SBK_PREFETCH; lane <= SBK_BACKGROUND; lane++) {
			r->dispatch[lane] = sbk_dispatch_create(setup->slots[lane], lane == SBK_PREFETCH);
			if (IS_ERR(r->dispatch[lane])) {
				err = PTR_ERR(r->dispatch[lane]);
				r->dispatch[lane] = NULL;
				goto fail;
			}
		}
	}
	setup->local = r->cfg.local;
	return r;
fail:
	sbk_rdma_put(r);
	return ERR_PTR(err);
}

int sbk_rdma_connect(struct sbk_rdma *r, const struct sbk_rdma_endpoint *p)
{
	struct ib_qp_attr a;
	u64 bytes, end;
	int lane, i, err;
	if (!r || r->connect_attempted || atomic_read(&r->stopped))
		return -EINVAL;
	if (p->version != SBK_ABI_VERSION || p->role + r->cfg.role != 3 ||
	    p->pages != r->cfg.pages || p->mtu < IB_MTU_256 || p->mtu > IB_MTU_4096 ||
	    (!p->pages && (p->address || p->rkey)) ||
	    !is_valid_ether_addr(p->mac) || (p->address & ~PAGE_MASK) ||
	    check_mul_overflow(p->pages, (u64)PAGE_SIZE, &bytes) ||
	    check_add_overflow(p->address, bytes, &end))
		return -EINVAL;
	for (lane = 0; lane < SBK_LANES; lane++) {
		if (p->slots[lane] != r->cfg.slots[lane])
			return -EINVAL;
		for (i = 0; i < p->slots[lane]; i++)
			if (!p->qpn[lane][i] || p->qpn[lane][i] > 0xffffff)
				return -EINVAL;
	}
	r->connect_attempted = true;
	r->peer = *p;
	for (lane = 0; lane < SBK_LANES; lane++)
		for (i = 0; i < p->slots[lane]; i++) {
			memset(&a, 0, sizeof(a));
			a.qp_state = IB_QPS_RTR;
			a.path_mtu = min(r->cfg.local.mtu, p->mtu);
			a.dest_qp_num = p->qpn[lane][i];
			a.max_dest_rd_atomic = 1;
			a.min_rnr_timer = 12;
			a.ah_attr.type = RDMA_AH_ATTR_TYPE_ROCE;
			a.ah_attr.ah_flags = IB_AH_GRH;
			a.ah_attr.port_num = r->cfg.port;
			a.ah_attr.grh.hop_limit = 64;
			a.ah_attr.grh.sgid_index = r->cfg.gid_index;
			a.ah_attr.grh.sgid_attr = r->gid;
			a.ah_attr.grh.traffic_class = r->cfg.traffic_class[lane];
			memcpy(a.ah_attr.grh.dgid.raw, p->gid, 16);
			memcpy(a.ah_attr.roce.dmac, p->mac, ETH_ALEN);
			err = ib_modify_qp(r->slot[lane][i].qp, &a,
				IB_QP_STATE | IB_QP_AV | IB_QP_PATH_MTU | IB_QP_DEST_QPN |
				IB_QP_RQ_PSN | IB_QP_MAX_DEST_RD_ATOMIC | IB_QP_MIN_RNR_TIMER);
			if (err)
				goto fail;
			memset(&a, 0, sizeof(a));
			a.qp_state = IB_QPS_RTS;
			a.timeout = rdma_ack_timeout;
			a.retry_cnt = rdma_retry_count;
			a.rnr_retry = 3;
			a.max_rd_atomic = 1;
			err = ib_modify_qp(r->slot[lane][i].qp, &a,
				IB_QP_STATE | IB_QP_TIMEOUT | IB_QP_RETRY_CNT | IB_QP_RNR_RETRY |
				IB_QP_SQ_PSN | IB_QP_MAX_QP_RD_ATOMIC);
			if (err)
				goto fail;
		}
	smp_store_release(&r->ready, true);
	return 0;
fail:
	sbk_rdma_cancel(r);
	return err;
}

bool sbk_rdma_ready(struct sbk_rdma *r, u64 pages)
{
	return r && r->cfg.role == SBK_RDMA_DESTINATION &&
		smp_load_acquire(&r->ready) && !atomic_read(&r->stopped) && r->cfg.pages == pages;
}

/* Handoff, rather than wake-and-race, prevents a hot VMA from repeatedly
 * stealing both slots from other regions. Each traffic lane has its own lock;
 * it is never held across allocation, CQ polling, or a network operation. */
void sbk_rdma_release(struct sbk_rdma *r, unsigned int lane,
			     struct sbk_rdma_slot *s)
{
	unsigned long flags;
	spin_lock_irqsave(&r->slot_lock[lane], flags);
	r->slot_progress[lane]++;
	if (!atomic_read(&r->stopped) && !list_empty(&r->slot_waiters[lane])) {
		struct sbk_slot_waiter *w = list_first_entry(&r->slot_waiters[lane],
							   struct sbk_slot_waiter, link);
		list_del_init(&w->link);
		WRITE_ONCE(w->slot, s); /* Busy remains set: only this waiter owns it. */
	} else {
		atomic_set(&s->busy, 0);
	}
	spin_unlock_irqrestore(&r->slot_lock[lane], flags);
	wake_up_all(&r->available[lane]);
}

static int sbk_acquire_slot(struct sbk_rdma *r, unsigned int lane,
			    struct sbk_rdma_slot **slot)
{
	struct sbk_slot_waiter w = {.slot = NULL};
	unsigned long flags;
	u64 started = ktime_get_ns(), progress, initial_progress;
	long waited;
	int i, ret;
	INIT_LIST_HEAD(&w.link);
	spin_lock_irqsave(&r->slot_lock[lane], flags);
	if (atomic_read(&r->stopped)) {
		spin_unlock_irqrestore(&r->slot_lock[lane], flags);
		return -ECANCELED;
	}
	if (list_empty(&r->slot_waiters[lane])) {
		for (i = 0; i < r->cfg.slots[lane]; i++) {
			if (!atomic_read(&r->slot[lane][i].busy)) {
				atomic_set(&r->slot[lane][i].busy, 1);
				*slot = &r->slot[lane][i];
				spin_unlock_irqrestore(&r->slot_lock[lane], flags);
				return 0;
			}
		}
	}
	list_add_tail(&w.link, &r->slot_waiters[lane]);
	initial_progress = progress = r->slot_progress[lane];
	spin_unlock_irqrestore(&r->slot_lock[lane], flags);
	/* Admission is backpressure, not a posted WR. A large but progressing
 * FIFO must not turn into SIGBUS just because its total queue time exceeds
 * one WR timeout. Fail if the lane makes no progress for that interval. */
	do {
		waited = wait_event_killable_timeout(r->available[lane],
			READ_ONCE(w.slot) || atomic_read(&r->stopped) ||
			READ_ONCE(r->slot_progress[lane]) != progress,
			msecs_to_jiffies(r->cfg.timeout_ms));
		progress = READ_ONCE(r->slot_progress[lane]);
	} while (waited > 0 && !READ_ONCE(w.slot) && !atomic_read(&r->stopped));
	/* Timeout/signal can race with handoff. The queue lock decides ownership
 * before this stack waiter disappears; a handed-off slot must be released. */
	spin_lock_irqsave(&r->slot_lock[lane], flags);
	list_del_init(&w.link);
	*slot = w.slot;
	ret = atomic_read(&r->stopped) ? -ECANCELED :
		w.slot ? 0 : waited < 0 ? waited : -ETIMEDOUT;
	spin_unlock_irqrestore(&r->slot_lock[lane], flags);
	if (ret) {
		if (*slot) {
			sbk_rdma_release(r, lane, *slot);
			*slot = NULL;
		}
		pr_err_ratelimited("swiftbaton_k: RDMA slot wait failed lane=%u error=%d progress=%llu elapsed_ms=%llu\n",
			lane, ret, progress - initial_progress, (ktime_get_ns() - started) / NSEC_PER_MSEC);
	} else if (rdma_debug && ktime_get_ns() - started >= 500 * NSEC_PER_MSEC) {
		pr_info("swiftbaton_k: RDMA slot admitted lane=%u progress=%llu elapsed_ms=%llu\n",
			lane, progress - initial_progress, (ktime_get_ns() - started) / NSEC_PER_MSEC);
	}
	return ret;
}

int sbk_rdma_reserve(struct sbk_rdma *r, unsigned int lane, struct sbk_rdma_slot **slot)
{
	*slot = NULL;
	if (!r || lane >= SBK_LANES || !sbk_rdma_ready(r, r->cfg.pages))
		return -ENOTCONN;
	return sbk_acquire_slot(r, lane, slot);
}

int sbk_rdma_read_reserved(struct sbk_rdma *r, unsigned int lane,
		  struct sbk_rdma_slot *s, struct page **pages,
		  const unsigned long *indices, unsigned int count,
		  const struct sbk_rdma_region *region, const struct sbk_rdma_notify *notify)
{
	const struct ib_send_wr *bad;
	struct ib_wc wc;
	u64 deadline, sequence;
	unsigned long mapped = 0, done = 0;
	unsigned int i, completed = 0, last_wc_status = ~0U;
	int err = 0, n;
	if (!r || !s || lane >= SBK_LANES || !count || count > SBK_MAX_BATCH ||
	    !sbk_rdma_ready(r, r->cfg.pages) ||
	    (region ? r->cfg.pages != 0 : r->cfg.pages == 0))
		return -ENOTCONN;
	for (i = 0; i < count; i++)
		if (indices[i] >= (region ? region->pages : r->peer.pages))
			return -EINVAL;
	/* Queue admission and DMA completion have independent bounded deadlines.
 * Application fault timing continues to include both intervals. */
	deadline = ktime_get_ns() + (u64)r->cfg.timeout_ms * NSEC_PER_MSEC;
	if (atomic_read(&s->failed)) {
		err = -EIO;
		goto out;
	}
	sequence = ++s->sequence & (U64_MAX / SBK_MAX_BATCH);
	for (i = 0; i < count; i++) {
		s->dma[i] = ib_dma_map_page(r->dev, pages[i], 0, PAGE_SIZE, DMA_FROM_DEVICE);
		if (ib_dma_mapping_error(r->dev, s->dma[i])) {
			err = -EIO;
			goto out;
		}
		mapped |= BIT(i);
		s->sge[i].addr = s->dma[i];
		s->sge[i].length = PAGE_SIZE;
		s->sge[i].lkey = r->pd->local_dma_lkey;
		memset(&s->wr[i], 0, sizeof(s->wr[i]));
		s->wr[i].wr.wr_id = sequence * SBK_MAX_BATCH + i;
		s->wr[i].wr.next = i + 1 < count ? &s->wr[i + 1].wr : NULL;
		s->wr[i].wr.sg_list = &s->sge[i];
		s->wr[i].wr.num_sge = 1;
		s->wr[i].wr.opcode = IB_WR_RDMA_READ;
		s->wr[i].wr.send_flags = IB_SEND_SIGNALED;
		s->wr[i].remote_addr = (region ? region->address : r->peer.address) +
					((u64)indices[i] << PAGE_SHIFT);
		s->wr[i].rkey = region ? region->rkey : r->peer.rkey;
	}
	err = ib_post_send(s->qp, &s->wr[0].wr, &bad);
	if (err)
		goto quiesce; /* Even a failing post can have submitted a prefix. */
	/* The demand WR is already on its independent QP before speculative work
	 * is queued. Its DMA can progress while four bounded FT enqueues execute.
	 * No transport/slot lock is held here; the completion deadline includes it. */
	if (notify && notify->posted)
		notify->posted(notify->cookie);
	while (completed < count) {
		n = ib_poll_cq(s->cq, 1, &wc);
		if (n > 0)
			last_wc_status = wc.status;
		if (n < 0 || (n && (wc.status != IB_WC_SUCCESS ||
		    wc.wr_id / SBK_MAX_BATCH != sequence))) {
			err = -EIO;
			goto quiesce;
		}
		if (atomic_read(&r->stopped) || atomic_read(&s->failed) ||
		    ktime_get_ns() >= deadline) {
			err = atomic_read(&r->stopped) ? -ECANCELED : -ETIMEDOUT;
			goto quiesce;
		}
		if (n) {
			i = wc.wr_id % SBK_MAX_BATCH;
			if (i >= count || (done & BIT(i))) {
				err = -EIO;
				goto quiesce;
			}
			ib_dma_unmap_page(r->dev, s->dma[i], PAGE_SIZE, DMA_FROM_DEVICE);
			mapped &= ~BIT(i);
			done |= BIT(i);
			completed++;
			/* A blocked application may adopt/free this completed page as
			 * soon as it is published, even while other WRs remain in flight. */
			if (notify && notify->page_ready)
				notify->page_ready(notify->cookie, i);
		}
		cpu_relax();
		cond_resched();
	}
	goto out;
quiesce:
	pr_err_ratelimited("swiftbaton_k: RDMA failure lane=%u posted=%u completed=%u error=%d last_wc=%u\n",
		lane, count, completed, err, last_wc_status);
	if (sbk_slot_quiesce(s)) {
		for (i = 0; i < count; i++)
			if (mapped & BIT(i)) {
				get_page(pages[i]);
				s->retained[i] = pages[i];
			}
		/* DMA mappings are deliberately retained with the quarantined pages. */
		mapped = 0;
	}
	/* Fail the whole session; never silently continue after an uncertain DMA. */
	sbk_rdma_cancel(r);
out:
	for (i = 0; i < count; i++)
		if (mapped & BIT(i))
			ib_dma_unmap_page(r->dev, s->dma[i], PAGE_SIZE, DMA_FROM_DEVICE);
	return err;
}

int sbk_rdma_read_notify(struct sbk_rdma *r, unsigned int lane, struct page **pages,
		  const unsigned long *indices, unsigned int count,
		  const struct sbk_rdma_region *region, const struct sbk_rdma_notify *notify)
{
	struct sbk_rdma_slot *slot;
	int err = sbk_rdma_reserve(r, lane, &slot);
	if (err)
		return err;
	err = sbk_rdma_read_reserved(r, lane, slot, pages, indices, count, region, notify);
	sbk_rdma_release(r, lane, slot);
	return err;
}

struct sbk_dispatch *sbk_rdma_dispatch(struct sbk_rdma *r, unsigned int lane)
{
	if (!r || lane < SBK_PREFETCH || lane > SBK_BACKGROUND)
		return NULL;
	return r->dispatch[lane];
}
int sbk_rdma_dispatch_stats(struct sbk_rdma *r, struct sbk_dispatch_stats *stats)
{
	unsigned int lane;
	if (!r || !r->dispatch[SBK_PREFETCH] || !r->dispatch[SBK_BACKGROUND])
		return -EOPNOTSUPP;
	for (lane = SBK_PREFETCH; lane <= SBK_BACKGROUND; lane++)
		sbk_dispatch_stats(r->dispatch[lane], &stats->lane[lane - SBK_PREFETCH]);
	return 0;
}
