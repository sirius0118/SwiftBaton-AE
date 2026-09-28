/* Actual catalog implementation with deterministic CONFIG barriers and failures.
 * This tests ownership/concurrency, not RDMA performance. */
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
static unsigned ids[8192],entered,active,peak,finished,imports;
static unsigned seal_entered,seal_active,seal_peak,seal_finished;
static unsigned import_entered,import_active,import_peak,import_finished;
static unsigned audits, bad_state;
static int proceed,seal_proceed,import_proceed,fail_config,fail_watch,fail_seal,fail_import,fail_create_after=-1,created;
int __real_open(const char *,int,...);
int __real_pthread_create(pthread_t *,const pthread_attr_t *,void *(*)(void *),void *);
int __wrap_open(const char *path,int flags,...) {
 if(!strcmp(path,"/dev/swiftbaton_k"))return __real_open("/dev/null",flags);
 assert(!(flags&O_CREAT));return __real_open(path,flags);
}
int __wrap_pthread_create(pthread_t *t,const pthread_attr_t *a,void *(*f)(void *),void *v) {
 pthread_mutex_lock(&gate);int fail=fail_create_after>=0 && created>=fail_create_after;if(!fail)created++;pthread_mutex_unlock(&gate);
 return fail?EAGAIN:__real_pthread_create(t,a,f,v);
}
int __wrap_ioctl(int fd,unsigned long cmd,...) {
 assert(fd>=0 && fd<8192);va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
 if(cmd==SBK_IOC_CAPABILITIES){*(struct sbk_capabilities *)arg=(struct sbk_capabilities){.version=SBK_ABI_VERSION,.features=SBK_FEATURE_PARALLEL_PS|SBK_FEATURE_PS_SLICE};return 0;}
 if(cmd==SBK_IOC_STATS){*(struct sbk_stats *)arg=(struct sbk_stats){.pages=8,.completed=7,.fetched={0,0,7}};return 0;}
 if(cmd==SBK_IOC_DRAIN_STATUS){*(struct sbk_drain_status *)arg=(struct sbk_drain_status){.pages=8,.retired_tokens=8,.armed=1,.drained=1};return 0;}
 if(cmd==SBK_IOC_PAGE){struct sbk_page_info *p=arg;p->state=p->index?6:(bad_state?3:0);return 0;}
 pthread_mutex_lock(&gate);unsigned id=ids[fd];
 if(cmd==SBK_IOC_BIND_REGION)ids[fd]=((struct sbk_region_bind *)arg)->remote.id;
 else if(cmd==SBK_IOC_CONFIG) {
  entered++;active++;if(active>peak)peak=active;pthread_cond_broadcast(&changed);
  while(!proceed)pthread_cond_wait(&changed,&gate);
  active--;finished++;int fail=fail_config && id==3;pthread_mutex_unlock(&gate);
  if(fail){errno=EIO;return -1;}return 0;
 } else if(cmd==SBK_IOC_WATCH_DRAIN){if(fail_watch && id==5){pthread_mutex_unlock(&gate);errno=ENOSPC;return -1;}}
 else if(cmd==SBK_IOC_IMPORT_PS){
  struct sbk_ps_slice *s=arg;assert(s->source_fd!=fd && s->pages==8);imports++;
  import_entered++;import_active++;if(import_active>import_peak)import_peak=import_active;
  pthread_cond_broadcast(&changed);
  while(!import_proceed)pthread_cond_wait(&changed,&gate);
  import_active--;import_finished++;int fail=fail_import && id==3;
  pthread_mutex_unlock(&gate);if(fail){errno=EIO;return -1;}return 0;
 }
 else if(cmd==SBK_IOC_SEAL_REGION){
  seal_entered++;seal_active++;if(seal_active>seal_peak)seal_peak=seal_active;
  pthread_cond_broadcast(&changed);
  while(!seal_proceed)pthread_cond_wait(&changed,&gate);
  seal_active--;seal_finished++;int fail=fail_seal && id==3;
  pthread_mutex_unlock(&gate);if(fail){errno=EIO;return -1;}return 0;
 }
 else assert(cmd==SBK_IOC_PRETRANSFER_MANY);
 pthread_mutex_unlock(&gate);return 0;
}
static unsigned fd_count(void){DIR*d=opendir("/proc/self/fd");assert(d);unsigned n=0;struct dirent*e;while((e=readdir(d)))if(e->d_name[0]!='.')n++;closedir(d);return n;}
struct job{struct sbk_catalog*c;struct sbk_catalog_final f[8];int ret;};
static void *seal(void *p){struct job*j=p;j->ret=sbk_catalog_seal(j->c,j->f,8);return NULL;}
static void audit(const struct sbk_catalog_audit *a) {
 assert(a->stats.completed==7 && a->states[0]==1 && a->states[6]==7 && a->drain.drained);
 audits++;
}
static void test(int mode) {
 unsigned before=fd_count();int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(fd>=0);
 struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA};
 struct job j={.c=sbk_catalog_create(fd,&cfg)};assert(j.c);close(fd);
 assert(sbk_catalog_prepare_workers(j.c,0)==-EINVAL);assert(sbk_catalog_prepare_workers(j.c,33)==-EINVAL);
 assert(!sbk_catalog_prepare_workers(j.c,4));
 entered=active=peak=finished=imports=created=0;proceed=seal_proceed=import_proceed=1;fail_config=fail_watch=fail_seal=fail_import=0;fail_create_after=-1;
 if(mode==5||mode==7)for(unsigned i=0;i<2;i++){
  struct sbk_catalog_record r={.source_pid=10,.address=0x100000+i*32*4096,.remote={.address=0x800000+i*32*4096,.pages=32,.id=100+i,.rkey=7}};
  uint64_t indices[]={1,3,7};assert(!sbk_catalog_stage(j.c,&r,indices,3));
 }
 for(unsigned i=0;i<8;i++)j.f[i].record=(struct sbk_catalog_record){.source_pid=10,.restore_pid=20,.address=0x100000+i*8*4096,.remote={.address=0x400000+i*8*4096,.pages=8,.id=i+1,.rkey=9}};
 entered=active=peak=finished=seal_entered=seal_active=seal_peak=seal_finished=0;
 import_entered=import_active=import_peak=import_finished=0;
 proceed=seal_proceed=0;import_proceed=mode!=5&&mode!=7;
 fail_config=mode==1;fail_watch=mode==4;fail_seal=mode==6;fail_import=mode==7;
 if(mode==2)fail_create_after=0;
 if(mode==3)fail_create_after=1;
 unsigned target=mode==2?1:mode==3?2:4;
 pthread_t t;assert(!__real_pthread_create(&t,NULL,seal,&j));
 pthread_mutex_lock(&gate);while(entered<target)pthread_cond_wait(&changed,&gate);
 assert(active==target && peak==target);proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
 if(mode==5||mode==7){
  pthread_mutex_lock(&gate);
  while(import_entered<4)pthread_cond_wait(&changed,&gate);
  assert(import_active==4 && import_peak==4);
  import_proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
 }
 if(mode!=1 && mode!=4 && mode!=7){
  unsigned seal_target=mode==2||mode==3?1:4;
  pthread_mutex_lock(&gate);
  while(seal_entered<seal_target)pthread_cond_wait(&changed,&gate);
  assert(seal_active==seal_target && seal_peak==seal_target);
  seal_proceed=1;pthread_cond_broadcast(&changed);pthread_mutex_unlock(&gate);
 }
 assert(!pthread_join(t,NULL));assert(!active && entered==finished && peak==target);
 assert(j.ret==(mode==1||mode==6||mode==7?-EIO:mode==4?-ENOSPC:0));
 if(mode!=1 && mode!=4 && mode!=7)assert(!seal_active && seal_entered==seal_finished);
 if(mode==5||mode==7)assert(!import_active && import_entered==import_finished);
 assert(sbk_catalog_prepare_workers(j.c,4)==-EINVAL);
 if(mode==5)assert(imports==8);
 if(!j.ret){uint64_t unfetched=0;audits=bad_state=0;assert(!sbk_catalog_audit_deficits(j.c,audit,&unfetched));assert(audits==8&&unfetched==8);
  bad_state=1;assert(sbk_catalog_audit_deficits(j.c,audit,&unfetched)==-EPROTO);bad_state=0;}
 if(j.ret)assert(sbk_catalog_seal(j.c,j.f,8)==-EINVAL);
 sbk_catalog_destroy(j.c);assert(fd_count()==before);
}
int main(void){alarm(90);for(int round=0;round<12;round++)for(int mode=0;mode<8;mode++)test(mode);puts("SBK_FINAL_PREPARE_PASS concurrency4 config_failure watch_failure seal_failure import_failure thread_fallback partial_thread_fallback PS_slices joined FD_leaks0 IOCTL_stub");return 0;}
