/* Actual planner and pagemap reads; no transport or source-page materialization. */
#define _GNU_SOURCE
#include "sb-kernel-sparse.h"
#include <assert.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#define P 4096UL
static void test_split(void) {
  struct sbk_rdma_region input[64], *out;
  unsigned nr, effective;
  for(unsigned step=1;step<=64;step++) {
    uint64_t address=0x100000, pages=0;
    for(unsigned i=0;i<step;i++) {
      input[i]=(struct sbk_rdma_region){.address=address,.pages=(i*173+step*29)%1048576+1};
      pages+=input[i].pages;address+=(input[i].pages+3)*P;
    }
    for(unsigned capacity=step;capacity<=256;capacity+=step) {
      assert(!sbk_split_plan(input,step,17,capacity,&out,&nr,&effective));
      assert(nr<=capacity&&effective>=17&&effective<=1048576);
      unsigned index=0;uint64_t sum=0;
      for(unsigned i=0;i<step;i++) {
        uint64_t next=input[i].address, end=next+input[i].pages*P;
        while(next<end) {
          assert(index<nr&&out[index].address==next&&out[index].pages&&out[index].pages<=effective);
          next+=out[index].pages*P;sum+=out[index].pages;
          assert(next<=end&&!out[index].id&&!out[index].rkey);index++;
        }
      }
      assert(index==nr&&sum==pages);free(out);
    }
  }
  /* Raw candidate count can exceed 32 bits; cap adaptation must not wrap. */
  struct sbk_rdma_region *large=calloc(SBK_MAX_REGIONS,sizeof(*large));assert(large);
  for(unsigned i=0;i<SBK_MAX_REGIONS;i++)
    large[i]=(struct sbk_rdma_region){.address=0x100000+(uint64_t)i*(1ULL<<32),.pages=1<<20};
  assert(!sbk_split_plan(large,SBK_MAX_REGIONS,1,SBK_MAX_REGIONS,&out,&nr,&effective));
  assert(nr==SBK_MAX_REGIONS&&effective==(1U<<20));free(out);free(large);
  input[0]=(struct sbk_rdma_region){.address=0x100000,.pages=4};
  assert(sbk_split_plan(input,1,0,32,&out,&nr,&effective)<0);
  input[0].address=UINT64_MAX-4095;
  assert(sbk_split_plan(input,1,1,32,&out,&nr,&effective)<0);
  puts("PASS final_MR_split_exact_coverage_order_gaps_capacity_and_64bit_count");
}
int main(void) {
  test_split();
  uint64_t entries[64] = {0};
  struct sbk_rdma_region spans[64];
  entries[2] = 1ULL<<63; entries[3] = 1ULL<<62; /* Swapped data cannot be skipped. */
  entries[31] = 1ULL<<55; /* Soft-dirty on an absent PTE is not resident data. */
  entries[60] = 1ULL<<63;
  assert(sbk_sparse_ranges(entries,64,0x100000,0,spans)==2);
  assert(spans[0].address==0x102000 && spans[0].pages==2 && spans[1].pages==1);
  assert(sbk_sparse_ranges(entries,64,0x100000,56,spans)==1 && spans[0].pages==59);
  /* Exhaustively verify preservation/bounds for varied sparse patterns/gap caps. */
  for (unsigned mask=0; mask<65536; mask++) {
    for(unsigned i=0;i<16;i++)entries[i]=(mask&(1U<<i))?1ULL<<(i%2?62:63):0;
    for(unsigned gap=0;gap<5;gap++) {
      unsigned n=sbk_sparse_ranges(entries,16,0x100000,gap,spans);
      uint64_t last=0;
      for(unsigned j=0;j<n;j++) {
        assert(spans[j].address>=0x100000 && spans[j].address>=last);
        last=spans[j].address+spans[j].pages*P;assert(last<=0x110000);
      }
      for(unsigned i=0;i<16;i++)if(mask&(1U<<i)) {
        unsigned covered=0;
        for(unsigned j=0;j<n;j++)covered+=0x100000+i*P>=spans[j].address && 0x100000+i*P<spans[j].address+spans[j].pages*P;
        assert(covered==1);
      }
    }
  }
  enum { NP=32768 };
  unsigned char *p=mmap(NULL,NP*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  assert(p!=MAP_FAILED && !madvise(p,NP*P,MADV_NOHUGEPAGE));
  p[9*P]=21;p[10*P]=22;p[30000*P]=23;
  unsigned char before[NP],after[NP];assert(!mincore(p,NP*P,before));
  struct sbk_rdma_region input={.address=(uintptr_t)p,.pages=NP},*plan=NULL;
  struct sbk_sparse_stats st;unsigned nr=0;
  assert(!sbk_sparse_plan(getpid(),&input,1,32,&plan,&nr,&st));
  assert(nr==2 && st.virtual_pages==NP && st.data_pages==3 && st.registered_pages==3);
  assert(!mincore(p,NP*P,after) && !memcmp(before,after,NP));
  free(plan);
  for(unsigned workers=1;workers<=32;workers*=2) {
    struct sbk_pm_snapshot *snapshot=NULL;
    assert(!sbk_pm_snapshot_create(getpid(),&input,1,workers,&snapshot)&&snapshot);
    assert(!sbk_sparse_plan_snapshot(getpid(),&input,1,32,&plan,&nr,&st,snapshot));
    assert(nr==2&&st.data_pages==3&&st.registered_pages==3);free(plan);
    const uint64_t *entry=sbk_pm_snapshot_find(snapshot,input.address+9*P,2);assert(entry&&(entry[0]&(1ULL<<63))&&(entry[1]&(1ULL<<63)));
    assert(!sbk_pm_snapshot_find(snapshot,input.address-4096,1));
    assert(!sbk_pm_snapshot_find(snapshot,input.address,NP+1));
    assert(!sbk_pm_snapshot_find(snapshot,input.address+1,1));
    assert(!sbk_pm_snapshot_find(snapshot,UINT64_MAX-4095,1));
    assert(!mincore(p,NP*P,after)&&!memcmp(before,after,NP));
    sbk_pm_snapshot_free(snapshot);
  }
  struct sbk_pm_snapshot *failed=(void *)1;
  assert(sbk_pm_snapshot_create(2147483647,&input,1,4,&failed)<0&&!failed);
  struct sbk_rdma_region oversized[33];
  for(unsigned i=0;i<33;i++)oversized[i]=(struct sbk_rdma_region){.address=0x100000+(uint64_t)i*(1ULL<<32),.pages=1U<<20};
  assert(!sbk_pm_snapshot_create(getpid(),oversized,33,4,&failed)&&!failed);
  assert(sbk_pm_snapshot_create(getpid(),&input,1,33,&failed)<0&&!failed);
  assert(!sbk_sparse_plan(getpid(),&input,1,1,&plan,&nr,&st));
  assert(nr==1 && st.registered_pages==30000-9+1);
  assert(!mincore(p,NP*P,after) && !memcmp(before,after,NP));
  free(plan);
  assert(!madvise(p,NP*P,MADV_DONTNEED));
  assert(!sbk_sparse_plan(getpid(),&input,1,32,&plan,&nr,&st));
  assert(nr==1 && st.data_pages==0 && st.registered_pages==1 && st.control_page==1);
  assert(!mincore(p,NP*P,after));for(unsigned i=0;i<NP;i++)assert(!(after[i]&1));
  free(plan);assert(!munmap(p,NP*P));
  puts("PASS sparse_planner_present_swap_exhaustive_coverage_budget_and_no_source_population");
  return 0;
}
