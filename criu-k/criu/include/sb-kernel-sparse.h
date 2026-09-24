/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_SPARSE_H
#define CR_SB_KERNEL_SPARSE_H
#include "common/sbk_uapi.h"
#include <stdint.h>
struct sbk_sparse_stats { uint64_t virtual_pages, data_pages, registered_pages; unsigned control_page; };
/* Caller must freeze all source application threads before planning and keep
 * them frozen through MR export and migration. Reads pagemap only; never faults
 * source data in. Both present and swapped entries must be preserved. */
int sbk_sparse_plan(int pid, const struct sbk_rdma_region *input, unsigned count,
                    unsigned capacity, struct sbk_rdma_region **out, unsigned *nr,
                    struct sbk_sparse_stats *stats);
/* Deterministic span generator, also exercised by the standalone regression. */
unsigned sbk_sparse_ranges(const uint64_t *entries, unsigned pages, uint64_t address,
                            unsigned merge_gap, struct sbk_rdma_region *out);
/* Split without changing byte coverage or order. Grow the requested span if
 * necessary to fit capacity; never discard a page to satisfy a size target. */
int sbk_split_plan(const struct sbk_rdma_region *input, unsigned count,
                   unsigned chunk_pages, unsigned capacity,
                   struct sbk_rdma_region **out, unsigned *nr, unsigned *effective_pages);
#endif
