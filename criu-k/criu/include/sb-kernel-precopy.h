/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_PRECOPY_H
#define CR_SB_KERNEL_PRECOPY_H
#include "sb-kernel-catalog.h"
#include <stdbool.h>
struct sbk_ps_region {
  struct sbk_catalog_record record;
  uint64_t *pfns, *indices;
  void *snapshot;
  size_t count;
};
/* Running-source snapshot: clear dirty epoch, record PFN identities, copy
 * candidates, register only the immutable controller snapshot. No PS ptrace
 * stop and no DMA pins on application pages; final validation is mandatory. */
/* begin plans the epoch and starts workers; next returns 1 for each planned
 * region (count==0 means raced-away/skip), 0 at EOF, or negative errno. finish
 * joins all workers before final validation, failure cleanup or snapshot free. */
int sb_kernel_ps_begin(int session, unsigned *nr);
int sb_kernel_ps_next(struct sbk_ps_region **record);
int sb_kernel_ps_finish(bool cancel);
int sb_kernel_ps_validate(int pid, const struct sbk_rdma_region *region,
                          uint64_t **dirty, size_t *nr);
void sb_kernel_ps_destroy(void);
#endif
