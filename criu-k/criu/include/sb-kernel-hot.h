/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_HOT_H
#define CR_SB_KERNEL_HOT_H
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

/* A temporary address index into caller-owned final records. Sorting this
 * table never reorders MRs, and appending preserves the sampled score order. */
struct sbk_hot_range {
  uint64_t address, pages;
  uint64_t *order;
  size_t *used;
};
static inline int sbk_hot_range_compare(const void *a, const void *b)
{
  const struct sbk_hot_range *x = a, *y = b;
  return (x->address > y->address) - (x->address < y->address);
}
static inline int sbk_hot_index_prepare(struct sbk_hot_range *ranges, size_t count)
{
  if (count && !ranges)
    return -EINVAL;
  for (size_t i = 0; i < count; i++) {
    const struct sbk_hot_range *r = &ranges[i];
    if (!r->pages || (r->address & 4095) ||
        r->pages > (UINT64_MAX - r->address) / 4096 ||
        r->pages > SIZE_MAX / sizeof(*r->order) ||
        !r->order || !r->used || *r->used)
      return -EINVAL;
  }
  if (count > 1)
    qsort(ranges, count, sizeof(*ranges), sbk_hot_range_compare);
  for (size_t i = 1; i < count; i++)
    if (ranges[i - 1].address + ranges[i - 1].pages * 4096 > ranges[i].address)
      return -EINVAL;
  return 0;
}
static inline int sbk_hot_index_append(struct sbk_hot_range *ranges, size_t count,
                                     uint64_t address)
{
  size_t lo = 0, hi = count;
  while (lo < hi) {
    size_t mid = lo + (hi - lo) / 2;
    if (ranges[mid].address <= address)
      lo = mid + 1;
    else
      hi = mid;
  }
  if (lo) {
    struct sbk_hot_range *r = &ranges[lo - 1];
    uint64_t index = (address - r->address) / 4096;
    if (index < r->pages) {
      if (*r->used >= r->pages)
        return -EINVAL;
      r->order[(*r->used)++] = index;
    }
  }
  return 0; /* PS heat in a final hole/unmapped range has no final MR. */
}
#endif
