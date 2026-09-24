// SPDX-License-Identifier: GPL-2.0
/* SwiftBaton-K page engine. LOOPBACK_TEST is only a correctness fixture. */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/fs.h>
#include <linux/file.h>
#include <linux/eventfd.h>
#include <linux/mm.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/highmem.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include <linux/uaccess.h>
#include <linux/kref.h>
#include <linux/workqueue.h>
#include <linux/wait.h>
#include <linux/ktime.h>
#include <linux/delay.h>
#include <linux/bitmap.h>
#include <linux/overflow.h>
#ifdef CONFIG_SWIFTBATON_PTE
#include <linux/swiftbaton_pte.h>
#endif
#include "sbk_uapi.h"
#include "sbk_rdma.h"
#include "sbk_dispatch.h"

#define SBK_MAX_PAGES (1UL << 20)
#define SBK_MAX_WORKERS 32
#define SBK_WAIT_TIMEOUT (30 * HZ)

enum sbk_state { REMOTE, QUEUED_BG, QUEUED_FT, INFLIGHT, READY, FAILED, ADOPTED };
struct sbk_context;
struct sbk_entry {
	atomic_t state;
	struct page *page;
	struct sbk_context *ctx;
	unsigned long index;
	wait_queue_head_t wait;
	union { struct work_struct prefetch; struct sbk_work ft_job; };
	u64 started, completed;
	u32 lane;
	int error;
};
struct sbk_bg_worker {
	union { struct work_struct work; struct sbk_work job; };
	struct sbk_context *ctx;
};
struct sbk_context {
	struct kref refs;
	struct work_struct destroy;
	struct work_struct drain;
	union { struct work_struct coverage; struct sbk_work coverage_job; };
	struct sbk_work_owner owner;
	struct sbk_dispatch *ft_pool, *bg_pool;
	atomic_t bg_remaining;
	unsigned long coverage_cursor, hot_count;
	struct eventfd_ctx *drain_event;
	atomic_t armed, drain_started, drained, drain_signaled;
	atomic64_t retired_tokens;
	struct mutex control;
	bool config_attempted, configured, mapped, background_started, pretransferred, sealed;
	atomic_t stopping;
	struct sbk_config cfg;
	struct sbk_rdma *rdma;
	struct sbk_rdma_region region;
	bool has_region;
	struct page **source;
	unsigned long pinned;
	struct sbk_entry *entries;
	struct workqueue_struct *ft_wq, *bg_wq;
	struct sbk_bg_worker bg[SBK_MAX_WORKERS];
	unsigned long *order;
	atomic_long_t cursor;
	struct mm_struct *mm;
	unsigned long base;
	bool anonymous;
	unsigned long *token_ids, tokens_created;
	atomic64_t faults, hits, waits, errors, fetched[SBK_LANES];
	atomic64_t installed, skipped, fault_ns, fault_max, histogram[32];
	atomic64_t batches, completed, ps_pages, invalidated;
};
static struct workqueue_struct *reap_wq;
static const struct vm_operations_struct sbk_vm_ops;
static const struct file_operations sbk_fops;
static bool early_prefetch = true;
module_param(early_prefetch, bool, 0444);
MODULE_PARM_DESC(early_prefetch, "Queue neighbors after demand WR post, before completion; false retains late-trigger ablation");
/* A large MR yields after at most eight batches so another region can run. */
#define SBK_DISPATCH_BATCHES 8
static void sbk_prefetch_neighbors(struct sbk_context *c, unsigned long index);

/* Stack-local to one provider call. No per-page memory or shared lock is added.
 * The callback never outlives the blocking read/ensure operation. */
struct sbk_prefetch_request {
	struct sbk_entry *entry;
	bool sent;
};
static void sbk_prefetch_posted(void *cookie)
{
	struct sbk_prefetch_request *request = cookie;
	if (request->sent)
		return;
	request->sent = true;
	sbk_prefetch_neighbors(request->entry->ctx, request->entry->index);
}

static void sbk_max(atomic64_t *v, u64 n)
{
	s64 old = atomic64_read(v);
	while (old < n) {
		s64 seen = atomic64_cmpxchg(v, old, n);
		if (seen == old)
			break;
		old = seen;
	}
}

/* Last VMA close can hold mmap_write_lock: destruction must not flush there. */
static void sbk_destroy(struct work_struct *work)
{
	struct sbk_context *c = container_of(work, struct sbk_context, destroy);
	unsigned long i;
	atomic_set(&c->stopping, 1);
	/* Close admission, then join this region only; sibling regions stay live. */
	sbk_owner_stop(&c->owner);
	/* Shared transport survives destruction of any individual region. */
	if (c->ft_wq)
		destroy_workqueue(c->ft_wq);
	if (c->bg_wq)
		destroy_workqueue(c->bg_wq);
	sbk_rdma_put(c->rdma);
	if (c->drain_event)
		eventfd_ctx_put(c->drain_event);
	if (c->entries)
		for (i = 0; i < c->cfg.pages; i++)
			if (c->entries[i].page)
				put_page(c->entries[i].page);
	for (i = 0; i < c->pinned; i++)
		unpin_user_page(c->source[i]);
	if (c->mm)
		mmdrop(c->mm);
	kvfree(c->source);
	kvfree(c->entries);
	kvfree(c->order);
	kvfree(c->token_ids);
	kfree(c);
	/* module_exit flushes reap_wq before unloading its callback text. */
	module_put(THIS_MODULE);
}

static void sbk_release_ref(struct kref *ref)
{
	struct sbk_context *c = container_of(ref, struct sbk_context, refs);
	queue_work(reap_wq, &c->destroy);
}

#ifdef CONFIG_SWIFTBATON_PTE
static void sbk_signal_drain(struct sbk_context *c)
{
	struct eventfd_ctx *event = smp_load_acquire(&c->drain_event);
	if (event && atomic_read_acquire(&c->drained) &&
	    atomic_cmpxchg(&c->drain_signaled, 0, 1) == 0)
		eventfd_signal(event, 1);
}
static void sbk_drain_work(struct work_struct *work)
{
	struct sbk_context *c = container_of(work, struct sbk_context, drain);
	/* All markers are gone, but background reads might still be completing
	 * after munmap/discard. Join those DMA owners before allowing retirement. */
	atomic_set(&c->stopping, 1);
	if (c->bg_pool)
		sbk_owner_stop(&c->owner);
	else {
		flush_workqueue(c->ft_wq);
		flush_workqueue(c->bg_wq);
	}
	atomic_set_release(&c->drained, 1);
	sbk_signal_drain(c);
	kref_put(&c->refs, sbk_release_ref);
}
static void sbk_maybe_drain(struct sbk_context *c)
{
	if (atomic_read_acquire(&c->armed) &&
	    atomic64_read(&c->retired_tokens) == c->cfg.pages &&
	    atomic_cmpxchg(&c->drain_started, 0, 1) == 0) {
		kref_get(&c->refs);
		queue_work(reap_wq, &c->drain);
	}
}
static int sbk_watch_drain(struct sbk_context *c, void __user *user)
{
	struct eventfd_ctx *event;
	int fd;
	if (!c->configured || (c->mapped && !c->anonymous))
		return -EINVAL;
	if (c->drain_event)
		return -EALREADY;
	if (copy_from_user(&fd, user, sizeof(fd)))
		return -EFAULT;
	event = eventfd_ctx_fdget(fd);
	if (IS_ERR(event))
		return PTR_ERR(event);
	smp_store_release(&c->drain_event, event);
	sbk_signal_drain(c); /* Also handles registering after the last marker. */
	return 0;
}
#endif

static void sbk_publish(struct sbk_entry *e, int error)
{
	struct sbk_context *c = e->ctx;
	e->completed = ktime_get_ns();
	e->error = error;
	if (error) {
		if (atomic64_inc_return(&c->errors) == 1)
			pr_err("swiftbaton_k: first page error base=%lx page=%lu lane=%u error=%d stopping=%d\n",
			       c->base, e->index, e->lane, error, atomic_read(&c->stopping));
		atomic_set_release(&e->state, FAILED);
	} else {
		if (e->lane == SBK_PRETRANSFER)
			atomic64_inc(&c->ps_pages);
		else
			atomic64_inc(&c->fetched[e->lane]);
		atomic64_inc(&c->completed);
		atomic_set_release(&e->state, READY);
	}
	wake_up_all(&e->wait);
}

