/* SPDX-License-Identifier: GPL-2.0 */
#include "sb-kernel-sparse.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>

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
int sbk_sparse_plan_snapshot(int pid, const struct sbk_rdma_region *input, unsigned count,
                    unsigned capacity, struct sbk_rdma_region **out, unsigned *nr,
                    struct sbk_sparse_stats *stats, const struct sbk_pm_snapshot *snapshot)
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
  entries = snapshot ? NULL : malloc(max_pages * sizeof(*entries));
  if (!plan || (!snapshot && !entries)) {
    ret = -ENOMEM;
    goto fail;
  }
  snprintf(path, sizeof(path), "/proc/%d/pagemap", pid);
  fd = snapshot ? -1 : open(path, O_RDONLY | O_CLOEXEC);
  if (!snapshot && fd < 0) {
    ret = -errno;
    goto fail;
  }
  for (unsigned i = 0; i < count; i++) {
    const struct sbk_rdma_region *r = &input[i];
    const uint64_t *values = snapshot ? sbk_pm_snapshot_find(snapshot, r->address, r->pages) : entries;
    if (!values) { ret = -ERANGE; goto fail; }
    size_t bytes = r->pages * sizeof(*entries), done = 0;
    while (!snapshot && done < bytes) {
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
      stats->data_pages += !!(values[j] & HAS_DATA);
    /* Reserve at least one descriptor per remaining input chunk. If sparse
     * fragmentation exhausts its fair share, merge small gaps instead of
     * dropping data or overflowing the bounded source MR catalog. */
    unsigned allowance = (capacity - used) / (count - i), gap = 0;
    unsigned n = sbk_sparse_ranges(values, r->pages, r->address, gap, NULL);
    while (n > allowance) {
      gap = gap ? gap * 2 : 1;
      n = sbk_sparse_ranges(values, r->pages, r->address, gap, NULL);
    }
    sbk_sparse_ranges(values, r->pages, r->address, gap, plan + used);
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
  if (fd >= 0) close(fd);
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

int sbk_sparse_plan(int pid, const struct sbk_rdma_region *input, unsigned count,
                    unsigned capacity, struct sbk_rdma_region **out, unsigned *nr,
                    struct sbk_sparse_stats *stats)
{
  return sbk_sparse_plan_snapshot(pid,input,count,capacity,out,nr,stats,NULL);
}

struct sbk_pm_range { uint64_t address, pages, *entries; };
struct sbk_pm_snapshot { unsigned count; struct sbk_pm_range *ranges; };
struct pm_job { struct sbk_pm_range *range; uint64_t offset, pages; };
struct pm_reader { int fd, error; unsigned count, next; struct pm_job *jobs; };
static void *pm_read_worker(void *arg)
{
  struct pm_reader *reader = arg;
  while (!__atomic_load_n(&reader->error,__ATOMIC_RELAXED)) {
    unsigned index=__atomic_fetch_add(&reader->next,1,__ATOMIC_RELAXED);
    if(index>=reader->count) break;
    struct pm_job *j=&reader->jobs[index];
    size_t done=0, bytes=j->pages*sizeof(uint64_t);
    while(done<bytes) {
      ssize_t n=pread(reader->fd,(char *)(j->range->entries+j->offset)+done,bytes-done,
                       (j->range->address/4096+j->offset)*sizeof(uint64_t)+done);
      if(n<0&&errno==EINTR) continue;
      if(n<=0) { __atomic_store_n(&reader->error,n<0?-errno:-EIO,__ATOMIC_RELAXED); return NULL; }
      done+=n;
    }
  }
  return NULL;
}
void sbk_pm_snapshot_free(struct sbk_pm_snapshot *s)
{
  if(!s)return;
  for(unsigned i=0;i<s->count;i++)free(s->ranges[i].entries);
  free(s->ranges);free(s);
}
const uint64_t *sbk_pm_snapshot_find(const struct sbk_pm_snapshot *s,uint64_t address,uint64_t pages)
{
  if(!s||!pages||(address&4095)||pages>UINT64_MAX/4096||address>UINT64_MAX-pages*4096)return NULL;
  unsigned low=0,high=s->count;
  while(low<high) {unsigned mid=low+(high-low)/2;
    if(s->ranges[mid].address<=address)low=mid+1;else high=mid;
  }
  if(!low)return NULL;
  const struct sbk_pm_range *r=&s->ranges[low-1];
  uint64_t offset=(address-r->address)/4096;
  if(offset>r->pages||pages>r->pages-offset)return NULL;
  return r->entries+offset;
}
int sbk_pm_snapshot_create(int pid,const struct sbk_rdma_region *regions,unsigned count,
                           unsigned workers,struct sbk_pm_snapshot **out)
{
  if(!out)return -EINVAL;
  *out=NULL;
  if(pid<=0||!regions||!count||count>SBK_MAX_REGIONS||!workers||workers>32)return -EINVAL;
  uint64_t total=0,end=0;unsigned jobs=0;
  for(unsigned i=0;i<count;i++) {
    const struct sbk_rdma_region *r=&regions[i];
    if(!r->pages||r->pages>(1U<<20)||(r->address&4095)||r->address<end||
       r->address>UINT64_MAX-r->pages*4096)return -EINVAL;
    end=r->address+r->pages*4096;total+=r->pages;jobs+=(r->pages+8191)/8192;
  }
  /* Bound controller memory at 256 MiB; caller retains the serial fallback. */
  if(total>(1U<<25))return 0;
  struct sbk_pm_snapshot *s=calloc(1,sizeof(*s));
  struct pm_reader reader={.fd=-1,.count=jobs};pthread_t threads[32];unsigned started=0;
  int ret=-ENOMEM;char path[64];
  if(!s)return ret;
  s->ranges=calloc(count,sizeof(*s->ranges));reader.jobs=calloc(jobs,sizeof(*reader.jobs));
  if(!s->ranges||!reader.jobs)goto out;
  s->count=count;unsigned j=0;
  for(unsigned i=0;i<count;i++) {
    struct sbk_pm_range *r=&s->ranges[i];r->address=regions[i].address;r->pages=regions[i].pages;
    r->entries=malloc(r->pages*sizeof(uint64_t));if(!r->entries)goto out;
    for(uint64_t first=0;first<r->pages;first+=8192)reader.jobs[j++]=(struct pm_job){r,first,r->pages-first<8192?r->pages-first:8192};
  }
  snprintf(path,sizeof(path),"/proc/%d/pagemap",pid);reader.fd=open(path,O_RDONLY|O_CLOEXEC);
  if(reader.fd<0){ret=-errno;goto out;}
  if(workers>jobs)workers=jobs;
  for(unsigned i=1;i<workers;i++) {
    int e=pthread_create(&threads[started],NULL,pm_read_worker,&reader);
    if(e){__atomic_store_n(&reader.error,-e,__ATOMIC_RELAXED);break;}
    started++;
  }
  pm_read_worker(&reader);
  for(unsigned i=0;i<started;i++)pthread_join(threads[i],NULL);
  ret=__atomic_load_n(&reader.error,__ATOMIC_RELAXED);
  if(!ret){*out=s;s=NULL;}
out:
  if(reader.fd>=0)close(reader.fd);
  free(reader.jobs);sbk_pm_snapshot_free(s);return ret;
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
