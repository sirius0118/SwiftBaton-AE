#include <dirent.h>
/* Included by sbk_test.c. Real MRs/RDMA; invalid input, failed pin/copyout and
 * revoke races must keep every registration owned by the source session. */
struct batch_race {
    int fd;
    struct sbk_export_batch batch;
    pthread_barrier_t *start;
    int result, error;
};
static void *batch_race_worker(void *arg)
{
    struct batch_race *r=arg;
    pthread_barrier_wait(r->start);
    r->result=ioctl(r->fd,SBK_IOC_EXPORT_BATCH,&r->batch);
    r->error=errno;
    return NULL;
}
static struct sbk_export_batch export_batch(unsigned char *data, unsigned workers)
{
    struct sbk_export_batch b={.count=SBK_MAX_BATCH,.workers=workers};
    for(unsigned i=0;i<b.count;i++)
        b.regions[i]=(struct sbk_rdma_region){.address=(uintptr_t)data+i*64*P,.pages=64};
    return b;
}
static int export_source(void)
{
    struct sbk_rdma_setup setup={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=2000,.slots={1,1,1}};
    const char *dev=getenv("SBK_RDMA_DEVICE"),*gid=getenv("SBK_GID_INDEX");
    snprintf(setup.device,sizeof(setup.device),"%s",dev?dev:"rxe0");
    if(gid)setup.gid_index=atoi(gid);
    int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    CHECK(fd>=0&&!ioctl(fd,SBK_IOC_RDMA_CREATE,&setup));
    return fd;
}
static int export_helper_present(void)
{
    DIR *dir=opendir("/proc");CHECK(dir);struct dirent *entry;int found=0;
    while((entry=readdir(dir))) {
        if(entry->d_name[0]<'0'||entry->d_name[0]>'9')continue;
        char path[320],name[64];snprintf(path,sizeof(path),"/proc/%s/comm",entry->d_name);
        FILE *f=fopen(path,"r");if(!f)continue;
        if(fgets(name,sizeof(name),f)&&!strncmp(name,"sbk_export/",11))found=1;
        fclose(f);if(found)break;
    }
    CHECK(!closedir(dir));return found;
}
static void test_export_killed_owner(void)
{
    int fd=export_source(), ready[2];CHECK(!pipe(ready)&&!export_helper_present());
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        close(ready[0]);size_t pages=32*4096;
        unsigned char *p=mmap(NULL,pages*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED&&!madvise(p,pages*P,MADV_NOHUGEPAGE));memset(p,0x59,pages*P);
        struct sbk_export_batch b={.count=32,.workers=4};
        for(unsigned i=0;i<b.count;i++)b.regions[i]=(struct sbk_rdma_region){.address=(uintptr_t)p+i*4096*P,.pages=4096};
        char c='R';CHECK(write(ready[1],&c,1)==1);
        CHECK(!ioctl(fd,SBK_IOC_EXPORT_BATCH,&b));_exit(0);
    }
    close(ready[1]);char c;CHECK(read(ready[0],&c,1)==1&&c=='R');
    int status,seen=0;
    for(unsigned i=0;i<10000;i++) {
        if(export_helper_present()){seen=1;break;}
        CHECK(!waitpid(child,&status,WNOHANG));
    }
    CHECK(seen&&!kill(child,SIGKILL));
    CHECK(waitpid(child,&status,0)==child&&WIFSIGNALED(status)&&WTERMSIG(status)==SIGKILL);
    CHECK(!export_helper_present());
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd)&&!close(ready[0]));
    puts("PASS RDMA_export_batch_SIGKILL_during_helpers joined=1 owner_mm_retained_until_revoke=1");
}
static void test_rdma_export_batch(void)
{
    test_export_killed_owner();
    unsigned char *data=mmap(NULL,SBK_MAX_BATCH*64*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(data!=MAP_FAILED);memset(data,0x57,SBK_MAX_BATCH*64*P);
    int fd=export_source();
    struct sbk_export_batch b=export_batch(data,4);
    struct sbk_capabilities caps;
    CHECK(!ioctl(fd,SBK_IOC_CAPABILITIES,&caps)&&(caps.features&SBK_FEATURE_PARALLEL_EXPORT));
    for(unsigned mode=0;mode<4;mode++) {
        b=export_batch(data,4);
        if(mode==0)b.workers=0;else if(mode==1)b.workers=33;
        else if(mode==2)b.peak=1;else b.count=33;
        CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,&b)<0&&errno==EINVAL);
    }
    b=export_batch(data,4);
    /* Last malformed descriptor must prevent *all* registrations. */
    b.regions[b.count-1].pages=0;
    CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,&b)<0&&errno==EINVAL);
    b=export_batch(data,4);
    CHECK(!ioctl(fd,SBK_IOC_EXPORT_BATCH,&b));
    CHECK(b.completed==b.count&&b.peak>=1&&b.peak<=4);
    for(unsigned i=0;i<b.count;i++) {
        CHECK(b.regions[i].id>=1&&b.regions[i].id<=b.count&&b.regions[i].rkey);
        for(unsigned j=0;j<i;j++)CHECK(b.regions[i].id!=b.regions[j].id);
    }
    printf("PASS RDMA_export_batch count=%u workers=4 peak=%u malformed_atomic=1 unordered_ids=1\n",b.count,b.peak);
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd));
    /* Deterministic partial pin failure, serial batch: first MR stays owned. */
    fd=export_source();b=export_batch(data,1);b.count=2;b.regions[1].address=0;
    CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,&b)<0&&b.completed==1&&b.regions[0].id==1);
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd));
    /* Parallel failure/copyout: revoke must reclaim successes not delivered. */
    fd=export_source();b=export_batch(data,4);b.regions[7].address=0;
    CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,&b)<0&&b.completed<b.count);
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd));
    fd=export_source();struct sbk_export_batch *ro=mmap(NULL,P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(ro!=MAP_FAILED);*ro=export_batch(data,4);CHECK(!mprotect(ro,P,PROT_READ));
    CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,ro)<0&&errno==EFAULT);
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd)&&!munmap(ro,P));
    unsigned successes=0,cancelled=0;
    for(unsigned round=0;round<4;round++) {
        fd=export_source();pthread_barrier_t start;pthread_t thread;
        CHECK(!pthread_barrier_init(&start,NULL,2));
        struct batch_race r={.fd=fd,.batch=export_batch(data,8),.start=&start};
        CHECK(!pthread_create(&thread,NULL,batch_race_worker,&r));
        pthread_barrier_wait(&start);
        if(round&1)usleep(100);
        CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE));
        CHECK(!pthread_join(thread,NULL));
        CHECK(!r.result||r.error==ECANCELED);
        successes+=r.batch.completed;cancelled+=!!r.result;
        b=export_batch(data,4);
        CHECK(ioctl(fd,SBK_IOC_EXPORT_BATCH,&b)<0&&errno==ECANCELED&&b.completed==0);
        CHECK(!close(fd)&&!pthread_barrier_destroy(&start));
    }
    /* Owner exit with MRs retained by a controller: its mm must stay valid
     * until revoke, even though the process and borrowed-mm helpers are gone. */
    fd=export_source();pid_t child=fork();CHECK(child>=0);
    if(!child){b=export_batch(data,4);CHECK(!ioctl(fd,SBK_IOC_EXPORT_BATCH,&b));_exit(0);}
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(!ioctl(fd,SBK_IOC_REVOKE_SOURCE)&&!close(fd));
    CHECK(!munmap(data,SBK_MAX_BATCH*64*P));
    printf("PASS RDMA_export_batch_cleanup partial_pin=1 copyout=1 revoke_races=4 completed=%u cancelled=%u owner_exit=1\n",successes,cancelled);
}