struct sbk_batch_progress {
	struct sbk_entry **entries;
	struct sbk_prefetch_request *prefetch;
	unsigned long published;
};
static void sbk_batch_posted(void *cookie)
{
	struct sbk_batch_progress *batch = cookie;
	if (batch->prefetch)
		sbk_prefetch_posted(batch->prefetch);
}
static void sbk_batch_page_ready(void *cookie, unsigned int index)
{
	struct sbk_batch_progress *batch = cookie;
	batch->published |= BIT(index);
	/* Only publish/wake here. PTE installation may take mmap locks and must
	 * not block polling the still-active batch's remaining completions. */
	sbk_publish(batch->entries[index], 0);
}

/* Owners only: both transports fill the pages ultimately returned by fault. */
static void sbk_fetch_batch_reserved(struct sbk_entry **entries, unsigned int count,
			    unsigned int lane, struct sbk_prefetch_request *request,
			    struct sbk_rdma_slot *slot)
{
	struct sbk_context *c = entries[0]->ctx;
	struct page *pages[SBK_MAX_BATCH];
	unsigned long indices[SBK_MAX_BATCH];
	struct sbk_batch_progress progress = {.entries = entries, .prefetch = request};
	struct sbk_rdma_notify notify = {.posted = request ? sbk_batch_posted : NULL,
		.page_ready = sbk_batch_page_ready, .cookie = &progress};
	unsigned int i;
	int err = 0;
	for (i = 0; i < count; i++) {
		struct sbk_entry *e = entries[i];
		e->lane = lane;
		e->started = ktime_get_ns();
		/* Every successful transfer writes exactly PAGE_SIZE before READY.
		 * Partial/error pages remain FAILED and are never installed. Avoid
		 * dirtying a cache line merely to overwrite it immediately via DMA. */
		e->page = alloc_page(GFP_HIGHUSER);
		pages[i] = e->page;
		indices[i] = e->index;
		if (!e->page)
			err = -ENOMEM;
	}
	if (atomic_read(&c->stopping))
		err = -ECANCELED;
	if (err)
		goto publish;
	if (c->cfg.backend == SBK_BACKEND_RDMA) {
		if (slot)
			err = sbk_rdma_read_reserved(c->rdma, lane, slot, pages, indices, count,
					c->has_region ? &c->region : NULL, &notify);
		else
			err = sbk_rdma_read_notify(c->rdma, lane == SBK_PRETRANSFER ? SBK_BACKGROUND : lane,
				    pages, indices, count, c->has_region ? &c->region : NULL,
				    &notify);
		goto publish;
	}
	if (request)
		sbk_prefetch_posted(request);
	for (i = 0; i < count; i++) {
		struct sbk_entry *e = entries[i];
		if (c->cfg.test_delay_us)
			usleep_range(c->cfg.test_delay_us, c->cfg.test_delay_us + 20);
		if (e->index == c->cfg.test_fail_page) {
			err = -EIO;
			break;
		}
		copy_highpage(e->page, c->source[e->index]);
		sbk_batch_page_ready(&progress, i);
	}
publish:
	if (atomic_read(&c->stopping))
		err = -ECANCELED;
	for (i = 0; i < count; i++)
		if (!(progress.published & BIT(i)))
			sbk_publish(entries[i], err);
}
static void sbk_fetch_batch(struct sbk_entry **entries, unsigned int count, unsigned int lane,
			    struct sbk_prefetch_request *request)
{
	sbk_fetch_batch_reserved(entries, count, lane, request, NULL);
}
static void sbk_fetch(struct sbk_entry *e, unsigned int lane,
		       struct sbk_prefetch_request *request)
{
	sbk_fetch_batch(&e, 1, lane, request);
}

static int sbk_ensure(struct sbk_entry *e, unsigned int lane, bool prefetch)
{
	struct sbk_context *c = e->ctx;
	struct sbk_prefetch_request request = {.entry = e};
	for (;;) {
		int s = atomic_read_acquire(&e->state);
		if (atomic_read(&c->stopping))
			return -ECANCELED;
		if (s == READY) {
			if (prefetch)
				sbk_prefetch_posted(&request);
			return 0;
		}
		if (s == FAILED)
			return e->error;
		if (s == ADOPTED)
			return -ESTALE;
		if (s == INFLIGHT) {
			long ret;
			/* Another owner is already fetching this page: overlap neighbors
			 * with that wait, without stealing or duplicating its transfer. */
			if (prefetch)
				sbk_prefetch_posted(&request);
			atomic64_inc(&c->waits);
			ret = wait_event_killable_timeout(e->wait,
				atomic_read_acquire(&e->state) != INFLIGHT ||
				atomic_read(&c->stopping), SBK_WAIT_TIMEOUT);
			if (ret <= 0)
				return ret ? ret : -ETIMEDOUT;
			continue;
		}
		if (atomic_cmpxchg(&e->state, s, INFLIGHT) == s) {
			sbk_fetch(e, lane, prefetch ? &request : NULL);
			continue;
		}
	}
}

/* GUP takes the ordinary file fault/COW path, never constructs private PTEs. */
static void sbk_install_ahead(struct sbk_entry *e)
{
	struct sbk_context *c = e->ctx;
	struct vm_area_struct *vma;
	struct page *page;
	unsigned long addr = c->base + (e->index << PAGE_SHIFT);
	long ret = 0;
#ifdef CONFIG_SWIFTBATON_PTE
	if (c->anonymous) {
		if (atomic_read(&c->stopping))
			return;
		ret = sbk_pte_populate(c->mm, addr, c->token_ids[e->index],
				       min_t(unsigned int, e->lane, SBK_BACKGROUND));
		if (ret)
			atomic64_inc(&c->skipped);
		return;
	}
#endif
	if (!c->mm || atomic_read_acquire(&e->state) != READY ||
	    atomic_read(&c->stopping) || !mmget_not_zero(c->mm))
		return;
	mmap_read_lock(c->mm);
	vma = find_vma(c->mm, addr);
	if (vma && vma->vm_start <= addr && vma->vm_ops == &sbk_vm_ops &&
	    vma->vm_private_data == c && (vma->vm_flags & VM_READ) &&
	    vma->vm_pgoff + ((addr - vma->vm_start) >> PAGE_SHIFT) == e->index)
		ret = get_user_pages_remote(c->mm, addr, 1, 0, &page, NULL, NULL);
	mmap_read_unlock(c->mm);
	mmput(c->mm);
	if (ret == 1) {
		put_page(page);
		atomic64_inc(&c->installed);
	} else {
		atomic64_inc(&c->skipped);
	}
}

static void sbk_prefetch_entry(struct sbk_entry *e)
{
	struct sbk_context *c = e->ctx;
	struct sbk_rdma_slot *slot = NULL;
	int err = 0;
	if (atomic_read_acquire(&e->state) != QUEUED_FT || atomic_read(&c->stopping))
		return;
	if (c->cfg.backend == SBK_BACKEND_RDMA)
		err = sbk_rdma_reserve(c->rdma, SBK_PREFETCH, &slot);
	if (atomic_cmpxchg(&e->state, QUEUED_FT, INFLIGHT) != QUEUED_FT) {
		if (slot)
			sbk_rdma_release(c->rdma, SBK_PREFETCH, slot);
		return;
	}
	if (err) {
		e->lane = SBK_PREFETCH;
		sbk_publish(e, err);
		return;
	}
	sbk_fetch_batch_reserved(&e, 1, SBK_PREFETCH, NULL, slot);
	if (slot)
		sbk_rdma_release(c->rdma, SBK_PREFETCH, slot);
	sbk_install_ahead(e);
}

static void sbk_prefetch_work(struct work_struct *work)
{
	sbk_prefetch_entry(container_of(work, struct sbk_entry, prefetch));
}
static bool sbk_prefetch_step(struct sbk_work *work)
{
	sbk_prefetch_entry(container_of(work, struct sbk_entry, ft_job));
	return false;
}
static void sbk_prefetch_neighbors(struct sbk_context *c, unsigned long index)
{
	static const int offsets[] = {1, -1, 2, -2};
	unsigned int n;
	if (!c->cfg.prefetch_enabled || atomic_read(&c->stopping))
		return;
	for (n = 0; n < ARRAY_SIZE(offsets); n++) {
		long j = offsets[n];
		struct sbk_entry *e;
		int s;
		if (!j || (j < 0 && index < -j) || index + j >= c->cfg.pages)
			continue;
		e = &c->entries[index + j];
		s = atomic_read(&e->state);
		while (s == REMOTE || s == QUEUED_BG) {
			int seen = atomic_cmpxchg(&e->state, s, QUEUED_FT);
			if (seen == s) {
				if (c->ft_pool)
					sbk_dispatch_queue(c->ft_pool, &e->ft_job);
				else
					queue_work(c->ft_wq, &e->prefetch);
				break;
			}
			s = seen;
		}
	}
}

