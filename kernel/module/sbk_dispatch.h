/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SBK_DISPATCH_H
#define SBK_DISPATCH_H
#include <linux/list.h>
#include <linux/spinlock.h>
#include <linux/wait.h>
#include "sbk_uapi.h"
struct sbk_dispatch;
struct sbk_work_owner {
	spinlock_t gate;
	wait_queue_head_t done;
	unsigned int pending;
	bool stopping;
};
struct sbk_work {
	struct list_head link;
	struct sbk_work_owner *owner;
	/* true yields this still-owned job to the back of the runnable queue. */
	bool (*step)(struct sbk_work *);
	bool busy, hot;
};
void sbk_owner_init(struct sbk_work_owner *owner);
void sbk_work_init(struct sbk_work *work, struct sbk_work_owner *owner,
		   bool (*step)(struct sbk_work *), bool hot);
bool sbk_dispatch_queue(struct sbk_dispatch *pool, struct sbk_work *work);
void sbk_work_wait(struct sbk_work *work);
/* Close admission and join this owner's jobs only, never sibling regions. */
void sbk_owner_stop(struct sbk_work_owner *owner);
struct sbk_dispatch *sbk_dispatch_create(unsigned int workers, bool prefetch);
void sbk_dispatch_destroy(struct sbk_dispatch *pool);
void sbk_dispatch_stats(struct sbk_dispatch *pool, struct sbk_dispatch_lane_stats *stats);
#endif
