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
/* One frozen application epoch; never reuse this after source resume or MR
 * registration. Bounds-checked slices let sparse planning and cache validation
 * share the same pagemap observation. Large maps may decline this optimization. */
struct sbk_pm_snapshot;
int sbk_pm_snapshot_create(int pid, const struct sbk_rdma_region *regions, unsigned count,
                           unsigned workers, struct sbk_pm_snapshot **out);
const uint64_t *sbk_pm_snapshot_find(const struct sbk_pm_snapshot *snapshot,
                                    uint64_t address, uint64_t pages);
void sbk_pm_snapshot_free(struct sbk_pm_snapshot *snapshot);
int sbk_sparse_plan_snapshot(int pid, const struct sbk_rdma_region *input, unsigned count,
                    unsigned capacity, struct sbk_rdma_region **out, unsigned *nr,
                    struct sbk_sparse_stats *stats, const struct sbk_pm_snapshot *snapshot);
/* Deterministic span generator, also exercised by the standalone regression. */
unsigned sbk_sparse_ranges(const uint64_t *entries, unsigned pages, uint64_t address,
                            unsigned merge_gap, struct sbk_rdma_region *out);
/* Split without changing byte coverage or order. Grow the requested span if
 * necessary to fit capacity; never discard a page to satisfy a size target. */
int sbk_split_plan(const struct sbk_rdma_region *input, unsigned count,
                   unsigned chunk_pages, unsigned capacity,
                   struct sbk_rdma_region **out, unsigned *nr, unsigned *effective_pages);
#endif
