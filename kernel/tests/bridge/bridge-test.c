// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL %d errno=%d\n",__LINE__,errno);exit(1);}}while(0)
struct request{uint64_t address;uint32_t pages,mode;};
int main(void){
 int fd=open("/dev/sbk_bridge_test",O_RDWR);CHECK(fd>=0);uint64_t expected=0;
 for(int repeat=0;repeat<12;repeat++)for(int mode=0;mode<4;mode++){
  size_t pages=513,size=pages*4096;unsigned char *p=mmap(NULL,size+4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
  CHECK(!madvise(p,size+4096,MADV_NOHUGEPAGE));unsigned char *q=p+4096;
  /* An already materialized final PTE must leave ALL earlier PTEs unchanged. */
  if(mode==3)q[size-4096]=0x7b;
  struct request r={(uintptr_t)q,pages,mode};int ret=ioctl(fd,_IOW('Z',0x51,struct request),&r);
  if(mode==0)CHECK(ret==0);else CHECK(ret==-1 && errno==(mode==2?ENOENT:EEXIST));
  if(mode){CHECK(q[0]==0 && q[4096]==0);CHECK(q[size-4096]==(mode==3?0x7b:0));}
  CHECK(!munmap(p,size+4096));expected+=pages;
  uint64_t stats[2];CHECK(!ioctl(fd,_IOR('Z',0x52,uint64_t[2]),stats));if(stats[0]!=expected || stats[1])fprintf(stderr,"mode=%d repeat=%d released=%llu expected=%llu unexpected=%llu\n",mode,repeat,(unsigned long long)stats[0],(unsigned long long)expected,(unsigned long long)stats[1]);CHECK(stats[0]==expected && stats[1]==0);
 }
 CHECK(!close(fd));puts("SBK_BRIDGE_TEST_PASS successful_unmap duplicate_token_partial_rollback missing_token late_PTE_conflict references_balanced");return 0;
}