#ifdef CONFIG_SWIFTBATON_PTE
/* The PTE bridge serializes each token, including forked, unresolved markers.
 * The first resolver adopts the speculative/prefetched page without a copy.
 * A later fork resolver needs its own exclusive page from the sealed source. */
static struct page *sbk_anon_get_page(void *cookie, struct vm_area_struct *vma,
				     unsigned long address, unsigned int lane, bool user)
{
	struct sbk_entry *e = cookie;
	struct sbk_context *c = e->ctx;
	struct page *page;
	struct sbk_prefetch_request request = {.entry = e};
	bool prefetch = user && early_prefetch && c->cfg.prefetch_enabled;
	struct sbk_rdma_notify notify = {.posted = prefetch ? sbk_prefetch_posted : NULL,
		.cookie = &request};
	int ret;
	if (atomic_read(&c->stopping))
		return ERR_PTR(-ECANCELED);
	if (user && atomic_read_acquire(&e->state) == READY)
		atomic64_inc(&c->hits);
	if (atomic_read_acquire(&e->state) != ADOPTED) {
		ret = sbk_ensure(e, lane, prefetch);
		if (ret)
			return ERR_PTR(ret);
		page = e->page;
		e->page = NULL;
		atomic_set_release(&e->state, ADOPTED);
		return page;
	}
	page = alloc_page_vma(GFP_HIGHUSER_MOVABLE, vma, address);
	if (!page)
		return ERR_PTR(-ENOMEM);
	if (c->cfg.backend == SBK_BACKEND_RDMA) {
		ret = sbk_rdma_read_notify(c->rdma, lane, &page, &e->index, 1,
				    c->has_region ? &c->region : NULL,
				    &notify);
	} else {
		if (prefetch)
			sbk_prefetch_posted(&request);
		if (c->cfg.test_delay_us)
			usleep_range(c->cfg.test_delay_us, c->cfg.test_delay_us + 20);
		ret = e->index == c->cfg.test_fail_page ? -EIO : 0;
		if (!ret)
			copy_highpage(page, c->source[e->index]);
	}
	if (ret) {
		atomic64_inc(&c->errors);
		put_page(page);
		return ERR_PTR(ret);
	}
	atomic64_inc(&c->fetched[lane]);
	return page;
}
static void sbk_anon_installed(void *cookie, struct mm_struct *mm,
			       unsigned long address, unsigned int lane, bool user)
{
	struct sbk_entry *e = cookie;
	if (user && !early_prefetch) {
		sbk_prefetch_neighbors(e->ctx, e->index);
	} else if (!user) {
		atomic64_inc(&e->ctx->installed);
	}
}
static void sbk_anon_fault_done(void *cookie, u64 ns, vm_fault_t result)
{
	struct sbk_context *c = ((struct sbk_entry *)cookie)->ctx;
	atomic64_inc(&c->faults);
	atomic64_add(ns, &c->fault_ns);
	sbk_max(&c->fault_max, ns);
	atomic64_inc(&c->histogram[min_t(unsigned int, fls64(ns), 31)]);
}
static void sbk_anon_release(void *cookie)
{
	struct sbk_entry *e = cookie;
	atomic64_inc(&e->ctx->retired_tokens);
	sbk_maybe_drain(e->ctx);
	kref_put(&e->ctx->refs, sbk_release_ref);
}
static const struct sbk_pte_provider sbk_anon_provider = {
	.owner = THIS_MODULE, .get_page = sbk_anon_get_page,
	.installed = sbk_anon_installed, .release = sbk_anon_release,
	.fault_done = sbk_anon_fault_done,
};
static int sbk_arm_anonymous(struct sbk_context *c, void __user *user)
{
	struct sbk_anon_arm a;
	unsigned long i;
	int ret;
	if (!c->configured || c->mapped || c->tokens_created ||
	    (c->pretransferred && !c->sealed) || atomic_read(&c->stopping))
		return -EINVAL;
	if (copy_from_user(&a, user, sizeof(a)))
		return -EFAULT;
	atomic64_set(&c->retired_tokens, 0);
	c->token_ids = kvcalloc(c->cfg.pages, sizeof(*c->token_ids), GFP_KERNEL);
	if (!c->token_ids)
		return -ENOMEM;
	for (i = 0; i < c->cfg.pages; i++) {
		kref_get(&c->refs);
		ret = sbk_pte_token_create(&sbk_anon_provider, &c->entries[i], &c->token_ids[i]);
		if (ret) {
			kref_put(&c->refs, sbk_release_ref);
			goto fail;
		}
		c->tokens_created++;
	}
	c->base = a.address;
	c->mm = current->mm;
	mmgrab(c->mm);
	c->anonymous = c->mapped = true;
	ret = sbk_pte_arm(c->mm, c->base, c->cfg.pages, c->token_ids);
	if (!ret) {
		c->sealed = true;
		atomic_set_release(&c->armed, 1);
		/* PTEs (including fork copies) now own the token references. Keeping
		 * creator references until fd close would hide outstanding markers. */
		for (i = 0; i < c->tokens_created; i++)
			sbk_pte_token_put(c->token_ids[i]);
		c->tokens_created = 0;
		sbk_maybe_drain(c);
		return 0;
	}
	c->anonymous = c->mapped = false;
	mmdrop(c->mm);
	c->mm = NULL;
fail:
	for (i = 0; i < c->tokens_created; i++)
		sbk_pte_token_put(c->token_ids[i]);
	c->tokens_created = 0;
	kvfree(c->token_ids);
	c->token_ids = NULL;
	return ret;
}
#endif

static vm_fault_t sbk_fault(struct vm_fault *vmf)
{
	struct sbk_context *c = vmf->vma->vm_private_data;
	unsigned long index = vmf->pgoff;
	struct sbk_entry *e;
	u64 start, ns;
	bool missed, user_fault;
	int ret;
	if (index >= c->cfg.pages || atomic_read(&c->stopping))
		return VM_FAULT_SIGBUS;
	e = &c->entries[index];
	start = ktime_get_ns();
	missed = atomic_read_acquire(&e->state) != READY;
	user_fault = !!(vmf->flags & FAULT_FLAG_USER);
	ret = sbk_ensure(e, SBK_DEMAND, user_fault && missed && early_prefetch && c->cfg.prefetch_enabled);
	if (!ret) {
		get_page(e->page);
		vmf->page = e->page;
		if (missed && (!user_fault || !early_prefetch))
			sbk_prefetch_neighbors(c, index);
	}
	ns = ktime_get_ns() - start;
	/* Exclude our GUP preinstallation callbacks from application-fault metrics. */
	if (user_fault) {
		atomic64_inc(&c->faults);
		if (!missed)
			atomic64_inc(&c->hits);
		atomic64_add(ns, &c->fault_ns);
		sbk_max(&c->fault_max, ns);
		atomic64_inc(&c->histogram[min_t(unsigned int, fls64(ns), 31)]);
	}
	if (ret)
		return ret == -ENOMEM ? VM_FAULT_OOM : VM_FAULT_SIGBUS;
	return 0;
}

static void sbk_vma_open(struct vm_area_struct *vma)
{
	struct sbk_context *c = vma->vm_private_data;
	kref_get(&c->refs);
}
static void sbk_vma_close(struct vm_area_struct *vma)
{
	struct sbk_context *c = vma->vm_private_data;
	kref_put(&c->refs, sbk_release_ref);
}
static const struct vm_operations_struct sbk_vm_ops = {
	.open = sbk_vma_open, .close = sbk_vma_close, .fault = sbk_fault,
};

