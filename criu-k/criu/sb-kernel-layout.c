/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sb-kernel-layout.h"
#include "sb-kernel-sparse.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int read_values(int fd, uint64_t address, uint64_t *values, size_t pages)
{
  size_t bytes = pages * sizeof(*values), done = 0;
  while (done < bytes) {
    ssize_t n = pread(fd, (char *)values + done, bytes - done,
                       address / 4096 * sizeof(*values) + done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) return n ? -errno : -EIO;
    done += n;
  }
  return 0;
}
struct layout_reader {
  uint32_t pid;
  unsigned chunk_pages, allowance, emitted, skipped;
  uint64_t budget, total_pages;
  int pagemap;
  uint64_t *values;
  struct sbk_rdma_region *ranges;
  struct sbk_catalog_layout *out;
};

static int collect_vma(struct layout_reader *r, uint64_t start, uint64_t end)
{
  for (uint64_t addr = start; addr < end;) {
    uint64_t pages = (end - addr) / 4096;
    if (pages > (1U << 20)) pages = 1U << 20;
    if (r->emitted == r->allowance || r->total_pages == r->budget) {
      r->skipped++;
      return 0;
    }
    int ret = read_values(r->pagemap, addr, r->values, pages);
    if (ret) return ret;
    unsigned gap = 0, spans = sbk_sparse_ranges(r->values, pages, addr, 0, NULL);
    while (spans > r->allowance - r->emitted) {
      gap = gap ? gap * 2 : 1;
      spans = sbk_sparse_ranges(r->values, pages, addr, gap, NULL);
    }
    sbk_sparse_ranges(r->values, pages, addr, gap, r->ranges);
    for (unsigned j = 0; j < spans; j++) {
      uint64_t first = r->ranges[j].address, left = r->ranges[j].pages;
      while (left) {
        uint64_t n = left < r->chunk_pages ? left : r->chunk_pages;
        if (r->emitted == r->allowance || n > r->budget - r->total_pages) {
          r->skipped++;
          return 0;
        }
        r->out[r->emitted++] = (struct sbk_catalog_layout){
            .source_pid = r->pid, .address = first, .pages = n};
        r->total_pages += n;
        first += n * 4096;
        left -= n;
      }
    }
    addr += pages * 4096;
  }
  return 0;
}

static int collect_smaps_vma(struct layout_reader *r, uint64_t start,
                             uint64_t end, uint64_t rss_kb, uint64_t swap_kb)
{
  if (!rss_kb && !swap_kb) return 0;
  uint64_t virtual_pages = (end - start) / 4096;
  uint64_t resident_pages = rss_kb / 4 + swap_kb / 4;
  /* Extremely sparse guard/shadow VMAs can span terabytes. Preparation is
   * advisory, so omit those hints instead of delaying PS by scanning an
   * enormous pagemap. The final frozen plan still covers every data page. */
  if (virtual_pages > (1U << 20) && resident_pages < virtual_pages / 64) {
    r->skipped++;
    return 0;
  }
  return collect_vma(r, start, end);
}

