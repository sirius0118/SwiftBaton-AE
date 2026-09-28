/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SB_KERNEL_LAYOUT_H
#define SB_KERNEL_LAYOUT_H
#include "sb-kernel-catalog.h"
struct sbk_layout_stats { uint64_t pages; unsigned pids, skipped_pids, skipped_ranges; };
/* Running-source advisory observation only. Never pins/faults source pages or
 * authorizes omission of a final range. Final frozen planning is authoritative.
 * Inaccessible/racing processes may contribute no hints; resource limits omit
 * preparation hints, never migration data. */
int sbk_layout_collect(const uint32_t *pids, size_t count, unsigned chunk_pages,
                       struct sbk_catalog_layout **out, unsigned *nr,
                       struct sbk_layout_stats *stats);
#endif
