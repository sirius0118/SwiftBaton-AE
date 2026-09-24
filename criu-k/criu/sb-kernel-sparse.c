/* SPDX-License-Identifier: GPL-2.0 */
#include "sb-kernel-sparse.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define HAS_DATA ((1ULL << 63) | (1ULL << 62))
unsigned sbk_sparse_ranges(const uint64_t *entries, unsigned pages, uint64_t address,
                            unsigned merge_gap, struct sbk_rdma_region *out)
{
  unsigned count = 0, begin = 0, last = 0;
  int active = 0;
  for (unsigned i = 0; i < pages; i++) {
    if (!(entries[i] & HAS_DATA))
      continue;
    if (active && i - last - 1 > merge_gap) {
      if (out)
        out[count] = (struct sbk_rdma_region){.address = address + (uint64_t)begin * 4096,
                                              .pages = last - begin + 1};
      count++;
      active = 0;
    }
    if (!active) {
      begin = i;
      active = 1;
    }
    last = i;
  }
  if (active) {
    if (out)
      out[count] = (struct sbk_rdma_region){.address = address + (uint64_t)begin * 4096,
                                            .pages = last - begin + 1};
    count++;
  }
  return count;
}
int sbk_sparse_plan(int pid, const struct sbk_rdma_region *input, unsigned count,
                    unsigned capacity, struct sbk_rdma_region **out, unsigned *nr,
                    struct sbk_sparse_stats *stats)
{
  char path[64];
  int fd = -1, ret = -EINVAL;
  uint64_t *entries = NULL;
  struct sbk_rdma_region *plan = NULL;
  unsigned used = 0;
  if (!out || !nr || !stats)
    return -EINVAL;
  *out = NULL;
  *nr = 0;
  memset(stats, 0, sizeof(*stats));
  if (pid <= 0 || !count || !input || capacity < count || capacity > SBK_MAX_REGIONS)
    return -EINVAL;
  uint64_t end = 0, max_pages = 0;
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    if (!r->pages || r->pages > (1U << 20) || (r->address & 4095) ||
        r->address < end || r->address > UINT64_MAX - r->pages * 4096)
      return -EINVAL;
    end = r->address + r->pages * 4096;
    if (r->pages > max_pages)
      max_pages = r->pages;
  }
  plan = calloc(capacity, sizeof(*plan));
  entries = malloc(max_pages * sizeof(*entries));
  if (!plan || !entries) {
    ret = -ENOMEM;
    goto fail;
  }
  snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
  fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) {
    ret = -errno;
    goto fail;
  }
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    size_t bytes = r->pages * sizeof(*entries), done = 0;
    while (done < bytes) {
      ssize_t n = pread(fd, (char *)entries + done, bytes - done,
                        r->address / 4096 * sizeof(*entries) + done);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        ret = n < 0 ? -errno : -EIO;
        goto fail;
      }
      done += n;
    }
    stats->virtual_pages += r->pages;
    for (uint64_t j = 0; j < r->pages; j++)
      stats->data_pages += !!(entries[j] & HAS_DATA);
    /* Reserve at least one descriptor per remaining input chunk. If sparse
     * fragmentation exhausts its fair share, merge small gaps instead of
     * dropping data or overflowing the bounded source MR catalog. */
    unsigned allowance = (capacity - used) / (count - i), gap = 0;
    unsigned n = sbk_sparse_ranges(entries, r->pages, r->address, gap, NULL);
    while (n > allowance) {
      gap = gap ? gap * 2 : 1;
      n = sbk_sparse_ranges(entries, r->pages, r->address, gap, NULL);
    }
    sbk_sparse_ranges(entries, r->pages, r->address, gap, plan + used);
    used += n;
  }
  if (!used) {
    /* Current per-process restore handshake needs a nonempty region. Keep
     * just one zero page for an entirely empty lazy address space, not a VMA. */
    plan[used++] = (struct sbk_rdma_region){.address = input[0].address, .pages = 1};
    stats->control_page = 1;
  }
  for (unsigned i = 0; i < used; i++)
    stats->registered_pages += plan[i].pages;
  close(fd);
  free(entries);
  *out = plan;
  *nr = used;
  return 0;
fail:
  if (fd >= 0)
    close(fd);
  free(entries);
  free(plan);
  return ret;
}

int sbk_split_plan(const struct sbk_rdma_region *input, unsigned count,
                   unsigned chunk_pages, unsigned capacity,
                   struct sbk_rdma_region **out, unsigned *nr, unsigned *effective_pages)
{
  uint64_t end = 0;
  uint64_t needed;
  unsigned used = 0;
  if (!out || !nr || !effective_pages) return -EINVAL;
  *out = NULL; *nr = 0; *effective_pages = 0;
  if (!input || !count || count > capacity || capacity > SBK_MAX_REGIONS ||
      !chunk_pages || chunk_pages > (1U << 20)) return -EINVAL;
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    if (!r->pages || r->pages > (1U << 20) || r->id || r->rkey ||
        (r->address & 4095) || r->address < end ||
        r->address > UINT64_MAX - r->pages * 4096) return -EINVAL;
    end = r->address + r->pages * 4096;
  }
  for (;;) {
    needed = 0;
    for (unsigned i = 0; i < count; i++)
      needed += (input[i].pages + chunk_pages - 1) / chunk_pages;
    if (needed <= capacity) break;
    chunk_pages = chunk_pages > (1U << 19) ? (1U << 20) : chunk_pages * 2;
  }
  struct sbk_rdma_region *plan = calloc(needed, sizeof(*plan));
  if (!plan) return -ENOMEM;
  for (unsigned i = 0; i < count; i++) {
    uint64_t pages = input[i].pages, address = input[i].address;
    while (pages) {
      unsigned n = pages < chunk_pages ? pages : chunk_pages;
      plan[used++] = (struct sbk_rdma_region){.address=address, .pages=n};
      address += (uint64_t)n * 4096; pages -= n;
    }
  }
  *out = plan; *nr = used; *effective_pages = chunk_pages;
  return 0;
}
