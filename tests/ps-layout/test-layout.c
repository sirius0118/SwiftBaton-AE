#define _GNU_SOURCE
#include "sb-kernel-layout.h"
#include "sb-kernel-layout-wire.h"
#include <assert.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
static unsigned char wire[200000];static size_t cursor,limit;static unsigned calls;
static int transfer(int fd,void *p,size_t n,int send){(void)fd;calls++;if(n>limit-cursor)return -1;if(send)memcpy(wire+cursor,p,n);else memcpy(p,wire+cursor,n);cursor+=n;return 0;}
static void test_wire(void){
 for(int d=0;d<2;d++)for(int a=0;a<2;a++){
  struct sbk_rdma_endpoint e={0};e.reserved[0]=d;e.reserved[1]=a;
  assert(sbk_layout_mode_matches(d,a,&e));assert(!sbk_layout_mode_matches(!d,a,&e));assert(!sbk_layout_mode_matches(d,!a,&e));
  for(unsigned v=0;v<10;v++){int expected=a?v==(unsigned)(d?7:6):d?v==5:v>=2&&v<=4;assert(sbk_layout_ps_version_valid(d,a,v)==expected);}
 }
 struct sbk_catalog_layout l[2]={{123,0,0x100000,8},{124,0,0x200000,32}},*p=l,*out=NULL;unsigned n=2,nr=0;
 cursor=0;limit=sizeof(wire);assert(!sbk_layout_wire_transfer(0,true,&p,&n,transfer));size_t bytes=cursor;
 cursor=0;limit=bytes;assert(!sbk_layout_wire_transfer(0,false,&out,&nr,transfer));assert(nr==2&&!memcmp(out,l,sizeof(l)));free(out);out=NULL;
 for(size_t cut=0;cut<bytes;cut++){cursor=0;limit=cut;nr=99;assert(sbk_layout_wire_transfer(0,false,&out,&nr,transfer)==-EIO);assert(!out&&nr==99);}
 cursor=0;limit=sizeof(wire);((uint32_t*)wire)[0]=SBK_MAX_REGIONS/2+1;calls=0;assert(sbk_layout_wire_transfer(0,false,&out,&nr,transfer)==-EPROTO&&calls==1);
 ((uint32_t*)wire)[0]=0;((uint32_t*)wire)[1]=1;cursor=0;assert(sbk_layout_wire_transfer(0,false,&out,&nr,transfer)==-EPROTO);
 ((uint32_t*)wire)[1]=0;cursor=0;assert(!sbk_layout_wire_transfer(0,false,&out,&nr,transfer)&&!nr&&!out);
 puts("SBK_LAYOUT_WIRE_PASS legacy_modes truncation oversized reserved empty roundtrip");
}
static void test_collect(void){
 int cmd[2],reply[2];assert(!pipe(cmd)&&!pipe(reply));pid_t child=fork();assert(child>=0);
 if(!child){
  close(cmd[1]);close(reply[0]);
  char *all=mmap(NULL,66*4096,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(all!=MAP_FAILED);char*p=all+4096;
  assert(!mprotect(p,64*4096,PROT_READ|PROT_WRITE));for(unsigned i=0;i<64;i++)if(i<16||i>=48)p[i*4096]=(char)i;
  uint64_t a=(uintptr_t)p;assert(write(reply[1],&a,8)==8);
  char c;assert(read(cmd[0],&c,1)==1);assert(!mprotect(p+8*4096,4096,PROT_NONE));assert(write(reply[1],"x",1)==1);
  assert(read(cmd[0],&c,1)==1);_exit(0);
 }
 close(cmd[0]);close(reply[1]);uint64_t address;assert(read(reply[0],&address,8)==8);uint32_t pids[2]={(uint32_t)child,(uint32_t)child};
 for(unsigned round=0;round<2;round++){
  struct sbk_catalog_layout *l=NULL;struct sbk_layout_stats s;unsigned count=0;
  assert(!sbk_layout_collect(pids,2,8,&l,&count,&s));assert(count&&s.pids==1&&!s.skipped_pids);
  unsigned char seen[64]={0};
  for(unsigned i=0;i<count;i++){
   assert(l[i].source_pid==(uint32_t)child&&l[i].pages<=8&&!l[i].reserved);
   uint64_t end=l[i].address+l[i].pages*4096;
   if(end<=address||l[i].address>=address+64*4096)continue;
   assert(l[i].address>=address&&end<=address+64*4096);
   if(round)assert(!(l[i].address<address+9*4096&&end>address+8*4096)|| (l[i].address==address+8*4096&&l[i].pages==1));
   for(uint64_t j=l[i].address;j<end;j+=4096)assert(!seen[(j-address)/4096]++);
  }
  for(unsigned i=0;i<64;i++)assert(seen[i]==(i<16||i>=48));
  free(l);
  if(!round){assert(write(cmd[1],"x",1)==1);char c;assert(read(reply[0],&c,1)==1);}
 }
 assert(write(cmd[1],"x",1)==1);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));close(cmd[1]);close(reply[0]);
 struct sbk_catalog_layout *l=NULL;struct sbk_layout_stats s;unsigned n=1;uint32_t invalid=0;
 assert(sbk_layout_collect(&invalid,1,8,&l,&n,&s)==-EINVAL&&!l&&!n);
 assert(!sbk_layout_collect(NULL,0,8,&l,&n,&s)&&!n);free(l);
 uint32_t missing=INT32_MAX;assert(!sbk_layout_collect(&missing,1,8,&l,&n,&s)&&!n&&s.skipped_pids==1);free(l);
 puts("SBK_LAYOUT_COLLECT_PASS running_child resident_coverage holes_unfaulted mprotect_split duplicate_pid bounds unavailable_pid");
}
int main(void){alarm(30);test_wire();test_collect();return 0;}