static bool sbk_background_batches(struct sbk_context *c)
{
	unsigned long first, last, j;
	struct sbk_entry *batch[SBK_MAX_BATCH];
	unsigned int count, i, quantum;
	for (quantum = 0; quantum < SBK_DISPATCH_BATCHES && !atomic_read(&c->stopping); quantum++) {
		struct sbk_rdma_slot *slot = NULL;
		bool pending = false;
		int err = 0;
		first = atomic_long_fetch_add(c->cfg.batch_pages, &c->cursor);
		if (first >= c->cfg.pages)
			break;
		last = min_t(unsigned long, first + c->cfg.batch_pages, c->cfg.pages);
		/* Install completed PS pages before reserving a QP. Installation can
		 * take the resolver/mmap locks; no transport capacity may be held then. */
		for (j = first; j < last; j++) {
			struct sbk_entry *e = &c->entries[c->order[j]];
			if (c->anonymous && atomic_read_acquire(&e->state) == READY)
				sbk_install_ahead(e); /* PS already fetched; adopt without another read. */
			if (atomic_read_acquire(&e->state) == QUEUED_BG)
				pending = true;
		}
		if (!pending) {
			cond_resched();
			continue;
		}
		if (c->cfg.backend == SBK_BACKEND_RDMA)
			err = sbk_rdma_reserve(c->rdma, SBK_BACKGROUND, &slot);
		/* PF/FT may have taken every candidate during admission. Claim only
		 * after capacity exists; an empty batch still returns its reservation. */
		count = 0;
		for (j = first; j < last; j++) {
			struct sbk_entry *e = &c->entries[c->order[j]];
			if (atomic_cmpxchg(&e->state, QUEUED_BG, INFLIGHT) == QUEUED_BG)
				batch[count++] = e;
		}
		if (count) {
			atomic64_inc(&c->batches);
			if (err) {
				for (i = 0; i < count; i++) {
					batch[i]->lane = SBK_BACKGROUND;
					sbk_publish(batch[i], err);
				}
			} else {
				sbk_fetch_batch_reserved(batch, count, SBK_BACKGROUND, NULL, slot);
			}
		}
		if (slot)
			sbk_rdma_release(c->rdma, SBK_BACKGROUND, slot);
		for (i = 0; i < count; i++)
			sbk_install_ahead(batch[i]);
		cond_resched();
	}
	return !atomic_read(&c->stopping) && atomic_long_read(&c->cursor) < c->cfg.pages;
}

static void sbk_background_work(struct work_struct *work)
{
	struct sbk_bg_worker *w = container_of(work, struct sbk_bg_worker, work);
	while (sbk_background_batches(w->ctx))
		cond_resched();
}

static void sbk_coverage_work(struct work_struct *work)
{
#ifdef CONFIG_SWIFTBATON_PTE
	struct sbk_context *c = container_of(work, struct sbk_context, coverage);
	unsigned long i;
	/* Initial workers use the known restore addresses. Complete aliases that
 * forked or moved meanwhile through the MM bridge's anon_vma index. Repeat
 * until the last marker is gone; completed-page counts cannot prove this. */
	while (!atomic_read(&c->stopping) &&
	       atomic64_read(&c->retired_tokens) < c->cfg.pages) {
		for (i = 0; i < c->cfg.pages && !atomic_read(&c->stopping); i++) {
			unsigned long index = c->order[i];
			if (atomic_read(&c->entries[index].state) == FAILED)
				continue;
			sbk_pte_populate_all(c->token_ids[index], SBK_BACKGROUND);
			cond_resched();
		}
		if (atomic64_read(&c->errors))
			break; /* Failed reads cannot authorize source retirement. */
		if (!atomic_read(&c->stopping) &&
		    atomic64_read(&c->retired_tokens) < c->cfg.pages)
			msleep(1);
	}
#endif
}

static bool sbk_coverage_step(struct sbk_work *work)
{
#ifdef CONFIG_SWIFTBATON_PTE
	struct sbk_context *c = container_of(work, struct sbk_context, coverage_job);
	unsigned int n, limit = SBK_DISPATCH_BATCHES * c->cfg.batch_pages;
	for (n = 0; n < limit && !atomic_read(&c->stopping) &&
	     atomic64_read(&c->retired_tokens) < c->cfg.pages; n++) {
		unsigned long index = c->order[c->coverage_cursor++];
		if (atomic_read(&c->entries[index].state) != FAILED)
			sbk_pte_populate_all(c->token_ids[index], SBK_BACKGROUND);
		if (atomic64_read(&c->errors))
			return false;
		if (c->coverage_cursor == c->cfg.pages) {
			c->coverage_cursor = 0;
			/* Alias removal may still be in progress. Bound retries as before. */
			if (!atomic_read(&c->stopping) &&
			    atomic64_read(&c->retired_tokens) < c->cfg.pages)
				msleep(1);
			break;
		}
		cond_resched();
	}
	work->hot = c->coverage_cursor < c->hot_count;
	return !atomic_read(&c->stopping) &&
	       atomic64_read(&c->retired_tokens) < c->cfg.pages;
#else
	return false;
#endif
}
static bool sbk_background_step(struct sbk_work *work)
{
	struct sbk_bg_worker *w = container_of(work, struct sbk_bg_worker, job);
	struct sbk_context *c = w->ctx;
	if (sbk_background_batches(c)) {
		work->hot = atomic_long_read(&c->cursor) < c->hot_count;
		return true;
	}
	/* Coverage starts after all initial workers in THIS region finish. Its
	 * queue admission occurs while this last job still holds its owner count. */
	if (atomic_dec_and_test(&c->bg_remaining) && c->anonymous &&
	    !atomic_read(&c->stopping) && !atomic64_read(&c->errors) &&
	    atomic64_read(&c->retired_tokens) < c->cfg.pages)
		sbk_dispatch_queue(c->bg_pool, &c->coverage_job);
	return false;
}

static int sbk_configure(struct sbk_context *c, void __user *arg)
{
	struct sbk_config cfg;
	unsigned long i, end, bytes;
	long pinned;
	if (c->config_attempted || atomic_read(&c->stopping))
		return -EBUSY; /* Failed configuration requires reopening the device. */
	if (copy_from_user(&cfg, arg, sizeof(cfg)))
		return -EFAULT;
	if (cfg.version != SBK_ABI_VERSION ||
	    (cfg.backend != SBK_BACKEND_LOOPBACK_TEST && cfg.backend != SBK_BACKEND_RDMA) ||
	    !cfg.pages || cfg.pages > SBK_MAX_PAGES || cfg.reserved ||
	    !cfg.prefetch_workers || cfg.prefetch_workers > SBK_MAX_WORKERS ||
	    !cfg.background_workers || cfg.background_workers > SBK_MAX_WORKERS ||
	    !cfg.batch_pages || cfg.batch_pages > SBK_MAX_BATCH ||
	    cfg.prefetch_enabled > 1 || cfg.test_delay_us > 1000000 ||
	    (cfg.source_address & ~PAGE_MASK) ||
	    (cfg.test_fail_page != SBK_NO_FAILURE && cfg.test_fail_page >= cfg.pages))
		return -EINVAL;
	if (cfg.backend == SBK_BACKEND_RDMA) {
		if (!sbk_rdma_ready(c->rdma, c->has_region ? 0 : cfg.pages) ||
		    (c->has_region && c->region.pages != cfg.pages) || cfg.test_delay_us ||
		    cfg.test_fail_page != SBK_NO_FAILURE || cfg.source_address)
			return -EINVAL;
	} else if (c->rdma ||
	    check_mul_overflow((unsigned long)cfg.pages, PAGE_SIZE, &bytes) ||
	    check_add_overflow((unsigned long)cfg.source_address, bytes, &end) ||
	    !access_ok(u64_to_user_ptr(cfg.source_address), bytes)) {
		return -EFAULT;
	}
	c->config_attempted = true;
	c->cfg = cfg;
	c->ft_pool = sbk_rdma_dispatch(c->rdma, SBK_PREFETCH);
	c->bg_pool = sbk_rdma_dispatch(c->rdma, SBK_BACKGROUND);
	c->entries = kvcalloc(cfg.pages, sizeof(*c->entries), GFP_KERNEL);
	if (cfg.backend == SBK_BACKEND_LOOPBACK_TEST)
		c->source = kvcalloc(cfg.pages, sizeof(*c->source), GFP_KERNEL);
	if (!c->entries || (cfg.backend == SBK_BACKEND_LOOPBACK_TEST && !c->source))
		return -ENOMEM;
	for (i = 0; i < cfg.pages; i++) {
		struct sbk_entry *e = &c->entries[i];
		e->ctx = c;
		e->index = i;
		e->lane = SBK_LANES + 1;
		atomic_set(&e->state, REMOTE);
		init_waitqueue_head(&e->wait);
		if (c->ft_pool)
			sbk_work_init(&e->ft_job, &c->owner, sbk_prefetch_step, false);
		else
			INIT_WORK(&e->prefetch, sbk_prefetch_work);
	}
	if (cfg.backend == SBK_BACKEND_LOOPBACK_TEST) {
		pinned = pin_user_pages_fast(cfg.source_address, cfg.pages, FOLL_LONGTERM,
					     c->source);
		if (pinned > 0)
			c->pinned = pinned;
		if (pinned != cfg.pages)
			return pinned < 0 ? pinned : -EFAULT;
	}
	if (!c->bg_pool) {
		c->ft_wq = alloc_workqueue("sbk_prefetch", WQ_UNBOUND | WQ_HIGHPRI,
					  cfg.prefetch_workers);
		c->bg_wq = alloc_workqueue("sbk_background", WQ_UNBOUND, cfg.background_workers);
		if (!c->ft_wq || !c->bg_wq)
			return -ENOMEM;
	}
	for (i = 0; i < cfg.background_workers; i++) {
		c->bg[i].ctx = c;
		if (c->bg_pool)
			sbk_work_init(&c->bg[i].job, &c->owner, sbk_background_step, false);
		else
			INIT_WORK(&c->bg[i].work, sbk_background_work);
	}
	if (c->bg_pool)
		sbk_work_init(&c->coverage_job, &c->owner, sbk_coverage_step, false);
	else
		INIT_WORK(&c->coverage, sbk_coverage_work);
	c->configured = true;
	return 0;
}

