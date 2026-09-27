/* Build with the actual cr-dump helper extracted by run.sh. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <stdio.h>
#include "sb-precopy.h"
#define MAX_PROCESS 4
#define IMG_FD_OFF 1
static int item_num=1, directory_fd;
static uint64_t pidset[MAX_PROCESS]={123},vpidset[MAX_PROCESS]={1};
static struct { unsigned sb_validation_workers; } opts={16};
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake=PTHREAD_COND_INITIALIZER;
static bool block_worker,entered,release_worker,fail_create,fail_validation;
static int calls,seen_fd;
static int get_service_fd(int id){assert(id==IMG_FD_OFF);return directory_fd;}
static void sb_trace(const char *phase){(void)phase;}
static void precopy_reserve_reset(void){}
static int precopy_reserve_source_page(pid_t pid,uint64_t address){return pid==123&&address==4096;}
static int test_create(pthread_t *thread,const pthread_attr_t *attr,void *(*fn)(void *),void *arg)
{ return fail_create ? EAGAIN : pthread_create(thread,attr,fn,arg); }
int sb_precopy_finalize_workers(const struct sb_precopy_pid *pids,size_t n,int fd,
                               sb_precopy_eligible_fn eligible,unsigned workers)
{
    assert(n==1&&pids[0].source==123&&pids[0].destination==1&&workers==16);
    assert(fd!=directory_fd&&fcntl(fd,F_GETFD)>=0&&eligible(123,4096));
    pthread_mutex_lock(&lock);calls++;seen_fd=fd;entered=true;pthread_cond_broadcast(&wake);
    while(block_worker&&!release_worker)pthread_cond_wait(&wake,&lock);
    pthread_mutex_unlock(&lock);
    assert(fcntl(fd,F_GETFD)>=0);
    return fail_validation ? -1 : 0;
}
#define pthread_create test_create
#include "validation-overlap.inc"
#undef pthread_create
int main(void)
{
    for(int mode=0;mode<3;mode++) {
        struct precopy_final_job job={.directory=-1};
        directory_fd=open("/tmp",O_RDONLY|O_DIRECTORY|O_CLOEXEC);assert(directory_fd>=0);
        block_worker=mode!=1;entered=release_worker=false;fail_create=mode==1;fail_validation=mode==2;
        int before=calls;assert(!precopy_final_start(&job));
        if(block_worker) {
            pthread_mutex_lock(&lock);
            while(!entered)pthread_cond_wait(&wake,&lock);
            /* Main-thread finalization can proceed while validation is live.
             * Its directory reference and copied PID list must stay valid. */
            assert(job.running);assert(!close(directory_fd));directory_fd=-1;
            pidset[0]=999;vpidset[0]=999;
            release_worker=true;pthread_cond_broadcast(&wake);pthread_mutex_unlock(&lock);
        }
        assert(precopy_final_finish(&job)==(mode==2?-1:0));
        assert(calls==before+1&&!job.running&&job.directory==-1);
        assert(fcntl(seen_fd,F_GETFD)<0&&errno==EBADF);
        assert(precopy_final_finish(&job)==(mode==2?-1:0));
        if(directory_fd>=0)assert(!close(directory_fd));
        pidset[0]=123;vpidset[0]=1;
    }
    directory_fd=-1;struct precopy_final_job invalid={.directory=-1};
    assert(precopy_final_start(&invalid)<0&&!invalid.prepared&&!precopy_final_finish(&invalid));
    puts("PASS actual validation helper: overlap, owned FD/PIDs, joined failure, synchronous fallback");
}
