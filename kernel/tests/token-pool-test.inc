/* Included by sbk_test.c. Real kernel token ownership and anonymous PTEs. */
static struct sbk_token_pool_stats pool_stats(int fd)
{
    struct sbk_token_pool_stats p = {0};
    CHECK(!ioctl(fd, SBK_IOC_TOKEN_POOL_STATS, &p));
    return p;
}
static void pool_failure_after(const char *value)
{
    int fd=open("/sys/module/swiftbaton_k/parameters/token_pool_test_fail_after",O_WRONLY);
    CHECK(fd>=0 && write(fd,value,strlen(value))==(ssize_t)strlen(value) && !close(fd));
}
static void *pool_reserver(void *arg)
{
    int fd=*(int *)arg;uint64_t n=257;
    CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n));return NULL;
}
static void test_retirement_with_background(void)
{
    for (int repeat=0;repeat<32;repeat++) {
        struct fixture f=fixture(256,0,16,1000,SBK_NO_FAILURE);
        background(f.fd,NULL,0);
        /* Remove all aliases while speculative reads are outstanding. The
         * drain ACK must join them, not turn successful retirement into a
         * cancellation failure or revoke their source before completion. */
        usleep(100+(repeat%4)*500);
        CHECK(!munmap(f.dst,f.n*P));f.dst=NULL;
        struct sbk_drain_status d={0};
        for (int attempt=0;attempt<10000;attempt++) {
            CHECK(!ioctl(f.fd,SBK_IOC_DRAIN_STATUS,&d));
            if(d.drained)break;
            usleep(1000);
        }
        CHECK(d.drained&&d.retired_tokens==f.n);
        struct sbk_stats before=stats(f.fd);CHECK(!before.errors);
        usleep(10000);
        struct sbk_stats after=stats(f.fd);
        CHECK(!after.errors&&before.completed==after.completed);
        for(size_t i=0;i<f.n;i++) {
            struct sbk_page_info p={.index=i};CHECK(!ioctl(f.fd,SBK_IOC_PAGE,&p));
            CHECK(!p.started_ns||p.completed_ns);
        }
        finish(&f);
    }
    puts("PASS retirement_joins_background_without_cancellation_error");
}
static void test_token_pool(void)
{
    int previous=use_token_pool;use_token_pool=0;
    int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    struct sbk_capabilities caps;CHECK(!ioctl(fd,SBK_IOC_CAPABILITIES,&caps));
    CHECK(caps.features&SBK_FEATURE_TOKEN_POOL);
    uint64_t n=0;CHECK(ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==EINVAL);
    n=(uint64_t)SBK_TOKEN_POOL_MAX_PAGES+1;
    CHECK(ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==EINVAL);
    CHECK(ioctl(fd,SBK_IOC_TOKEN_RESERVE,(void *)1)<0&&errno==EFAULT);
    n=128;pool_failure_after("7");
    CHECK(ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==ENOMEM);
    struct sbk_token_pool_stats ps=pool_stats(fd);CHECK(ps.prepared==7&&ps.available==7&&!ps.claimed&&!ps.sealed);
    pool_failure_after("-1");CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n));
    CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n));
    ps=pool_stats(fd);CHECK(ps.prepared==128&&ps.available==128);
    pthread_t workers[8];for(int i=0;i<8;i++)CHECK(!pthread_create(&workers[i],NULL,pool_reserver,&fd));
    for(int i=0;i<8;i++)CHECK(!pthread_join(workers[i],NULL));
    ps=pool_stats(fd);CHECK(ps.prepared==257&&ps.available==257&&!ps.claimed);
    CHECK(!ioctl(fd,SBK_IOC_CANCEL));CHECK(ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==EINVAL);
    CHECK(!close(fd)); /* Entirely unused and partially failed pools. */
    for(int mode=0;mode<3;mode++) {
        defer_mapping=1;struct fixture f=fixture(64,1,2,0,SBK_NO_FAILURE);defer_mapping=0;
        n=mode==0?96:mode==1?17:64;CHECK(!ioctl(f.fd,SBK_IOC_TOKEN_RESERVE,&n));
        if(mode==2) {
            unsigned char *bad=mmap(NULL,64*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            CHECK(bad!=MAP_FAILED);bad[0]=123;
            struct sbk_anon_arm a={.address=(uintptr_t)bad};
            CHECK(ioctl(f.fd,SBK_IOC_ARM_ANON,&a)<0&&errno==EEXIST);
            CHECK(bad[0]==123&&!munmap(bad,64*P));
            ps=pool_stats(f.fd);CHECK(ps.claimed==64&&ps.available==0&&ps.sealed);
            CHECK(ioctl(f.fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==EBUSY);
        }
        f.dst=map_destination(f.fd,64);CHECK(f.dst!=MAP_FAILED);
        ps=pool_stats(f.fd);
        CHECK(ps.claimed==(mode==1?17:64));
        CHECK(ps.fallback==(mode==1?47:mode==2?64:0));
        CHECK(ps.available==(mode==0?32:0)&&ps.sealed);
        CHECK(ioctl(f.fd,SBK_IOC_TOKEN_RESERVE,&n)<0&&errno==EINVAL);
        /* Close the only device fd with both prepared leftovers and live PTEs. */
        CHECK(!close(f.fd));f.fd=-1;
        pid_t child=fork();CHECK(child>=0);
        if(!child){verify(&f);f.dst[8*P]^=0x5a;_exit(0);}
        int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
        verify(&f);CHECK(f.dst[8*P]==pattern(8,0));finish(&f);
    }
    /* Span two chunk allocations; cancellation must drop every unused token. */
    fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(fd>=0);n=65536+3;
    CHECK(!ioctl(fd,SBK_IOC_TOKEN_RESERVE,&n));ps=pool_stats(fd);
    CHECK(ps.available==n&&ps.prepared==n&&!ps.claimed);CHECK(!close(fd));
    use_token_pool=previous;
    test_retirement_with_background();
    puts("SBK_TOKEN_POOL_PASS partial_failure concurrency fallback retry close_before_fault fork_COW unused_chunks");
}