static int sbk_background(struct sbk_context *c, void __user *arg)
{
	struct sbk_hot_list list;
	u64 *hot = NULL;
	unsigned long *seen = NULL, i, n = 0;
	int ret = 0;
	if (!c->configured || !c->mapped || atomic_read(&c->stopping))
		return -EINVAL;
	if (c->background_started)
		return -EALREADY;
	if (copy_from_user(&list, arg, sizeof(list)))
		return -EFAULT;
	if (list.count > c->cfg.pages)
		return -EINVAL;
	if (list.count) {
		hot = kvmalloc_array(list.count, sizeof(*hot), GFP_KERNEL);
		if (!hot)
			return -ENOMEM;
		if (copy_from_user(hot, u64_to_user_ptr(list.indices), list.count * sizeof(*hot))) {
			ret = -EFAULT;
			goto out;
		}
	}
	seen = bitmap_zalloc(c->cfg.pages, GFP_KERNEL);
	c->order = kvmalloc_array(c->cfg.pages, sizeof(*c->order), GFP_KERNEL);
	if (!seen || !c->order) {
		ret = -ENOMEM;
		goto out;
	}
	for (i = 0; i < list.count; i++) {
		if (hot[i] >= c->cfg.pages || test_and_set_bit(hot[i], seen)) {
			ret = -EINVAL;
			goto out;
		}
		c->order[n++] = hot[i];
	}
	for (i = 0; i < c->cfg.pages; i++)
		if (!test_bit(i, seen))
			c->order[n++] = i;
	for (i = 0; i < c->cfg.pages; i++)
		atomic_cmpxchg(&c->entries[i].state, REMOTE, QUEUED_BG);
	c->background_started = true;
	c->hot_count = list.count;
	if (c->bg_pool) {
		unsigned int workers = min_t(unsigned long, c->cfg.background_workers,
					 DIV_ROUND_UP(c->cfg.pages, c->cfg.batch_pages));
		atomic_set(&c->bg_remaining, workers);
		c->coverage_job.hot = !!c->hot_count;
		for (i = 0; i < workers; i++) {
			c->bg[i].job.hot = !!c->hot_count;
			/* Retirement may close admission concurrently; in that case no
			 * further coverage is needed. Destruction still joins admitted jobs. */
			if (!sbk_dispatch_queue(c->bg_pool, &c->bg[i].job))
				atomic_dec(&c->bg_remaining);
		}
	} else {
		for (i = 0; i < c->cfg.background_workers; i++)
			queue_work(c->bg_wq, &c->bg[i].work);
		if (c->anonymous)
			queue_work(c->bg_wq, &c->coverage);
	}
out:
	if (ret) {
		kvfree(c->order);
		c->order = NULL;
	}
	kvfree(hot);
	bitmap_free(seen);
	return ret;
}

struct sbk_ps_job {
	struct sbk_context *ctx;
	u64 *indices;
	unsigned long count;
	atomic_long_t cursor;
	atomic_t error;
};
struct sbk_ps_worker {
	union { struct work_struct work; struct sbk_work dispatch; };
	struct sbk_ps_job *job;
};

static bool sbk_ps_batches(struct sbk_ps_job *job)
{
	struct sbk_context *c = job->ctx;
	struct sbk_entry *batch[SBK_MAX_BATCH];
	unsigned long first, last, j;
	unsigned int count, i, quantum;
	for (quantum = 0; quantum < SBK_DISPATCH_BATCHES &&
	     !atomic_read(&job->error) && !atomic_read(&c->stopping); quantum++) {
		first = atomic_long_fetch_add(c->cfg.batch_pages, &job->cursor);
		if (first >= job->count)
			break;
		last = min_t(unsigned long, first + c->cfg.batch_pages, job->count);
		count = 0;
		for (j = first; j < last; j++) {
			struct sbk_entry *e = &c->entries[job->indices[j]];
			if (atomic_cmpxchg(&e->state, REMOTE, INFLIGHT) == REMOTE)
				batch[count++] = e;
		}
		if (count) {
			sbk_fetch_batch(batch, count, SBK_PRETRANSFER, NULL);
			for (i = 0; i < count; i++)
				if (atomic_read_acquire(&batch[i]->state) != READY)
					atomic_set(&job->error, -EIO);
		}
		cond_resched();
	}
	return !atomic_read(&job->error) && !atomic_read(&c->stopping) &&
	       atomic_long_read(&job->cursor) < job->count;
}
static void sbk_ps_work(struct work_struct *work)
{
	struct sbk_ps_job *job = container_of(work, struct sbk_ps_worker, work)->job;
	while (sbk_ps_batches(job))
		cond_resched();
}
static bool sbk_ps_step(struct sbk_work *work)
{
	return sbk_ps_batches(container_of(work, struct sbk_ps_worker, dispatch)->job);
}

/* Caller holds control, excluding ARM/SEAL/BACKGROUND and any other PS call.
 * Workers never take control. All workers, including reads racing an error,
 * finish before this stack job or the copied indices can be destroyed. */
static int sbk_stage_parallel(struct sbk_context *c, u64 *indices, unsigned long count)
{
	struct sbk_ps_job job = {.ctx = c, .indices = indices, .count = count};
	struct sbk_ps_worker *workers;
	unsigned int nr, i;
	if (!count)
		return 0;
	nr = min_t(unsigned long, c->cfg.background_workers,
		   DIV_ROUND_UP(count, c->cfg.batch_pages));
	workers = kcalloc(nr, sizeof(*workers), GFP_KERNEL);
	if (!workers)
		return -ENOMEM;
	atomic_long_set(&job.cursor, 0);
	atomic_set(&job.error, 0);
	c->pretransferred = true;
	for (i = 0; i < nr; i++) {
		workers[i].job = &job;
		if (c->bg_pool) {
			sbk_work_init(&workers[i].dispatch, &c->owner, sbk_ps_step, false);
			if (!sbk_dispatch_queue(c->bg_pool, &workers[i].dispatch))
				atomic_set(&job.error, -ECANCELED);
		} else {
			INIT_WORK(&workers[i].work, sbk_ps_work);
			queue_work(c->bg_wq, &workers[i].work);
		}
	}
	for (i = 0; i < nr; i++) {
		if (c->bg_pool)
			sbk_work_wait(&workers[i].dispatch);
		else
			flush_work(&workers[i].work);
	}
	kfree(workers);
	return atomic_read(&job.error);
}

