/* SPDX-License-Identifier: GPL-2.0 */
/* Deterministic anonymous-marker discard accounting; no RDMA timing claim. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include "sbk_uapi.h"
#define CHECK(x) do { if (!(x)) {fprintf(stderr,"FAIL line=%d expr=%s errno=%d\n",__LINE__,#x,errno);exit(1);} } while(0)
#define P 4096UL
int main(void)
{
  const size_t pages=64,touched=16;
  int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
  struct sbk_capabilities cap;CHECK(!ioctl(fd,SBK_IOC_CAPABILITIES,&cap));CHECK(cap.features&SBK_FEATURE_ANONYMOUS_PTE);
  unsigned char *src=mmap(NULL,pages*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  unsigned char *dst=mmap(NULL,pages*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(src!=MAP_FAILED&&dst!=MAP_FAILED);
  for(size_t i=0;i<pages*P;i++)src[i]=(i*37+i/P+13)%251;
  struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_LOOPBACK_TEST,
    .source_address=(uintptr_t)src,.pages=pages,.prefetch_workers=1,.background_workers=1,
    .batch_pages=1,.prefetch_enabled=0,.test_fail_page=SBK_NO_FAILURE};
  CHECK(!ioctl(fd,SBK_IOC_CONFIG,&cfg));
  uint64_t reserve=pages;CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&reserve));
  struct sbk_anon_arm arm={.address=(uintptr_t)dst};CHECK(!ioctl(fd,SBK_IOC_ARM_ANON,&arm));
  CHECK(!memcmp(src,dst,touched*P));
  struct sbk_stats before;CHECK(!ioctl(fd,SBK_IOC_STATS,&before));
  CHECK(before.completed==touched&&before.fetched[SBK_DEMAND]==touched&&!before.errors);
  CHECK(!madvise(dst+touched*P,(pages-touched)*P,MADV_DONTNEED));
  for(size_t i=touched*P;i<pages*P;i++)CHECK(dst[i]==0);
  CHECK(!memcmp(src,dst,touched*P));
  struct sbk_drain_status drain={0};
  for(unsigned i=0;i<2000;i++) {CHECK(!ioctl(fd,SBK_IOC_DRAIN_STATUS,&drain));if(drain.drained)break;usleep(1000);}
  CHECK(drain.drained&&drain.retired_tokens==pages);
  struct sbk_stats after;CHECK(!ioctl(fd,SBK_IOC_STATS,&after));
  CHECK(after.completed==touched&&after.fetched[SBK_DEMAND]==touched&&!after.errors);
  CHECK(after.faults==before.faults&&!after.fetched[SBK_PREFETCH]&&!after.fetched[SBK_BACKGROUND]);
  size_t states[7]={0};
  for(size_t i=0;i<pages;i++) {
    struct sbk_page_info info={.index=i};CHECK(!ioctl(fd,SBK_IOC_PAGE,&info));CHECK(info.state<7);states[info.state]++;
    CHECK(i<touched?info.state==6:info.state==0); /* Current module ADOPTED / REMOTE. */
  }
  printf("{\"test\":\"untouched_marker_discard\",\"transport\":\"LOOPBACK_TEST\",\"pages\":64,\"touched\":16,\"discarded\":48,\"completed\":%llu,\"PF\":%llu,\"retired_tokens\":%llu,\"drained\":%u,\"state_remote\":%zu,\"state_adopted\":%zu,\"original_bytes_verified\":true,\"discarded_bytes_zero\":true,\"result\":\"PASS\"}\n",
   (unsigned long long)after.completed,(unsigned long long)after.fetched[SBK_DEMAND],(unsigned long long)drain.retired_tokens,drain.drained,states[0],states[6]);
  CHECK(!munmap(dst,pages*P));CHECK(!close(fd));CHECK(!munmap(src,pages*P));
  return 0;
}
