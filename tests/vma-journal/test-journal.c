#define _GNU_SOURCE
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <pthread.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include "delta.h"
#define CHECK(x) do{if(!(x)){fprintf(stderr,"FAIL line=%d expr=%s errno=%d\n",__LINE__,#x,errno);exit(1);}}while(0)
#define P 4096
struct result { uintptr_t first,second,fixed; };
static void *thread_work(void *unused)
{
    (void)unused;
    for(int i=0;i<4;i++) {
        void *p=mmap(NULL,P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
        CHECK(!mprotect(p,P,PROT_READ));CHECK(!munmap(p,P));
    }
    return NULL;
}
static void child(int commands,int results)
{
    char command;CHECK(read(commands,&command,1)==1 && command=='S');
    struct result r={0};
    unsigned char *p=mmap(NULL,8*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);r.first=(uintptr_t)p;
    CHECK(!mprotect(p+P,P,PROT_READ));CHECK(!madvise(p+2*P,P,MADV_DONTNEED));
    CHECK(!mprotect(p+P,P,PROT_READ|PROT_WRITE)); /* Rejoin the split VMA for mremap. */
    p=mremap(p,8*P,12*P,MREMAP_MAYMOVE);CHECK(p!=MAP_FAILED);r.second=(uintptr_t)p;
    CHECK(mprotect(p+1,P,PROT_READ)<0 && errno==EINVAL);
    CHECK(mmap(p+2*P,2*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==p+2*P);r.fixed=(uintptr_t)(p+2*P);
    CHECK(!munmap(p,12*P));
    CHECK(syscall(SYS_brk,0)>0);
    pthread_t threads[4];
    for(int i=0;i<4;i++)CHECK(!pthread_create(&threads[i],NULL,thread_work,NULL));
    for(int i=0;i<4;i++)CHECK(!pthread_join(threads[i],NULL));
    CHECK(write(results,&r,sizeof(r))==sizeof(r));
    if(read(commands,&command,1)!=1)_exit(2);
    CHECK(command=='O');
    p=mmap(NULL,P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);CHECK(p!=MAP_FAILED);
    for(int i=0;i<SB_VMA_JOURNAL_CAPACITY+20;i++)CHECK(!mprotect(p,P,(i&1)?PROT_READ:PROT_READ|PROT_WRITE));
    CHECK(!munmap(p,P));CHECK(write(results,&r,sizeof(r))==sizeof(r));_exit(0);
}
static void unit_decode(void)
{
    struct journal_event e={.pid_tgid=(uint64_t)17<<32,.syscall_nr=9,.args={0x1000,4096},.result=0x9000};
    struct vma_delta d=decode_delta(&e);CHECK(d.pid==17 && d.scope==DELTA_RANGES && d.count==1 && d.ranges[0].start==0x9000 && d.ranges[0].end==0xa000);
    e.result=-ENOMEM;d=decode_delta(&e);CHECK(d.scope==DELTA_PROCESS);
    e.syscall_nr=25;e.args[0]=0x1000;e.args[1]=8192;e.args[2]=12288;e.args[3]=2;e.args[4]=0x9000;
    d=decode_delta(&e);CHECK(d.scope==DELTA_RANGES && d.count==2 && d.ranges[1].end==0xc000);
    e.syscall_nr=10;e.args[0]=UINT64_MAX-100;e.args[1]=4096;d=decode_delta(&e);CHECK(d.scope==DELTA_PROCESS);
    e.global_scope=1;d=decode_delta(&e);CHECK(d.scope==DELTA_ALL);
    puts("PASS conservative_range_decode hints fixed_remap overflow global_scope");
}
int main(int argc,char **argv)
{
    CHECK(argc==2);unit_decode();setvbuf(stdout,NULL,_IONBF,0);
    struct rlimit limit={RLIM_INFINITY,RLIM_INFINITY};CHECK(!setrlimit(RLIMIT_MEMLOCK,&limit));
    int to[2],from[2];CHECK(!pipe(to)&&!pipe(from));pid_t pid=fork();CHECK(pid>=0);
    if(!pid){close(to[1]);close(from[0]);child(to[0],from[1]);}
    close(to[0]);close(from[1]);
    struct bpf_object *obj=bpf_object__open_file(argv[1],NULL);CHECK(obj&&!libbpf_get_error(obj));CHECK(!bpf_object__load(obj));
    int watch=bpf_object__find_map_fd_by_name(obj,"watched"),epoch=bpf_object__find_map_fd_by_name(obj,"epoch"),journal=bpf_object__find_map_fd_by_name(obj,"journal");CHECK(watch>=0&&epoch>=0&&journal>=0);
    unsigned key=pid,value=1,zero=0;CHECK(!bpf_map_update_elem(watch,&key,&value,BPF_NOEXIST));
    struct bpf_link *enter=bpf_program__attach_raw_tracepoint(bpf_object__find_program_by_name(obj,"journal_enter"),"sys_enter");CHECK(enter&&!libbpf_get_error(enter));
    struct bpf_link *leave=bpf_program__attach_raw_tracepoint(bpf_object__find_program_by_name(obj,"journal_exit"),"sys_exit");CHECK(leave&&!libbpf_get_error(leave));
    CHECK(write(to[1],"S",1)==1);struct result r;CHECK(read(from[0],&r,sizeof(r))==sizeof(r));
    CHECK(ioctl(-1,0xaa00,0)<0); /* An unmonitored external UFFD attempt. */
    struct journal_epoch before,after;CHECK(!bpf_map_lookup_elem(epoch,&zero,&before));CHECK(!before.active&&!before.poisoned&&before.events<SB_VMA_JOURNAL_CAPACITY&&before.generation==2*before.events);
    unsigned bits=0,thread_events=0,globals=0;
    struct journal_key previous={0},next;unsigned seen=0;
    while(!bpf_map_get_next_key(journal,seen?&previous:NULL,&next)) {
        CHECK(seen<SB_VMA_JOURNAL_CAPACITY);seen++;previous=next;
        struct journal_event e;CHECK(!bpf_map_lookup_elem(journal,&next,&e)&&!memcmp(&e.key,&next,sizeof(next)));
        struct vma_delta d=decode_delta(&e);
        CHECK((unsigned)(e.pid_tgid>>32)==(unsigned)pid || e.global_scope);
        if((unsigned)e.pid_tgid!=(unsigned)pid && !e.global_scope)thread_events++;
        if(e.global_scope){CHECK(d.scope==DELTA_ALL);globals++;}
        if(e.syscall_nr==9&&e.result==(long long)r.first&&e.args[1]==8*P)bits|=1;
        if(e.syscall_nr==10&&e.args[0]==r.first+P&&e.result==0)bits|=2;
        if(e.syscall_nr==28&&e.args[0]==r.first+2*P&&e.result==0)bits|=4;
        if(e.syscall_nr==25&&e.args[0]==r.first&&e.result==(long long)r.second){CHECK(d.count==2);bits|=8;}
        if(e.syscall_nr==10&&e.args[0]==r.second+1&&e.result==-EINVAL)bits|=16;
        if(e.syscall_nr==9&&e.result==(long long)r.fixed&&(e.args[3]&MAP_FIXED))bits|=32;
        if(e.syscall_nr==11&&e.args[0]==r.second&&e.args[1]==12*P)bits|=64;
        if(e.syscall_nr==12){CHECK(d.scope==DELTA_PROCESS);bits|=128;}
    }
    CHECK(!bpf_map_lookup_elem(epoch,&zero,&after));CHECK(!memcmp(&before,&after,sizeof(before)));CHECK(bits==255&&thread_events>=48&&globals==1&&seen==before.events);
    printf("PASS native_BPF_journal events=%llu thread_events=%u globals=%u exact_syscall_arguments_and_returns=1\n",before.events,thread_events,globals);
    CHECK(write(to[1],"O",1)==1);CHECK(read(from[0],&r,sizeof(r))==sizeof(r));
    CHECK(!bpf_map_lookup_elem(epoch,&zero,&after));CHECK(!after.active&&after.poisoned&&after.events>SB_VMA_JOURNAL_CAPACITY);
    printf("PASS journal_overflow_requires_full_revalidation events=%llu poisoned=%llu\n",after.events,after.poisoned);
    int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
    bpf_link__destroy(leave);bpf_link__destroy(enter);bpf_object__close(obj);close(to[1]);close(from[0]);puts("SBK_VMA_JOURNAL_DRAFT_PASS no_migration_integration");return 0;
}
