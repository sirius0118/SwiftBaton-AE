// SPDX-License-Identifier: GPL-2.0
/* Anonymous marker setup microbenchmark. Never represents RDMA/migration latency. */
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <time.h>
#include "sbk_uapi.h"
#define P 4096UL
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"FAIL line=%d errno=%d\n",__LINE__,errno); exit(1); } } while(0)
static uint64_t now(void) {struct timespec t;CHECK(!clock_gettime(CLOCK_MONOTONIC,&t));return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;}
int main(int argc,char **argv) {
 setvbuf(stdout,NULL,_IONBF,0);
 size_t n=argc>1?strtoul(argv[1],NULL,10):65536;CHECK(n && n<=1UL<<20);
 for(int repeat=0;repeat<3;repeat++) {
  int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
  unsigned char *src=mmap(NULL,n*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  unsigned char *dst=mmap(NULL,n*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
  CHECK(src!=MAP_FAILED && dst!=MAP_FAILED);
  CHECK(!madvise(src,n*P,MADV_NOHUGEPAGE));CHECK(!madvise(dst,n*P,MADV_NOHUGEPAGE));
  for(size_t i=0;i<n;i++)src[i*P]=(i*37+19)%251;
  struct sbk_config c={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_LOOPBACK_TEST,.pages=n,
   .source_address=(uintptr_t)src,.prefetch_workers=1,.background_workers=1,.batch_pages=8,.test_fail_page=SBK_NO_FAILURE};
  uint64_t a=now();CHECK(!ioctl(fd,SBK_IOC_CONFIG,&c));uint64_t b=now();
  uint64_t pages=n;CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&pages));uint64_t d=now();
  struct sbk_anon_arm arm={.address=(uintptr_t)dst};CHECK(!ioctl(fd,SBK_IOC_ARM_ANON,&arm));uint64_t e=now();
  struct sbk_token_pool_stats ps;CHECK(!ioctl(fd,SBK_IOC_TOKEN_POOL_STATS,&ps));
  CHECK(ps.claimed==n && !ps.fallback && !ps.available);
  for(size_t i=0;i<n;i+=997)CHECK(dst[i*P]==(i*37+19)%251);
  CHECK(dst[(n-1)*P]==((n-1)*37+19)%251);
  CHECK(!munmap(dst,n*P));struct sbk_drain_status drain={0};
  for(int j=0;j<10000;j++){CHECK(!ioctl(fd,SBK_IOC_DRAIN_STATUS,&drain));if(drain.drained)break;usleep(1000);}
  CHECK(drain.drained && drain.retired_tokens==n);
  struct sbk_stats st;CHECK(!ioctl(fd,SBK_IOC_STATS,&st) && !st.errors);
  CHECK(!close(fd) && !munmap(src,n*P));
  printf("SBK_ARM_BENCH repeat=%d pages=%zu config_ns=%llu reserve_ns=%llu arm_ns=%llu\n",repeat,n,
   (unsigned long long)(b-a),(unsigned long long)(d-b),(unsigned long long)(e-d));
  usleep(100000);
 }
 puts("SBK_ARM_BENCH_PASS");return 0;
}
