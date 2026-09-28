/* Concurrent calls into the actual catalog implementation. Device ioctls are
 * held at a deterministic barrier; tests locks/lifetimes, not RDMA performance. */
#define _GNU_SOURCE
#include "sb-kernel-catalog.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static pthread_mutex_t gate=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static unsigned ids[8192],entered;
static int proceed, fail_stage;
int __real_open(const char *,int,...);
int __wrap_open(const char *path,int flags,...) {
  if(!strcmp(path,"/dev/swiftbaton_k"))return __real_open("/dev/null",flags);
  assert(!(flags&O_CREAT));return __real_open(path,flags);
}
int __wrap_ioctl(int fd,unsigned long cmd,...) {
  assert(fd>=0&&fd<8192);
  va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
  if(cmd==SBK_IOC_CAPABILITIES) {
    *(struct sbk_capabilities *)arg=(struct sbk_capabilities){.version=SBK_ABI_VERSION,.features=SBK_FEATURE_PARALLEL_PS};
  } else if(cmd==SBK_IOC_BIND_REGION) ids[fd]=((struct sbk_region_bind *)arg)->remote.id;
  else if(cmd==SBK_IOC_PRETRANSFER_MANY) {
    struct sbk_hot_list *list=arg;assert(list->count==3);
    pthread_mutex_lock(&gate);entered++;pthread_cond_broadcast(&changed);
    while(!proceed)pthread_cond_wait(&changed,&gate);
    pthread_mutex_unlock(&gate);
    if(fail_stage&&ids[fd]==2){errno=EIO;return -1;}
  } else assert(cmd==SBK_IOC_CONFIG||cmd==SBK_IOC_WATCH_DRAIN||cmd==SBK_IOC_SEAL_REGION);
  return 0;
}
static unsigned fd_count(void) {
  DIR *d=opendir("/proc/self/fd");assert(d);unsigned n=0;struct dirent *e;
  while((e=readdir(d)))if(e->d_name[0]!='.')n++;
  closedir(d);return n;
}
struct argument {struct sbk_catalog *catalog;struct sbk_catalog_record record;int result;};
static void *stage(void *arg) {
  struct argument *a=arg;uint64_t indices[]={1,3,7};
  a->result=sbk_catalog_stage(a->catalog,&a->record,indices,3);return NULL;
}
static void exercise(int failure) {
  unsigned before=fd_count();int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(fd>=0);
  struct sbk_config config={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA};
  struct sbk_catalog *c=sbk_catalog_create(fd,&config);assert(c);close(fd);
  entered=0;proceed=0;fail_stage=failure;
  pthread_t threads[6];struct argument args[6];
  for(unsigned i=0;i<6;i++) {
    unsigned region=i%4;
    args[i]=(struct argument){.catalog=c,.record={.source_pid=10,.address=0x100000+region*65536,
      .remote={.address=0x200000+region*65536,.pages=16,.id=region+1,.rkey=100+region}}};
    assert(!pthread_create(&threads[i],NULL,stage,&args[i]));
  }
  pthread_mutex_lock(&gate);
  while(entered!=6)pthread_cond_wait(&changed,&gate);
  proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
  for(unsigned i=0;i<6;i++) {
    assert(!pthread_join(threads[i],NULL));
    assert(args[i].result==(failure&&i%4==1?-EIO:0));
  }
  struct sbk_catalog_final final[4]={0};
  for(unsigned i=0;i<4;i++) {final[i].record=args[i].record;final[i].record.restore_pid=20;}
  assert(sbk_catalog_seal(c,final,4)==(failure?-EINVAL:0));
  sbk_catalog_destroy(c);assert(fd_count()==before);
}
int main(void) {
  alarm(90);
  for(unsigned i=0;i<30;i++){exercise(0);exercise(1);}
  puts("SBK_CATALOG_STAGE_PASS rounds=60 concurrent_calls=6 regions=4 replay=1 error_rejects_final=1 fd_leaks=0 IOCTL=stub");
  return 0;
}
