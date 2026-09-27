#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "sb-sched.h"
static int selected(size_t i,unsigned test) { return test%4==0?1:test%4==1?0:(i*17+test)%5!=0; }
int main(void) {
 for(unsigned test=0;test<2048;test++) {
  size_t pages=65+test%1024,first=test%13,count=pages-first;size_t bytes=sb_sched_size(pages,4096);void *mem;assert(!posix_memalign(&mem,64,bytes));
  memset(mem,test%2?0:0xa5,bytes);
  struct sb_sched *s=test%2?sb_sched_init_zeroed(mem,bytes,pages,4096):sb_sched_init(mem,bytes,pages,4096);assert(s);assert(sb_sched_attach(mem,bytes)==s);
  unsigned long *bits=calloc((count+63)/64,sizeof(*bits));assert(bits);uint64_t expected=0,n=0;
  for(size_t i=0;i<count;i++)if(selected(i,test))bits[i/64]|=1UL<<(i%64),expected++;
  // Ignore unused final-word bits; changing them must not seed past the range.
  if(count%64)bits[count/64]|=~((1UL<<(count%64))-1);
  assert(!sb_sched_seed_bitmap(s,first,count,bits,&n)&&n==expected);
  assert(sb_sched_seed_bitmap(s,pages+1,0,bits,&n)==-EINVAL);
  struct sb_job job;uint64_t committed=0;
  for(size_t i=0;i<pages;i++) {
   int seeded=i>=first&&selected(i-first,test);
   assert(sb_sched_state(s,i)==(seeded?SB_PAGE_PRECOPY_PENDING:SB_PAGE_IDLE));
   if(seeded) { assert(sb_sched_request(s,i,SB_DEMAND)==0);assert(!sb_sched_seed_commit(s,i)); }
   else { assert(sb_sched_request(s,i,SB_BACKGROUND)==1);assert(sb_sched_request(s,i,SB_DEMAND)==1); }
  }
  while(sb_sched_claim(s,SB_ALL_LANES,&job)>0) {assert(!sb_sched_commit(s,&job));committed++;}
  struct sb_sched_stats st;sb_sched_stats(s,&st);assert(committed+expected==pages&&st.committed==pages);
  assert(sb_sched_seed_bitmap(s,first,count,bits,&n)==(expected?-EALREADY:0));
  free(bits);free(mem);
 }
 return 0;
}