/* No VMA can see speculative PS pages until the final invalidation is sealed. */
static int sbk_stage_list(struct sbk_context *c, struct sbk_hot_list list, bool seal,
			  bool parallel)
{
	u64 *indices = NULL;
	unsigned long *seen = NULL;
	struct sbk_entry *batch[SBK_MAX_BATCH];
	unsigned long i, count = 0;
	int ret = 0;
	if (!c->configured || c->mapped || c->sealed || atomic_read(&c->stopping))
		return -EINVAL;
	if (list.count > c->cfg.pages || (!seal && !parallel && list.count > SBK_MAX_BATCH))
		return -EINVAL;
	if (list.count) {
		indices = kvmalloc_array(list.count, sizeof(*indices), GFP_KERNEL);
		seen = bitmap_zalloc(c->cfg.pages, GFP_KERNEL);
		if (!indices || !seen) {
			ret = -ENOMEM;
			goto out;
		}
		if (copy_from_user(indices, u64_to_user_ptr(list.indices), list.count * sizeof(*indices))) {
			ret = -EFAULT;
			goto out;
		}
		/* Validate all input before any mutation. */
		for (i = 0; i < list.count; i++)
			if (indices[i] >= c->cfg.pages || test_and_set_bit(indices[i], seen)) {
				ret = -EINVAL;
				goto out;
			}
	}
	if (parallel) {
		/* A previous failed transfer cannot silently become a successful PS
		 * call. READY pages are allowed and skipped without duplicate reads. */
		for (i = 0; i < list.count; i++) {
			int state = atomic_read_acquire(&c->entries[indices[i]].state);
			if (state != REMOTE && state != READY) {
				ret = -EIO;
				goto out;
			}
		}
		ret = sbk_stage_parallel(c, indices, list.count);
		goto out;
	}
	for (i = 0; i < list.count; i++) {
		struct sbk_entry *e = &c->entries[indices[i]];
		int state = atomic_read(&e->state);
		if (seal) {
			if (state == READY) {
				atomic64_dec(&c->completed);
				atomic64_inc(&c->invalidated);
			}
			if (e->page) {
				put_page(e->page);
				e->page = NULL;
			}
			e->started = e->completed = 0;
			e->error = 0;
			e->lane = SBK_LANES + 1;
			atomic_set(&e->state, REMOTE);
		} else if (state == REMOTE) {
			atomic_set(&e->state, INFLIGHT);
			batch[count++] = e;
		}
	}
	if (seal) {
		c->sealed = true;
	} else if (count) {
		c->pretransferred = true;
		sbk_fetch_batch(batch, count, SBK_PRETRANSFER, NULL);
		for (i = 0; i < count; i++)
			if (atomic_read_acquire(&batch[i]->state) != READY)
				ret = -EIO;
	}
out:
	kvfree(indices);
	bitmap_free(seen);
	return ret;
}

static int sbk_stage(struct sbk_context *c, void __user *arg, bool seal, bool parallel)
{
	struct sbk_hot_list list;
	if (copy_from_user(&list, arg, sizeof(list)))
		return -EFAULT;
	return sbk_stage_list(c, list, seal, parallel);
}

/* PS registrations pin physical pages. A later COW/remap needs a fresh MR;
 * changing the descriptor and invalidating the final dirty set must precede
 * exposing any destination PTE. All fallible input validation happens first. */
static int sbk_seal_region(struct sbk_context *c, void __user *user)
{
	struct sbk_region_seal seal;
	int ret;
	if (!c->has_region || !c->configured || c->mapped || c->sealed ||
	    atomic_read(&c->stopping))
		return -EINVAL;
	if (copy_from_user(&seal, user, sizeof(seal)))
		return -EFAULT;
	if (seal.remote.pages != c->cfg.pages)
		return -EINVAL;
	ret = sbk_rdma_get_region(c->rdma, &seal.remote);
	if (ret)
		return ret;
	ret = sbk_stage_list(c, seal.dirty, true, false);
	if (!ret)
		c->region = seal.remote;
	sbk_rdma_put(c->rdma);
	return ret;
}

/* Never hold two context mutexes: opposite-direction imports must not ABBA.
 * fget pins the parent while we acquire a transport reference; only then lock
 * the child and recheck its state. The descriptor is immutable after binding. */
static int sbk_bind_region(struct file *file, void __user *user)
{
	struct sbk_context *c = file->private_data, *parent;
	struct sbk_region_bind bind;
	struct sbk_rdma *r;
	struct file *session;
	int ret;
	if (copy_from_user(&bind, user, sizeof(bind)))
		return -EFAULT;
	if (bind.reserved)
		return -EINVAL;
	session = fget(bind.session_fd);
	if (!session)
		return -EBADF;
	if (session == file || session->f_op != &sbk_fops) {
		ret = -EINVAL;
		goto out;
	}
	parent = session->private_data;
	mutex_lock(&parent->control);
	r = parent->rdma;
	ret = (parent->has_region || parent->config_attempted ||
	       atomic_read(&parent->stopping)) ? -EINVAL :
		sbk_rdma_get_region(r, &bind.remote);
	mutex_unlock(&parent->control);
	if (ret)
		goto out;
	mutex_lock(&c->control);
	if (c->rdma || c->config_attempted || atomic_read(&c->stopping)) {
		ret = -EBUSY;
	} else {
		c->rdma = r;
		c->region = bind.remote;
		c->has_region = true;
	}
	mutex_unlock(&c->control);
	if (ret)
		sbk_rdma_put(r);
out:
	fput(session);
	return ret;
}

/* Both control locks use one stable address order. No other path takes two
 * context locks. No RDMA, worker flush, or page allocation occurs while held. */
static int sbk_import_ps(struct file *file, void __user *user)
{
	struct sbk_context *dst = file->private_data, *src, *first, *second;
	struct sbk_ps_slice slice;
	struct file *source;
	unsigned long i;
	int ret = -EINVAL;
	if (copy_from_user(&slice, user, sizeof(slice)))
		return -EFAULT;
	if (slice.reserved || !slice.pages)
		return -EINVAL;
	source = fget(slice.source_fd);
	if (!source)
		return -EBADF;
	if (source == file || source->f_op != &sbk_fops)
		goto put;
	src = source->private_data;
	first = (unsigned long)src < (unsigned long)dst ? src : dst;
	second = first == src ? dst : src;
	mutex_lock(&first->control);
	mutex_lock_nested(&second->control, SINGLE_DEPTH_NESTING);
	if (!src->configured || !dst->configured || !src->has_region || !dst->has_region ||
	    src->rdma != dst->rdma || src->mapped || dst->mapped || src->sealed || dst->sealed ||
	    src->background_started || dst->background_started ||
	    atomic_read(&src->stopping) || atomic_read(&dst->stopping) ||
	    slice.source_offset >= src->cfg.pages || slice.destination_offset >= dst->cfg.pages ||
	    slice.pages > src->cfg.pages - slice.source_offset ||
	    slice.pages > dst->cfg.pages - slice.destination_offset)
		goto unlock;
	/* Validate the entire move before mutation. The control locks also join
	 * preceding PS ioctls and prevent either context being mapped or sealed. */
	for (i = 0; i < slice.pages; i++) {
		struct sbk_entry *s = &src->entries[slice.source_offset + i];
		struct sbk_entry *d = &dst->entries[slice.destination_offset + i];
		int state = atomic_read_acquire(&s->state);
		if ((state != REMOTE && state != READY) ||
		    (state == READY && (!s->page || s->lane != SBK_PRETRANSFER)) ||
		    atomic_read(&d->state) != REMOTE || d->page) {
			ret = -EEXIST;
			goto unlock;
		}
	}
	for (i = 0; i < slice.pages; i++) {
		struct sbk_entry *s = &src->entries[slice.source_offset + i];
		struct sbk_entry *d = &dst->entries[slice.destination_offset + i];
		if (atomic_read(&s->state) != READY)
			continue;
		d->page = s->page;
		d->started = s->started;
		d->completed = s->completed;
		d->lane = SBK_PRETRANSFER;
		d->error = 0;
		s->page = NULL;
		s->started = s->completed = 0;
		s->lane = SBK_LANES + 1;
		s->error = 0;
		atomic64_dec(&src->completed);
		atomic64_dec(&src->ps_pages);
		atomic_set_release(&s->state, REMOTE);
		atomic64_inc(&dst->completed);
		atomic64_inc(&dst->ps_pages);
		atomic_set_release(&d->state, READY);
	}
	dst->pretransferred = true;
	ret = 0;
unlock:
	mutex_unlock(&second->control);
	mutex_unlock(&first->control);
put:
	fput(source);
	return ret;
}

