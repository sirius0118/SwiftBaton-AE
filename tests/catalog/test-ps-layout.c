/* Actual catalog with device ioctls mocked: resource lifetime and final reuse. */
#define _GNU_SOURCE
#include "sb-kernel-catalog.h"
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
static unsigned features, configs, plans, imports, seals, binds, fail_plan;
static int prepared[16], sealed[16];
int __real_open(const char *,int,...);
int __wrap_open(const char *p,int f,...) {
 assert(!(f&O_CREAT)); return __real_open(!strcmp(p,"/dev/swiftbaton_k")?"/dev/null":p,f);
}
int __wrap_ioctl(int fd,unsigned long cmd,...) {
 va_list ap;va_start(ap,cmd);void *arg=va_arg(ap,void *);va_end(ap);
 assert(fd>=0);
 if(cmd==SBK_IOC_CAPABILITIES){*(struct sbk_capabilities *)arg=(struct sbk_capabilities){.version=SBK_ABI_VERSION,.features=features};return 0;}
 if(cmd==SBK_IOC_BIND_UNBOUND){assert(*(int *)arg!=fd);binds++;}
 else if(cmd==SBK_IOC_BIND_REGION){assert(((struct sbk_region_bind *)arg)->remote.id);binds++;}
 else if(cmd==SBK_IOC_CONFIG)configs++;
 else if(cmd==SBK_IOC_PREPARE_ANON){assert(plans<16);prepared[plans++]=fd;if(fail_plan==plans){errno=ENOMEM;return -1;}}
 else if(cmd==SBK_IOC_IMPORT_PS){struct sbk_ps_slice *s=arg;assert(s->source_fd!=fd&&s->pages);imports++;}
 else if(cmd==SBK_IOC_SEAL_REGION){assert(seals<16);sealed[seals++]=fd;assert(((struct sbk_region_seal *)arg)->remote.id);}
 else assert(cmd==SBK_IOC_WATCH_DRAIN||cmd==SBK_IOC_PRETRANSFER_MANY);
 return 0;
}
static unsigned fds(void){DIR*d=opendir("/proc/self/fd");assert(d);unsigned n=0;struct dirent*e;while((e=readdir(d)))if(e->d_name[0]!='.')n++;closedir(d);return n;}
static struct sbk_catalog *create(void){int fd=open("/dev/null",O_RDONLY|O_CLOEXEC);assert(fd>=0);struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA};struct sbk_catalog*c=sbk_catalog_create(fd,&cfg);close(fd);assert(c);return c;}
static void reset(void){features=SBK_FEATURE_PREPARED_ARM|SBK_FEATURE_UNBOUND_REGION|SBK_FEATURE_PARALLEL_PS|SBK_FEATURE_PS_SLICE;configs=plans=imports=seals=binds=fail_plan=0;}
static void test_reuse(void){
 unsigned before=fds();reset();struct sbk_catalog*c=create();
 struct sbk_catalog_layout l[3]={{10,0,0x100000,8},{10,0,0x200000,8},{11,0,0x100000,8}};
 assert(!sbk_catalog_prepare_layout(c,l,3)&&configs==3&&plans==3);
 assert(sbk_catalog_prepare_layout(c,l,3)==-EINVAL);
 /* Same-shaped PS byte cache must coexist with the unbound plan. */
 struct sbk_catalog_record r={.source_pid=10,.address=l[0].address,.remote={.address=0x900000,.pages=8,.id=1,.rkey=7}};
 uint64_t index=1;assert(!sbk_catalog_stage(c,&r,&index,1));assert(configs==4);
 struct sbk_catalog_final f[3]={0};
 for(unsigned i=0;i<3;i++)f[i].record=(struct sbk_catalog_record){.source_pid=l[i].source_pid,.restore_pid=20+i,.address=l[i].address,.remote={.address=0x800000+i*0x10000,.pages=l[i].pages,.id=2+i,.rkey=8}};
 f[1].record.remote.pages=4; /* Changed shape: fallback, retain unused owner off final path. */
 f[0].dirty=&index;f[0].dirty_count=1;
 assert(!sbk_catalog_seal(c,f,3));assert(configs==5&&plans==3&&imports==1&&seals==3);
 assert(sealed[0]==prepared[0]&&sealed[1]!=prepared[1]&&sealed[2]==prepared[2]);
 assert(fcntl(prepared[1],F_GETFD)>=0);
 struct sbk_catalog_timing t;sbk_catalog_get_timing(c,&t);
 assert(t.ps_plans==3&&t.reused_plans==2&&t.discarded_plans==1);
 assert(t.ps_plan_pages==24&&t.reused_plan_pages==16&&t.discarded_plan_pages==8);
 sbk_catalog_destroy(c);assert(fds()==before);
}
static void test_invalid(void){
 unsigned before=fds();reset();struct sbk_catalog*c=create();
 struct sbk_catalog_layout l[2]={{10,0,0x100000,8},{10,0,0x101000,8}};
 assert(sbk_catalog_prepare_layout(c,l,2)==-EINVAL&&!binds);
 l[1].address=0x200000;l[1].reserved=1;assert(sbk_catalog_prepare_layout(c,l,2)==-EINVAL&&!binds);
 l[1].reserved=0;l[1].pages=0;assert(sbk_catalog_prepare_layout(c,l,2)==-EINVAL&&!binds);
 l[1].pages=8;l[1].address=UINT64_MAX-4095;assert(sbk_catalog_prepare_layout(c,l,2)==-EINVAL&&!binds);
 l[1].address=0x200000;l[1].source_pid=0;assert(sbk_catalog_prepare_layout(c,l,2)==-EINVAL&&!binds);
 l[1].source_pid=10;fail_plan=2;assert(sbk_catalog_prepare_layout(c,l,2)==-ENOMEM);
 assert(sbk_catalog_prepare_layout(c,l,1)==-EINVAL);sbk_catalog_destroy(c);assert(fds()==before);
 reset();features&=~SBK_FEATURE_UNBOUND_REGION;c=create();assert(sbk_catalog_prepare_layout(c,l,1)==-EOPNOTSUPP&&!binds);sbk_catalog_destroy(c);assert(fds()==before);
}
int main(void){alarm(30);for(int i=0;i<20;i++){test_reuse();test_invalid();}puts("SBK_PS_LAYOUT_PASS exact_reuse changed_shape same_shape_cache deferred_cleanup malformed prepare_failure FD_leaks0 IOCTL_stub");return 0;}
