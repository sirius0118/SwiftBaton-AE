/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static int fail_alloc;
static void *hot_alloc(size_t bytes) { return fail_alloc ? NULL : malloc(bytes); }
#define malloc hot_alloc
#include "sb-kernel-hot.h"
#undef malloc

static uint64_t seed = 17;
static uint64_t next_random(void) { seed ^= seed << 13; seed ^= seed >> 7; return seed ^= seed << 17; }
static uint64_t now(void) { struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC, &t)); return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec; }
static void check(const uint64_t *a, size_t n, const uint64_t *start,
                  const uint64_t *length, size_t regions, int strategy)
{
  struct sbk_hot_run *runs = NULL; size_t nr = 0;
  assert(!sbk_hot_runs_prepare(a, n, &runs, &nr));
  if (strategy >= 0) assert(!!nr == strategy);
  struct sbk_hot_range *r = calloc(regions ? regions : 1, sizeof(*r));
  uint64_t **expected = calloc(regions ? regions : 1, sizeof(*expected));
  size_t *used = calloc(regions ? regions : 1, sizeof(*used));
  size_t *counts = calloc(regions ? regions : 1, sizeof(*counts));
  assert(r && expected && used && counts);
  for (size_t j = 0; j < regions; j++) {
    /* Reverse descriptor order to exercise the existing address sort. */
    r[regions - j - 1] = (struct sbk_hot_range){start[j], length[j], calloc(length[j], sizeof(uint64_t)), &used[j]};
    expected[j] = calloc(length[j], sizeof(uint64_t));
    assert(r[regions - j - 1].order && expected[j]);
  }
  assert(!sbk_hot_index_prepare(r, regions));
  /* Independent linear oracle; never shares the optimized range search. */
  int want = 0;
  for (size_t i = 0; i < n && !want; i++)
    for (size_t j = 0; j < regions; j++)
      if (a[i] >= start[j] && (a[i] - start[j]) / 4096 < length[j]) {
        if (counts[j] == length[j]) { want = -EINVAL; break; }
        expected[j][counts[j]++] = (a[i] - start[j]) / 4096;
        break;
      }
  assert(sbk_hot_index_append_runs(r, regions, a, n, runs, nr) == want);
  for (size_t j = 0; j < regions; j++) {
    assert(used[j] == counts[j]);
    assert(!memcmp(r[j].order, expected[j], used[j] * sizeof(uint64_t)));
    free(r[j].order); free(expected[j]);
  }
  free(r); free(expected); free(used); free(counts); free(runs);
}
static void benchmark(void)
{
  const size_t n = 1560000, count = 414;
  uint64_t *a = malloc(n * sizeof(*a)); assert(a);
  size_t next = 0;
  for (unsigned heat = 0; heat < 6; heat++)
    for (size_t i = n; i--;)
      if (i % 6 == heat) a[next++] = (i + 1024) * 4096;
  assert(next == n);
  struct sbk_hot_run *runs = NULL; size_t nr = 0;
  uint64_t begin = now(); assert(!sbk_hot_runs_prepare(a, n, &runs, &nr));
  printf("PS plan pages=%zu runs=%zu time_ms=%.3f\n", n, nr, (now()-begin)/1e6);
  assert(nr && nr <= 6);
  struct sbk_hot_range r[count]; size_t used[count];
  for (size_t j = 0; j < count; j++) {
    used[j] = 0; r[j] = (struct sbk_hot_range){(1024+j*4097)*4096, 4096, malloc(4096*sizeof(uint64_t)), &used[j]};
    assert(r[j].order);
  }
  assert(!sbk_hot_index_prepare(r, count));
  uint64_t *reference = malloc(n * sizeof(*reference)); assert(reference);
  size_t counts[count];
  for (int iteration = 0; iteration < 6; iteration++) {
    memset(used, 0, sizeof(used)); begin = now();
    for (size_t i = 0; i < n; i++) assert(!sbk_hot_index_append(r,count,a[i]));
    uint64_t binary = now()-begin; size_t at = 0;
    for (size_t j = 0; j < count; j++) { counts[j]=used[j]; memcpy(reference+at,r[j].order,used[j]*8); at+=used[j]; }
    memset(used, 0, sizeof(used)); begin=now();
    assert(!sbk_hot_index_append_runs(r,count,a,n,runs,nr));
    uint64_t merge=now()-begin; at=0;
    for (size_t j=0;j<count;j++) {assert(used[j]==counts[j]); assert(!memcmp(reference+at,r[j].order,used[j]*8));at+=used[j];}
    printf("same-input repeat=%d binary_ms=%.3f merge_ms=%.3f\n",iteration,binary/1e6,merge/1e6);
  }
  for (size_t j=0;j<count;j++) free(r[j].order);
  free(reference);free(a);free(runs);
}
int main(int argc, char **argv)
{
  (void)argv;
  uint64_t start[]={0x1000,0x41000,0x101000,0x1f1000}, len[]={32,128,240,512};
  size_t n=2048; uint64_t a[n];
  for (int iteration=0;iteration<250;iteration++) {
    for (size_t i=0;i<n;i++) a[i]=i*4096;
    if (iteration%4==0) {
      for(size_t i=0;i<n;i++) a[i]=(n-i-1)*4096;
    } else if (iteration%4==1) {
      for(size_t i=n-1;i;i--) {size_t j=next_random()%(i+1);uint64_t t=a[i];a[i]=a[j];a[j]=t;}
    } else if (iteration%4==2) {
      size_t at=0;for(unsigned score=0;score<7;score++)for(size_t i=n;i--;)if(i%7==score)a[at++]=i*4096;
    }
    check(a,n,start,len,4,iteration%4==1?0:1);
  }
  uint64_t extremes[]={UINT64_MAX,UINT64_MAX-1,0x1800,0x1000,0};
  check(extremes,5,start,len,4,1);
  uint64_t duplicates[128]; for(size_t i=0;i<128;i++)duplicates[i]=0x1000;
  check(duplicates,128,start,len,4,1); /* Same bounded-output failure. */
  check(NULL,0,NULL,NULL,0,0);check(a,n,NULL,NULL,0,-1);
  uint64_t fragmented[20000];for(size_t i=0;i<20000;i++)fragmented[i]=(i%2?20000-i:i)*4096;
  check(fragmented,20000,NULL,NULL,0,0); /* Bounded run capacity fallback. */
  struct sbk_hot_run *runs=(void *)1; size_t nr=77;
  fail_alloc=1;assert(sbk_hot_runs_prepare(a,n,&runs,&nr)==-ENOMEM);assert(!runs&&!nr);fail_alloc=0;
  assert(sbk_hot_runs_prepare(NULL,1,&runs,&nr)==-EINVAL);
  uint64_t order[2];size_t used=0;struct sbk_hot_range r={0x1000,2,order,&used};
  struct sbk_hot_run bad={1,1,1};assert(sbk_hot_index_append_runs(&r,1,a,2,&bad,1)==-EINVAL);assert(!used);
  bad=(struct sbk_hot_run){0,3,1};assert(sbk_hot_index_append_runs(&r,1,a,2,&bad,1)==-EINVAL);assert(!used);
  bad=(struct sbk_hot_run){0,1,1};assert(sbk_hot_index_append_runs(&r,1,a,2,&bad,1)==-EINVAL);assert(!used);
  puts("PASS exact heat order, ascending/descending, changed final ranges, holes, empty plans, random fallback, capacity fallback and allocation failure");
  if(argc>1)benchmark();
  return 0;
}