/* Retain the transport under control, then let independent owners fault/pin
 * in parallel. The file pins c throughout this ioctl; the extra reference also
 * explicitly protects the transport through registration and copyout. */
static int sbk_export_region(struct sbk_context *c, void __user *user)
{
	struct sbk_rdma_region region;
	struct sbk_rdma *r;
	int ret;
	if (copy_from_user(&region, user, sizeof(region)))
		return -EFAULT;
	mutex_lock(&c->control);
	r = c->rdma;
	if (!r || c->has_region || c->config_attempted || atomic_read(&c->stopping)) {
		mutex_unlock(&c->control);
		return -EINVAL;
	}
	sbk_rdma_get(r);
	mutex_unlock(&c->control);
	ret = sbk_rdma_export_region(r, &region);
	if (!ret && copy_to_user(user, &region, sizeof(region)))
		ret = -EFAULT;
	sbk_rdma_put(r);
	return ret;
}

/* A helper owns no userspace file table. Only the frozen caller's mm and
 * memlock limit are borrowed; registration still uses the session creator's
 * credentials in sbk_rdma_export_region(). In particular, kthreadd's unlimited
 * RLIMIT_MEMLOCK must never bypass an unprivileged controller's pin limit. */
struct sbk_export_job {
	struct sbk_rdma *rdma;
	struct mm_struct *mm;
	struct sbk_export_batch batch;
	unsigned long memlock_limit;
	atomic_t next, error, completed, active, peak;
};
struct sbk_export_worker {
	struct sbk_export_job *job;
	struct task_struct *task;
	struct completion done;
};
static void sbk_export_run(struct sbk_export_job *job)
{
	int index, ret, active, peak;
	while (!atomic_read(&job->error)) {
		index = atomic_inc_return(&job->next) - 1;
		if (index >= job->batch.count)
			break;
		active = atomic_inc_return(&job->active);
		peak = atomic_read(&job->peak);
		while (active > peak && !atomic_try_cmpxchg(&job->peak, &peak, active))
			;
		ret = sbk_rdma_export_region(job->rdma, &job->batch.regions[index]);
		atomic_dec(&job->active);
		if (ret)
			atomic_cmpxchg(&job->error, 0, ret);
		else
			atomic_inc(&job->completed);
	}
}
static int sbk_export_thread(void *arg)
{
	struct sbk_export_worker *worker = arg;
	struct sbk_export_job *job = worker->job;
	/* kthread_create does not use CLONE_SIGHAND: this signal/rlimit is private. */
	WRITE_ONCE(current->signal->rlim[RLIMIT_MEMLOCK].rlim_cur, job->memlock_limit);
	kthread_use_mm(job->mm);
	sbk_export_run(job);
	kthread_unuse_mm(job->mm);
	complete(&worker->done);
	return 0;
}
static int sbk_export_batch(struct sbk_context *c, void __user *user)
{
	struct sbk_export_job *job;
	struct sbk_export_worker *workers = NULL;
	unsigned int i, launched = 0, nr;
	int ret = -EINVAL;
	job = kzalloc(sizeof(*job), GFP_KERNEL);
	if (!job)
		return -ENOMEM;
	if (copy_from_user(&job->batch, user, sizeof(job->batch))) {
		ret = -EFAULT;
		goto free_job;
	}
	if (!current->mm || !job->batch.count || job->batch.count > SBK_MAX_BATCH ||
	    !job->batch.workers || job->batch.workers > SBK_MAX_WORKERS ||
	    job->batch.completed || job->batch.peak)
		goto free_job;
	/* Reject the entire malformed request before pinning anything. Overlap is
	 * permitted, as with EXPORT_REGION; separate immutable MRs can alias. */
	for (i = 0; i < job->batch.count; i++) {
		struct sbk_rdma_region *r = &job->batch.regions[i];
		if (!r->pages || r->pages > SBK_MAX_PAGES || (r->address & ~PAGE_MASK) ||
		    r->address > U64_MAX - (r->pages << PAGE_SHIFT) || r->id || r->rkey)
			goto free_job;
	}
	nr = min(job->batch.workers, job->batch.count);
	workers = kcalloc(nr, sizeof(*workers), GFP_KERNEL);
	if (!workers) { ret = -ENOMEM; goto free_job; }
	mutex_lock(&c->control);
	job->rdma = c->rdma;
	if (!job->rdma || c->has_region || c->config_attempted || atomic_read(&c->stopping)) {
		mutex_unlock(&c->control);
		goto free_workers;
	}
	sbk_rdma_get(job->rdma);
	mutex_unlock(&c->control);
	/* mm_users, not just mm_count: prevents address-space teardown while any
	 * helper runs, even if another thread kills the owner during this ioctl. */
	job->mm = current->mm;
	mmget(job->mm);
	job->memlock_limit = rlimit(RLIMIT_MEMLOCK);
	if (nr == 1) {
		sbk_export_run(job);
	} else {
		for (i = 0; i < nr && !atomic_read(&job->error); i++) {
			workers[i].job = job;
			init_completion(&workers[i].done);
			workers[i].task = kthread_create(sbk_export_thread, &workers[i], "sbk_export/%u", i);
			if (IS_ERR(workers[i].task)) {
				atomic_cmpxchg(&job->error, 0, PTR_ERR(workers[i].task));
				break;
			}
			/* Keep task storage valid even if this short helper exits before
			 * the join. Match the frozen owner's CPU/NUMA locality. */
			get_task_struct(workers[i].task);
			ret = set_cpus_allowed_ptr(workers[i].task, current->cpus_ptr);
			if (ret) {
				kthread_stop(workers[i].task);
				put_task_struct(workers[i].task);
				atomic_cmpxchg(&job->error, 0, ret);
				break;
			}
			wake_up_process(workers[i].task);
			launched++;
		}
		for (i = 0; i < launched; i++) {
			wait_for_completion(&workers[i].done);
			kthread_stop(workers[i].task);
			put_task_struct(workers[i].task);
		}
	}
	mmput(job->mm);
	job->batch.completed = atomic_read(&job->completed);
	job->batch.peak = atomic_read(&job->peak);
	ret = atomic_read(&job->error);
	if (!ret && job->batch.completed != job->batch.count)
		ret = -EIO;
	/* A failing copyout cannot release MRs behind potential remote readers.
	 * As in EXPORT_REGION, close/revoke reclaims all partial registrations. */
	if (copy_to_user(user, &job->batch, sizeof(job->batch)))
		ret = -EFAULT;
	sbk_rdma_put(job->rdma);
free_workers:
	kfree(workers);
free_job:
	kfree(job);
	return ret;
}