static int candidate_process(uint32_t pid, unsigned chunk_pages, unsigned allowance,
                             uint64_t budget, struct sbk_catalog_layout *out,
                             unsigned *used, struct sbk_layout_stats *stats)
{
  char path[64], *line = NULL;
  size_t length = 0;
  FILE *smaps = NULL;
  struct layout_reader r = {.pid = pid, .chunk_pages = chunk_pages,
      .allowance = allowance, .budget = budget, .pagemap = -1, .out = out};
  uint64_t start = 0, end = 0, rss = 0, swap = 0;
  bool eligible = false;
  int ret = -ENOMEM;
  r.values = malloc((1U << 20) * sizeof(*r.values));
  r.ranges = calloc(allowance, sizeof(*r.ranges));
  if (!r.values || !r.ranges) goto out;
  snprintf(path, sizeof(path), "/proc/%u/smaps", pid);
  smaps = fopen(path, "re");
  if (!smaps) { ret = -errno; goto out; }
  snprintf(path, sizeof(path), "/proc/%u/pagemap", pid);
  r.pagemap = open(path, O_RDONLY | O_CLOEXEC);
  if (r.pagemap < 0) { ret = -errno; goto out; }
  /* smaps avoids letting enormous untouched VMAs consume the bounded PS
   * catalog before later resident VMAs. RSS/Swap is advisory only: final
   * frozen pagemap and MR planning still decide migration correctness. */
  while (getline(&line, &length, smaps) >= 0) {
    unsigned long long a, b, offset, inode, kb;
    unsigned major, minor;
    char perms[5];
    int used_chars = 0;
    if (sscanf(line, "%llx-%llx %4s %llx %x:%x %llu %n",
               &a, &b, perms, &offset, &major, &minor, &inode, &used_chars) == 7) {
      if (eligible) {
        ret = collect_smaps_vma(&r, start, end, rss, swap);
        if (ret) goto out;
      }
      if ((a & 4095) || (b & 4095) || b <= a) { ret = -EINVAL; goto out; }
      const char *name = line + used_chars;
      eligible = !inode && !major && !minor && perms[3] == 'p' &&
          (!name[0] || name[0] == '\n' || !strncmp(name, "[heap]", 6) ||
           !strncmp(name, "[stack", 6) || !strncmp(name, "[anon:", 6));
      start = a; end = b; rss = swap = 0;
    } else if (sscanf(line, "Rss: %llu kB", &kb) == 1) {
      rss = kb;
    } else if (sscanf(line, "Swap: %llu kB", &kb) == 1) {
      swap = kb;
    }
  }
  if (ferror(smaps)) { ret = -EIO; goto out; }
  if (eligible) {
    ret = collect_smaps_vma(&r, start, end, rss, swap);
    if (ret) goto out;
  }
  *used = r.emitted;
  stats->pages += r.total_pages;
  stats->skipped_ranges += r.skipped;
  ret = 0;
out:
  if (r.pagemap >= 0) close(r.pagemap);
  if (smaps) fclose(smaps);
  free(line);
  free(r.values);
  free(r.ranges);
  return ret;
}
int sbk_layout_collect(const uint32_t *pids, size_t count, unsigned chunk_pages,
                       struct sbk_catalog_layout **out, unsigned *nr,
                       struct sbk_layout_stats *stats)
{
  if (!out || !nr || !stats) return -EINVAL;
  *out = NULL; *nr = 0; memset(stats,0,sizeof(*stats));
  if ((count && !pids) || count > 4096 || !chunk_pages || chunk_pages > (1U << 20)) return -EINVAL;
  for (size_t i=0;i<count;i++) if (!pids[i] || pids[i]>INT32_MAX) return -EINVAL;
  struct sbk_catalog_layout *layout = calloc(SBK_MAX_REGIONS/2,sizeof(*layout));
  if (!layout) return -ENOMEM;
  unsigned used=0;
  for (size_t i=0;i<count;i++) {
    size_t prior; for(prior=0;prior<i;prior++) if(pids[prior]==pids[i]) break;
    if(prior<i) continue;
    unsigned allowance=(SBK_MAX_REGIONS/2-used)/(count-i), n=0;
    if(!allowance || stats->pages==SBK_TOKEN_POOL_MAX_PAGES) {stats->skipped_pids++;continue;}
    int ret=candidate_process(pids[i],chunk_pages,allowance,SBK_TOKEN_POOL_MAX_PAGES-stats->pages,
                               layout+used,&n,stats);
    if(ret==-ENOMEM) {free(layout);return ret;}
    if(ret) {stats->skipped_pids++;continue;}
    stats->pids++;used+=n;
  }
  *out=layout;*nr=used;return 0;
}
