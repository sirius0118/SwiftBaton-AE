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
/* PS identifies monotone address runs without reordering the heat sequence.
 * Each final run then merges with the sorted final ranges in linear time.
 * The source may change its VMAs after PS: only final ranges choose coverage.
 * A fragmented hint sequence keeps the original per-address binary search.
 * These immutable hints never assert page validity or keep source pages alive. */
struct sbk_hot_run {
  size_t first, count;
  int ascending;
};
#define SBK_HOT_MAX_RUNS 4096
static inline int sbk_hot_runs_prepare(const uint64_t *addresses, size_t count,
                                      struct sbk_hot_run **out, size_t *nr)
{
  if (!out || !nr || (count && !addresses)) return -EINVAL;
  *out = NULL;
  *nr = 0;
  if (!count) return 0;
  size_t capacity = count < SBK_HOT_MAX_RUNS ? count : SBK_HOT_MAX_RUNS;
  struct sbk_hot_run *runs = malloc(capacity * sizeof(*runs));
  if (!runs) return -ENOMEM;
  size_t n = 0;
  for (size_t first = 0; first < count;) {
    if (n == capacity) { free(runs); return 0; }
    size_t end = first + 1;
    int ascending = end == count || addresses[end] >= addresses[first];
    while (end < count && (ascending ? addresses[end] >= addresses[end - 1]
                                    : addresses[end] <= addresses[end - 1])) end++;
    runs[n++] = (struct sbk_hot_run){first, end - first, ascending};
    first = end;
  }
  /* Short runs save little and add branch/lookup overhead. This is only an
   * optimization choice; both paths preserve identical indices and order. */
  if (n > 1 && count / n < 16) { free(runs); return 0; }
  *out = runs;
  *nr = n;
  return 0;
}
static inline int sbk_hot_index_append_runs(struct sbk_hot_range *ranges, size_t count,
                                           const uint64_t *addresses, size_t pages,
                                           const struct sbk_hot_run *runs, size_t nr)
{
  if ((pages && !addresses) || (count && !ranges) || (nr && !runs)) return -EINVAL;
  if (!nr) {
    for (size_t i = 0; i < pages; i++) {
      int ret = sbk_hot_index_append(ranges, count, addresses[i]);
      if (ret) return ret;
    }
    return 0;
  }
  /* Validate the plan before any output. It must cover the immutable snapshot
   * once, in its original order; PS already verified each run's direction. */
  size_t end = 0;
  for (size_t r = 0; r < nr; r++) {
    if (runs[r].first != end || !runs[r].count || runs[r].count > pages - end ||
        (runs[r].ascending != 0 && runs[r].ascending != 1)) return -EINVAL;
    end += runs[r].count;
  }
  if (end != pages) return -EINVAL;
  for (size_t r = 0; r < nr; r++) {
    const struct sbk_hot_run *run = &runs[r];
    uint64_t first = addresses[run->first];
    size_t lo = 0, hi = count;
    while (lo < hi) {
      size_t mid = lo + (hi - lo) / 2;
      if (ranges[mid].address <= first) lo = mid + 1;
      else hi = mid;
    }
    for (size_t i = run->first; i < run->first + run->count; i++) {
      uint64_t address = addresses[i];
      if (run->ascending) {
        while (lo < count && ranges[lo].address <= address) lo++;
      } else {
        while (lo && ranges[lo - 1].address > address) lo--;
      }
      if (lo) {
        struct sbk_hot_range *range = &ranges[lo - 1];
        uint64_t index = (address - range->address) / 4096;
        if (index < range->pages) {
          if (*range->used >= range->pages) return -EINVAL;
          range->order[(*range->used)++] = index;
        }
      }
    }
  }
  return 0;
}

#endif