static long sbk_ioctl(struct file *f, unsigned int cmd, unsigned long arg)
{
	struct sbk_context *c = f->private_data;
	void __user *user = (void __user *)arg;
	struct sbk_stats stats = {};
	struct sbk_capabilities caps = {.version = SBK_ABI_VERSION,
		.max_regions = SBK_MAX_REGIONS, .max_pages = SBK_MAX_PAGES};
	struct sbk_drain_status drain = {};
	struct sbk_dispatch_stats dispatch = {};
	struct sbk_page_info p;
	struct sbk_rdma_setup setup;
	struct sbk_rdma_endpoint peer;
	struct sbk_entry *e;
	int ret = 0, i;
	if (cmd == SBK_IOC_EXPORT_BATCH)
		return sbk_export_batch(c, user);
	if (cmd == SBK_IOC_EXPORT_REGION)
		return sbk_export_region(c, user);
	if (cmd == SBK_IOC_BIND_REGION)
		return sbk_bind_region(f, user);
	if (cmd == SBK_IOC_IMPORT_PS)
		return sbk_import_ps(f, user);
	mutex_lock(&c->control);
	switch (cmd) {
	case SBK_IOC_CAPABILITIES:
		caps.features |= SBK_FEATURE_PARALLEL_PS | SBK_FEATURE_PS_SLICE | SBK_FEATURE_PARALLEL_EXPORT;
		if (sbk_session_dispatch_enabled())
			caps.features |= SBK_FEATURE_SESSION_DISPATCH;
#ifdef CONFIG_SWIFTBATON_PTE
		caps.features |= SBK_FEATURE_ANONYMOUS_PTE;
#endif
		ret = copy_to_user(user, &caps, sizeof(caps)) ? -EFAULT : 0;
		break;
	case SBK_IOC_WATCH_DRAIN:
#ifdef CONFIG_SWIFTBATON_PTE
		ret = sbk_watch_drain(c, user);
#else
		ret = -EOPNOTSUPP;
#endif
		break;
	case SBK_IOC_DRAIN_STATUS:
		if (!c->configured || (c->mapped && !c->anonymous)) {
			ret = -EINVAL;
			break;
		}
		drain.pages = c->cfg.pages;
		drain.retired_tokens = atomic64_read(&c->retired_tokens);
		drain.armed = atomic_read_acquire(&c->armed);
		drain.drained = atomic_read_acquire(&c->drained);
		ret = copy_to_user(user, &drain, sizeof(drain)) ? -EFAULT : 0;
		break;
	case SBK_IOC_REVOKE_SOURCE:
		ret = sbk_rdma_revoke_source(c->rdma);
		break;
	case SBK_IOC_ARM_ANON:
#ifdef CONFIG_SWIFTBATON_PTE
		ret = sbk_arm_anonymous(c, user);
#else
		ret = -EOPNOTSUPP;
#endif
		break;
	case SBK_IOC_PRETRANSFER:
		ret = sbk_stage(c, user, false, false);
		break;
	case SBK_IOC_PRETRANSFER_MANY:
		ret = sbk_stage(c, user, false, true);
		break;
	case SBK_IOC_SEAL_REGION:
		ret = sbk_seal_region(c, user);
		break;
	case SBK_IOC_SEAL:
		ret = sbk_stage(c, user, true, false);
		break;
	case SBK_IOC_RDMA_CREATE:
		if (c->rdma || c->config_attempted || atomic_read(&c->stopping)) {
			ret = -EBUSY;
			break;
		}
		if (copy_from_user(&setup, user, sizeof(setup))) {
			ret = -EFAULT;
			break;
		}
		c->rdma = sbk_rdma_create(&setup);
		if (IS_ERR(c->rdma)) {
			ret = PTR_ERR(c->rdma);
			c->rdma = NULL;
			break;
		}
		ret = copy_to_user(user, &setup, sizeof(setup)) ? -EFAULT : 0;
		break;
	case SBK_IOC_RDMA_CONNECT:
		if (copy_from_user(&peer, user, sizeof(peer))) {
			ret = -EFAULT;
			break;
		}
		ret = sbk_rdma_connect(c->rdma, &peer);
		break;
	case SBK_IOC_CONFIG:
		ret = sbk_configure(c, user);
		break;
	case SBK_IOC_BACKGROUND:
		ret = sbk_background(c, user);
		break;
	case SBK_IOC_DISPATCH_STATS:
		ret = sbk_rdma_dispatch_stats(c->rdma, &dispatch);
		if (!ret && copy_to_user(user, &dispatch, sizeof(dispatch)))
			ret = -EFAULT;
		break;
	case SBK_IOC_STATS:
		stats.faults = atomic64_read(&c->faults);
		stats.hits = atomic64_read(&c->hits);
		stats.waits = atomic64_read(&c->waits);
		stats.errors = atomic64_read(&c->errors);
		for (i = 0; i < SBK_LANES; i++)
			stats.fetched[i] = atomic64_read(&c->fetched[i]);
		stats.installed_ahead = atomic64_read(&c->installed);
		stats.skipped_install = atomic64_read(&c->skipped);
		stats.fault_ns = atomic64_read(&c->fault_ns);
		stats.fault_max_ns = atomic64_read(&c->fault_max);
		for (i = 0; i < 32; i++)
			stats.hist_ns_pow2[i] = atomic64_read(&c->histogram[i]);
		stats.batches = atomic64_read(&c->batches);
		stats.pages = c->cfg.pages;
		stats.completed = atomic64_read(&c->completed);
		stats.pretransferred = atomic64_read(&c->ps_pages);
		stats.invalidated = atomic64_read(&c->invalidated);
		ret = copy_to_user(user, &stats, sizeof(stats)) ? -EFAULT : 0;
		break;
	case SBK_IOC_PAGE:
		if (copy_from_user(&p, user, sizeof(p))) {
			ret = -EFAULT;
			break;
		}
		if (!c->configured || p.index >= c->cfg.pages) {
			ret = -EINVAL;
			break;
		}
		e = &c->entries[p.index];
		p.state = atomic_read_acquire(&e->state);
		/* Only terminal states publish the associated timing fields. */
		p.started_ns = p.completed_ns = 0;
		p.lane = SBK_LANES + 1;
		if (p.state == READY || p.state == FAILED || p.state == ADOPTED) {
			p.started_ns = e->started;
			p.completed_ns = e->completed;
			p.lane = e->lane;
		}
		ret = copy_to_user(user, &p, sizeof(p)) ? -EFAULT : 0;
		break;
	case SBK_IOC_CANCEL:
		atomic_set(&c->stopping, 1);
		/* A region only cancels itself; catalog controller cancels all views. */
		if (!c->has_region)
			sbk_rdma_cancel(c->rdma);
		if (c->configured)
			for (i = 0; i < c->cfg.pages; i++)
				wake_up_all(&c->entries[i].wait);
		break;
	default:
		ret = -ENOTTY;
	}
	mutex_unlock(&c->control);
	return ret;
}

static int sbk_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct sbk_context *c = file->private_data;
	int ret = 0;
	/* mmap holds mmap_write_lock; configure can fault/pin source pages. */
	if (!mutex_trylock(&c->control))
		return -EAGAIN;
	if (!c->configured || c->mapped || c->drain_event || atomic_read(&c->stopping) ||
	    (c->pretransferred && !c->sealed) ||
	    vma->vm_pgoff || (vma->vm_end - vma->vm_start) != c->cfg.pages * PAGE_SIZE ||
	    (vma->vm_flags & (VM_SHARED | VM_EXEC))) {
		ret = -EINVAL;
		goto out;
	}
	c->base = vma->vm_start;
	c->mm = vma->vm_mm;
	mmgrab(c->mm);
	c->mapped = true;
	c->sealed = true;
	vma->vm_ops = &sbk_vm_ops;
	vma->vm_private_data = c;
	vma->vm_flags |= VM_DONTEXPAND | VM_DONTDUMP;
	sbk_vma_open(vma);
out:
	mutex_unlock(&c->control);
	return ret;
}

static int sbk_open(struct inode *inode, struct file *file)
{
	struct sbk_context *c;
	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	c = kzalloc(sizeof(*c), GFP_KERNEL);
	if (!c)
		return -ENOMEM;
	if (!try_module_get(THIS_MODULE)) {
		kfree(c);
		return -ENODEV;
	}
	kref_init(&c->refs);
	INIT_WORK(&c->destroy, sbk_destroy);
#ifdef CONFIG_SWIFTBATON_PTE
	INIT_WORK(&c->drain, sbk_drain_work);
#endif
	mutex_init(&c->control);
	sbk_owner_init(&c->owner);
	file->private_data = c;
	return 0;
}
static int sbk_release(struct inode *inode, struct file *file)
{
	struct sbk_context *c = file->private_data;
#ifdef CONFIG_SWIFTBATON_PTE
	unsigned long i;
	/* Markers still mapped after close keep the context and transport alive. */
	for (i = 0; i < c->tokens_created; i++)
		sbk_pte_token_put(c->token_ids[i]);
#endif
	kref_put(&c->refs, sbk_release_ref);
	return 0;
}
static const struct file_operations sbk_fops = {
	.owner = THIS_MODULE, .open = sbk_open, .release = sbk_release,
	.mmap = sbk_mmap, .unlocked_ioctl = sbk_ioctl, .llseek = no_llseek,
};
static struct miscdevice sbk_device = {
	.minor = MISC_DYNAMIC_MINOR, .name = "swiftbaton_k", .mode = 0600,
	.fops = &sbk_fops,
};
static int __init sbk_init(void)
{
	int ret;
	reap_wq = alloc_workqueue("sbk_reap", WQ_UNBOUND, 1);
	if (!reap_wq)
		return -ENOMEM;
	ret = misc_register(&sbk_device);
	if (ret)
		destroy_workqueue(reap_wq);
	return ret;
}
static void __exit sbk_exit(void)
{
	misc_deregister(&sbk_device);
	destroy_workqueue(reap_wq);
}
module_init(sbk_init);
module_exit(sbk_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("SwiftBaton-K kernel page engine; test transport explicitly selected");
