/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_KERNEL_WORK_H
#define SB_KERNEL_WORK_H
#include <stdbool.h>
struct sbk_work_pool;
struct sbk_work_stats {
  unsigned submitted, completed, peak_active, peak_queued;
};
/* A successful submit transfers ownership to the pool. dispose runs exactly
 * once for every accepted item, including queued items discarded on error.
 * run/dispose execute without the pool mutex. One coordinator submits/finishes;
 * finish joins all workers before returning or releasing any queued payload. */
struct sbk_work_pool *sbk_work_create(unsigned workers, unsigned capacity,
                                     int (*run)(void *), void (*dispose)(void *));
int sbk_work_submit(struct sbk_work_pool *pool, void *item);
int sbk_work_finish(struct sbk_work_pool *pool, bool cancel,
                     struct sbk_work_stats *stats);
#endif
