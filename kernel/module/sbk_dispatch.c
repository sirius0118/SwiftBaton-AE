// SPDX-License-Identifier: GPL-2.0
/* Fixed session-wide CPU workers. Demand faults never enter these queues. */
#include <linux/kthread.h>
#include <linux/sched.h>
#include <linux/slab.h>
#include "sbk_dispatch.h"

struct sbk_dispatch {
	spinlock_t lock;
	wait_queue_head_t ready;
	struct list_head hot, normal;
	struct task_struct *threads[SBK_RDMA_MAX_SLOTS];
	struct sbk_dispatch_lane_stats stats;
	unsigned int launched;
	bool prefetch;
};
void sbk_owner_init(struct sbk_work_owner *o)
{
	spin_lock_init(&o->gate);
	init_waitqueue_head(&o->done);
	o->pending = 0;
	o->stopping = false;
}
void sbk_work_init(struct sbk_work *w, struct sbk_work_owner *o,
		   bool (*step)(struct sbk_work *), bool hot)
{
	INIT_LIST_HEAD(&w->link);
	w->owner = o;
	w->step = step;
	w->busy = false;
	w->hot = hot;
}
/* Caller holds owner.gate. Workers never hold pool.lock while taking that gate. */
static void sbk_dispatch_push(struct sbk_dispatch *p, struct sbk_work *w, bool first)
{
	spin_lock(&p->lock);
	list_add_tail(&w->link, w->hot ? &p->hot : &p->normal);
	p->stats.queued++;
	p->stats.queue_peak = max(p->stats.queue_peak, p->stats.queued);
	if (first)
		p->stats.submitted++;
	else
		p->stats.active--;
	spin_unlock(&p->lock);
	wake_up(&p->ready);
}
bool sbk_dispatch_queue(struct sbk_dispatch *p, struct sbk_work *w)
{
	struct sbk_work_owner *o = w->owner;
	unsigned long flags;
	bool queued = false;
	spin_lock_irqsave(&o->gate, flags);
	if (!o->stopping && !w->busy) {
		w->busy = true;
		o->pending++;
		sbk_dispatch_push(p, w, true);
		queued = true;
	}
	spin_unlock_irqrestore(&o->gate, flags);
	return queued;
}
static int sbk_dispatch_thread(void *arg)
{
	struct sbk_dispatch *p = arg;
	if (p->prefetch)
		set_user_nice(current, -20); /* Same class as WQ_HIGHPRI, not realtime. */
	for (;;) {
		struct sbk_work *w;
		struct sbk_work_owner *o;
		struct list_head *head;
		unsigned long flags;
		bool again;
		wait_event(p->ready, kthread_should_stop() || READ_ONCE(p->stats.queued));
		if (kthread_should_stop())
			break;
		spin_lock_irqsave(&p->lock, flags);
		head = !list_empty(&p->hot) ? &p->hot : &p->normal;
		if (list_empty(head)) {
			spin_unlock_irqrestore(&p->lock, flags);
			continue;
		}
		w = list_first_entry(head, struct sbk_work, link);
		list_del_init(&w->link);
		p->stats.queued--;
		p->stats.active++;
		p->stats.peak = max(p->stats.peak, p->stats.active);
		p->stats.quanta++;
		spin_unlock_irqrestore(&p->lock, flags);
		o = w->owner;
		again = w->step(w);
		spin_lock_irqsave(&o->gate, flags);
		if (again && !o->stopping) {
			sbk_dispatch_push(p, w, false);
		} else {
			spin_lock(&p->lock);
			p->stats.active--;
			p->stats.completed++;
			spin_unlock(&p->lock);
			w->busy = false;
			o->pending--;
			wake_up_all(&o->done);
		}
		spin_unlock_irqrestore(&o->gate, flags);
		/* Owner and job can be freed after this unlock. Do not touch either.
		 * The transport joins these threads before freeing the pool itself. */
		cond_resched();
	}
	return 0;
}
void sbk_work_wait(struct sbk_work *w)
{
	struct sbk_work_owner *o = w->owner;
	unsigned long flags;
	wait_event(o->done, !READ_ONCE(w->busy));
	/* Pair with the final wake while gate is held, before freeing stack jobs. */
	spin_lock_irqsave(&o->gate, flags);
	spin_unlock_irqrestore(&o->gate, flags);
}
void sbk_owner_stop(struct sbk_work_owner *o)
{
	unsigned long flags;
	spin_lock_irqsave(&o->gate, flags);
	o->stopping = true;
	spin_unlock_irqrestore(&o->gate, flags);
	wait_event(o->done, !READ_ONCE(o->pending));
	spin_lock_irqsave(&o->gate, flags);
	spin_unlock_irqrestore(&o->gate, flags);
}
void sbk_dispatch_destroy(struct sbk_dispatch *p)
{
	unsigned int i;
	if (!p)
		return;
	/* Each region has closed admission and joined its own jobs first. */
	WARN_ON(p->stats.queued || p->stats.active);
	for (i = 0; i < p->launched; i++)
		kthread_stop(p->threads[i]);
	kfree(p);
}
struct sbk_dispatch *sbk_dispatch_create(unsigned int workers, bool prefetch)
{
	struct sbk_dispatch *p;
	unsigned int i;
	int ret;
	if (!workers || workers > SBK_RDMA_MAX_SLOTS)
		return ERR_PTR(-EINVAL);
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return ERR_PTR(-ENOMEM);
	spin_lock_init(&p->lock);
	init_waitqueue_head(&p->ready);
	INIT_LIST_HEAD(&p->hot);
	INIT_LIST_HEAD(&p->normal);
	p->stats.workers = workers;
	p->prefetch = prefetch;
	for (i = 0; i < workers; i++) {
		p->threads[i] = kthread_create(sbk_dispatch_thread, p,
					      prefetch ? "sbk_ft/%u" : "sbk_bg/%u", i);
		if (IS_ERR(p->threads[i])) {
			ret = PTR_ERR(p->threads[i]);
			goto fail;
		}
		p->launched++;
		/* pageclient affinity is set before RDMA_CREATE, including NUMA tests.
		 * This is CPU locality, not a promise to inherit task memory policy. */
		ret = set_cpus_allowed_ptr(p->threads[i], current->cpus_ptr);
		if (ret)
			goto fail;
		wake_up_process(p->threads[i]);
	}
	return p;
fail:
	sbk_dispatch_destroy(p);
	return ERR_PTR(ret);
}
void sbk_dispatch_stats(struct sbk_dispatch *p, struct sbk_dispatch_lane_stats *stats)
{
	unsigned long flags;
	spin_lock_irqsave(&p->lock, flags);
	*stats = p->stats;
	spin_unlock_irqrestore(&p->lock, flags);
}
