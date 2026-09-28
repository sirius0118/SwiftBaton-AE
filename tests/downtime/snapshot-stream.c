/* Actual /proc epoch/PFN tracking and process_vm_readv against our own child.
 * Only EXPORT_REGION is replaced: validates immutable bytes without loading a
 * module. Force an unmap after PFN capture but before reads to test skip frames. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "sb-kernel-precopy.h"
#include "sb-kernel-sparse.h"
#include "cr_options.h"
#include "pre-transfer.h"
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>
#define P 4096UL
#define REGIONS 5
#define PAGES 256
struct options opts;
volatile struct pid_data_list *pid_data_list;
int list_length=1;
static uintptr_t addresses[REGIONS];
static int command_fd, reply_fd;
static atomic_int unmapping, exports;
static int fail_export, fragment_case;
static unsigned char pattern(unsigned region,size_t offset) { return (region*31+offset*7)%251; }
static void exact(int fd,void *p,size_t n,int output) {
  size_t done=0;
  while(done<n) {ssize_t rc=output?write(fd,(char *)p+done,n-done):read(fd,(char *)p+done,n-done);assert(rc>0);done+=rc;}
}
ssize_t __real_process_vm_readv(pid_t,const struct iovec *,unsigned long,const struct iovec *,unsigned long,unsigned long);
ssize_t __wrap_process_vm_readv(pid_t pid,const struct iovec *l,unsigned long ln,const struct iovec *r,unsigned long rn,unsigned long flags) {
  uintptr_t address=(uintptr_t)r[0].iov_base;
  if(address>=addresses[2]&&address<addresses[2]+PAGES*P) {
    int expected=0;
    if(atomic_compare_exchange_strong(&unmapping,&expected,1)) {
      char c='U';exact(command_fd,&c,1,1);exact(reply_fd,&c,1,0);assert(c=='U');
      atomic_store(&unmapping,2);
    } else while(atomic_load(&unmapping)!=2) sched_yield();
  }
  return __real_process_vm_readv(pid,l,ln,r,rn,flags);
}
int __wrap_ioctl(int fd,unsigned long cmd,...) {
  assert(fd==12345&&cmd==SBK_IOC_EXPORT_REGION);
  va_list ap;va_start(ap,cmd);struct sbk_rdma_region *r=va_arg(ap,struct sbk_rdma_region *);va_end(ap);
  if(fragment_case) {
    assert(!r->id&&!r->rkey&&(r->pages==1||r->pages==SBK_MAX_BATCH+2));
    unsigned char *data=(void *)(uintptr_t)r->address;
    for(size_t page=0;page<r->pages;page++)for(size_t i=0;i<P;i++)
      assert(data[page*P+i]==((!page||page==r->pages-1)?0x6b:0));
    r->id=atomic_fetch_add(&exports,1)+1;r->rkey=1000+r->id;return 0;
  }
  assert(r->pages==PAGES&&!r->id&&!r->rkey);
  unsigned char *p=(void *)(uintptr_t)r->address;
  int found=-1;
  for(unsigned k=0;k<REGIONS;k++)if(k!=2&&k!=4&&p[0]==pattern(k,0))found=k;
  assert(found>=0);
  for(size_t i=0;i<PAGES*P;i++)assert(p[i]==pattern(found,i));
  if(fail_export && found==3) {errno=EIO;return -1;}
  r->id=atomic_fetch_add(&exports,1)+1;r->rkey=1000+r->id;
  return 0;
}
static void exercise(unsigned workers,int failure) {
  fail_export=failure;fragment_case=0;
  int to_child[2],from_child[2];assert(!pipe(to_child)&&!pipe(from_child));
  atomic_store(&unmapping,0);atomic_store(&exports,0);
  pid_t child=fork();assert(child>=0);
  if(!child) {
    close(to_child[1]);close(from_child[0]);
    for(unsigned k=0;k<REGIONS;k++) {
      unsigned char *guard=mmap(NULL,(PAGES+2)*P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
      unsigned char *p=guard+P;assert(!mprotect(p,PAGES*P,PROT_READ|PROT_WRITE));addresses[k]=(uintptr_t)p;
      if(k!=4)for(size_t i=0;i<PAGES*P;i++)p[i]=pattern(k,i);
    }
    exact(from_child[1],addresses,sizeof(addresses),1);
    for(;;) {
      char c;exact(to_child[0],&c,1,0);
      if(c=='U')assert(!munmap((void *)addresses[2],PAGES*P));
      else if(c=='M')*((volatile unsigned char *)addresses[0]+17*P)^=0xff;
      else if(c=='Q')_exit(0);
      else assert(0);
      exact(from_child[1],&c,1,1);
    }
  }
  close(to_child[0]);close(from_child[1]);command_fd=to_child[1];reply_fd=from_child[0];
  exact(reply_fd,addresses,sizeof(addresses),0);
  unsigned long *heat=calloc(REGIONS*PAGES,sizeof(*heat));assert(heat);
  for(unsigned k=0;k<REGIONS;k++)for(unsigned j=0;j<PAGES;j++)heat[k*PAGES+j]=addresses[k]+j*P;
  struct pid_data_list sampled={child,REGIONS*PAGES,heat};pid_data_list=&sampled;
  opts=(struct options){workers,32,0};
  unsigned planned=0;int begin=sb_kernel_ps_begin(12345,&planned);
  if(failure) {
    int result=begin;
    if(!result) {
      struct sbk_ps_region *p;
      do {result=sb_kernel_ps_next(&p);} while(result==1);
      sb_kernel_ps_finish(true);
    }
    assert(result==-EIO);
    sb_kernel_ps_destroy();
    char quit='Q';exact(command_fd,&quit,1,1);int status;
    assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    close(command_fd);close(reply_fd);free(heat);pid_data_list=NULL;
    printf("SBK_SNAPSHOT_FAILURE_PASS workers=%u register_error=EIO joined_before_destroy=1 MR=stub\n",workers);
    return;
  }
  assert(!begin&&planned==REGIONS);
  unsigned present=0,skipped=0;bool seen[REGIONS]={0};
  void *snapshots[REGIONS]={0};size_t snapshot_pages[REGIONS]={0};
  for(unsigned i=0;i<planned;i++) {
    struct sbk_ps_region *p;assert(sb_kernel_ps_next(&p)==1);
    unsigned k=0;while(k<REGIONS&&p->record.address!=addresses[k])k++;
    assert(k<REGIONS&&!seen[k]);seen[k]=true;
    snapshots[k]=p->snapshot;snapshot_pages[k]=p->record.remote.pages;
    if(k==2||k==4) {assert(!p->count&&!p->record.remote.id);skipped++;}
    else {assert(p->count==PAGES&&p->record.remote.id);present++;}
  }
  struct sbk_ps_region *none=NULL;assert(!sb_kernel_ps_next(&none));
  assert(!sb_kernel_ps_finish(false));
  assert(present==3&&skipped==2&&atomic_load(&exports)==3);
  pid_t helper=fork();assert(helper>=0);
  if(!helper) {
    unsigned char resident[PAGES];
    for(unsigned k=0;k<REGIONS;k++) {
      assert(snapshot_pages[k]<=PAGES);
      errno=0;assert(mincore(snapshots[k],snapshot_pages[k]*P,resident)==-1&&errno==ENOMEM);
    }
    _exit(0);
  }
  int helper_status;assert(waitpid(helper,&helper_status,0)==helper&&WIFEXITED(helper_status)&&!WEXITSTATUS(helper_status));
  for(unsigned k=0;k<REGIONS;k++) {
    unsigned char resident[PAGES];assert(!mincore(snapshots[k],snapshot_pages[k]*P,resident));
    if(k!=2&&k!=4)for(size_t i=0;i<PAGES*P;i++)assert(((unsigned char *)snapshots[k])[i]==pattern(k,i));
  }
  printf("SBK_SNAPSHOT_NOINHERIT_PASS workers=%u helper_absent_regions=%u parent_bytes_intact=1 MR=stub\n",workers,REGIONS);
  char c='M';exact(command_fd,&c,1,1);exact(reply_fd,&c,1,0);assert(c=='M');
  struct sbk_rdma_region r={.address=addresses[0],.pages=PAGES};uint64_t *dirty=NULL;size_t count=0;
  assert(!sb_kernel_ps_validate(child,&r,&dirty,&count)&&count==1&&dirty[0]==17);free(dirty);
  assert(!kill(child,SIGSTOP));int stopped;assert(waitpid(child,&stopped,WUNTRACED)==child&&WIFSTOPPED(stopped));
  for(unsigned parallel=1;parallel<=32;parallel*=2) {
    struct sbk_pm_snapshot *pm=NULL;assert(!sbk_pm_snapshot_create(child,&r,1,parallel,&pm)&&pm);
    const uint64_t *observed=sbk_pm_snapshot_find(pm,r.address,r.pages);assert(observed);
    assert(!sb_kernel_ps_validate_snapshot(child,&r,observed,&dirty,&count)&&count==1&&dirty[0]==17);free(dirty);
    uint64_t *changed=malloc(PAGES*sizeof(uint64_t));assert(changed);memcpy(changed,observed,PAGES*sizeof(uint64_t));
    changed[24]=(changed[24]&~(1ULL<<55))^1ULL;
    assert(!sb_kernel_ps_validate_snapshot(child,&r,changed,&dirty,&count)&&count==2&&dirty[0]==17&&dirty[1]==24);free(dirty);
    memcpy(changed,observed,PAGES*sizeof(uint64_t));changed[25]&=~(1ULL<<63);
    assert(!sb_kernel_ps_validate_snapshot(child,&r,changed,&dirty,&count)&&count==2&&dirty[0]==17&&dirty[1]==25);free(dirty);
    memcpy(changed,observed,PAGES*sizeof(uint64_t));changed[26]|=1ULL<<62;
    assert(!sb_kernel_ps_validate_snapshot(child,&r,changed,&dirty,&count)&&count==2&&dirty[0]==17&&dirty[1]==26);free(dirty);
    free(changed);sbk_pm_snapshot_free(pm);
  }
  assert(!kill(child,SIGCONT));
  sb_kernel_ps_destroy();
  c='Q';exact(command_fd,&c,1,1);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  close(command_fd);close(reply_fd);free(heat);pid_data_list=NULL;
  printf("SBK_SNAPSHOT_STREAM_PASS workers=%u planned=5 snapshots=3 empty=1 raced_unmap=1 final_dirty=1 source_running=1 MR=stub\n",workers);
}
static void exercise_budget(unsigned workers, unsigned islands, unsigned budget_mb) {
  enum { VIRTUAL_PAGES=16384 };
  int to_child[2],from_child[2];assert(!pipe(to_child)&&!pipe(from_child));
  fragment_case=0;fail_export=0;atomic_store(&exports,0);
  pid_t child=fork();assert(child>=0);
  if(!child) {
    close(to_child[1]);close(from_child[0]);
    unsigned char *guard=mmap(NULL,(VIRTUAL_PAGES+2)*P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
    unsigned char *data=guard+P;assert(!mprotect(data,VIRTUAL_PAGES*P,PROT_READ|PROT_WRITE));
    for(unsigned k=0;k<islands;k++)
      for(size_t i=0;i<PAGES*P;i++)data[k*(VIRTUAL_PAGES-PAGES)*P+i]=pattern(k,i);
    uintptr_t address=(uintptr_t)data;exact(from_child[1],&address,sizeof(address),1);
    char c;exact(to_child[0],&c,1,0);assert(c=='C');
    unsigned char *resident=malloc(VIRTUAL_PAGES);assert(resident&&!mincore(data,VIRTUAL_PAGES*P,resident));
    unsigned count=0;for(unsigned i=0;i<VIRTUAL_PAGES;i++)count+=!!(resident[i]&1);
    exact(from_child[1],&count,sizeof(count),1);
    exact(to_child[0],&c,1,0);assert(c=='Q');_exit(0);
  }
  close(to_child[0]);close(from_child[1]);command_fd=to_child[1];reply_fd=from_child[0];
  exact(reply_fd,&addresses[0],sizeof(addresses[0]),0);
  /* No forced-unmap region in this source. */
  addresses[2]=0;
  /* Reverse order, duplicates, unaligned samples and a PROT_NONE guard entry.
   * A tight budget must reach both distant islands without pinning the gap. */
  unsigned long heat[PAGES*2*2+2];unsigned n=0;
  for(unsigned k=islands;k>0;k--)for(unsigned i=PAGES;i>0;i--) {
    unsigned long address=addresses[0]+((k-1)*(VIRTUAL_PAGES-PAGES)+i-1)*P;
    heat[n++]=address;heat[n++]=address;
  }
  heat[n++]=addresses[0]+1;heat[n++]=addresses[0]-P;
  struct pid_data_list sampled={child,n,heat};pid_data_list=&sampled;opts=(struct options){workers,budget_mb,0};
  unsigned planned=0;assert(!sb_kernel_ps_begin(12345,&planned)&&planned==islands);
  bool seen[2]={0};
  for(unsigned i=0;i<planned;i++) {
    struct sbk_ps_region *p;assert(sb_kernel_ps_next(&p)==1&&p->count==PAGES&&p->record.remote.pages==PAGES);
    unsigned k=p->record.address==addresses[0]?0:1;
    assert(k<islands&&!seen[k]&&p->record.address==addresses[0]+k*(VIRTUAL_PAGES-PAGES)*P);seen[k]=true;
    for(unsigned j=0;j<PAGES;j++)assert(p->indices[j]==j);
  }
  struct sbk_ps_region *none=NULL;assert(!sb_kernel_ps_next(&none));
  assert(atomic_load(&exports)==(int)islands);
  assert(!sb_kernel_ps_finish(false));sb_kernel_ps_destroy();
  char c='C';exact(command_fd,&c,1,1);unsigned resident;exact(reply_fd,&resident,sizeof(resident),0);assert(resident==islands*PAGES);
  c='Q';exact(command_fd,&c,1,1);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  close(command_fd);close(reply_fd);pid_data_list=NULL;
  printf("SBK_SNAPSHOT_BUDGET_PASS workers=%u virtual_MB=64 budget_MB=%u islands=%u selected_pages=%u MR_pages=%u original_resident_pages=%u duplicates=1 reversed=1 guard_filtered=1 MR=stub\n",workers,budget_mb,islands,islands*PAGES,islands*PAGES,resident);
}
static void exercise_invalid(void) {
  struct pid_data_list sampled={getpid(),-1,NULL};pid_data_list=&sampled;opts=(struct options){4,2,0};
  unsigned planned=999;assert(sb_kernel_ps_begin(12345,&planned)==-EINVAL&&!planned);sb_kernel_ps_destroy();
  sampled.read_hot_num=1;assert(sb_kernel_ps_begin(12345,&planned)==-EINVAL&&!planned);sb_kernel_ps_destroy();
  sampled.read_hot_num=0;assert(!sb_kernel_ps_begin(12345,&planned)&&!planned);assert(!sb_kernel_ps_finish(false));sb_kernel_ps_destroy();
  pid_data_list=NULL;puts("SBK_SNAPSHOT_INVALID_PASS negative_count=1 missing_list=1 empty_list=1");
}
static void exercise_fragments(int capacity_case) {
  const unsigned count=capacity_case?SBK_MAX_REGIONS/2+1:3;
  unsigned long *offsets=malloc(count*sizeof(*offsets));assert(offsets);
  for(unsigned i=0;i<count;i++)offsets[i]=i*(SBK_MAX_BATCH+2UL);
  if(!capacity_case)offsets[1]=SBK_MAX_BATCH+1; /* 32-page gap retained, 34-page gap split. */
  const size_t virtual_pages=offsets[count-1]+1;
  int to_child[2],from_child[2];assert(!pipe(to_child)&&!pipe(from_child));
  fragment_case=1;fail_export=0;atomic_store(&exports,0);addresses[2]=0;
  pid_t child=fork();assert(child>=0);
  if(!child) {
    close(to_child[1]);close(from_child[0]);
    unsigned char *guard=mmap(NULL,(virtual_pages+2)*P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);assert(guard!=MAP_FAILED);
    unsigned char *data=guard+P;assert(!mprotect(data,virtual_pages*P,PROT_READ|PROT_WRITE));
    assert(!madvise(data,virtual_pages*P,MADV_NOHUGEPAGE));
    for(unsigned i=0;i<count;i++)memset(data+offsets[i]*P,0x6b,P);
    uintptr_t address=(uintptr_t)data;exact(from_child[1],&address,sizeof(address),1);
    char c;exact(to_child[0],&c,1,0);assert(c=='Q');_exit(0);
  }
  close(to_child[0]);close(from_child[1]);command_fd=to_child[1];reply_fd=from_child[0];
  uintptr_t address;exact(reply_fd,&address,sizeof(address),0);
  for(unsigned i=0;i<count;i++)offsets[i]=address+offsets[i]*P;
  struct pid_data_list sampled={child,count,offsets};pid_data_list=&sampled;opts=(struct options){4,32,0};
  unsigned planned=0;assert(!sb_kernel_ps_begin(12345,&planned));
  assert(planned==(capacity_case?SBK_MAX_REGIONS/2:2));
  unsigned pages=0,registered=0;unsigned char *seen=calloc(count,1);assert(seen);
  for(unsigned i=0;i<planned;i++) {
    struct sbk_ps_region *p;assert(sb_kernel_ps_next(&p)==1);
    assert(p->count==(capacity_case||p->record.address!=address?1:2));
    for(size_t j=0;j<p->count;j++) {
      uintptr_t a=p->record.address+p->indices[j]*P;
      unsigned k=0;while(k<count&&offsets[k]!=a)k++;
      assert(k<count&&!seen[k]);seen[k]=1;pages++;
    }
    registered+=p->record.remote.pages;
  }
  assert(pages==(capacity_case?count-1:count)&&registered==(capacity_case?count-1:SBK_MAX_BATCH+3));
  if(capacity_case)assert(!seen[count-1]);
  struct sbk_ps_region *none=NULL;assert(!sb_kernel_ps_next(&none));assert(!sb_kernel_ps_finish(false));
  assert(atomic_load(&exports)==(int)planned);sb_kernel_ps_destroy();
  char c='Q';exact(command_fd,&c,1,1);int status;assert(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
  close(command_fd);close(reply_fd);free(offsets);free(seen);pid_data_list=NULL;fragment_case=0;
  printf("SBK_SNAPSHOT_FRAGMENT_PASS capacity_case=%d selected=%u copied=%u regions=%u registered_pages=%u MR=stub\n",capacity_case,count,pages,planned,registered);
}
int main(void) {setvbuf(stdout,NULL,_IONBF,0);alarm(90);exercise(1,0);exercise(4,0);exercise(4,1);exercise_budget(1,1,2);exercise_budget(4,1,2);exercise_budget(1,2,2);exercise_budget(4,2,2);exercise_budget(1,2,64);exercise_budget(4,2,64);exercise_invalid();exercise_fragments(0);exercise_fragments(1);return 0;}
