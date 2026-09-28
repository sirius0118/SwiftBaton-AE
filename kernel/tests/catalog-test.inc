/* Included by sbk_test.c: scattered VMAs share the same lane/QP pools. */
static void *catalog_verify(void *arg)
{
    struct fixture *f = arg;
    CHECK(memcmp(f->dst, f->src, f->n * P) == 0);
    return NULL;
}
struct catalog_export {
    int fd;
    struct sbk_rdma_region *region;
    pthread_barrier_t *barrier;
};
static void *catalog_export_worker(void *arg)
{
    struct catalog_export *e = arg;
    pthread_barrier_wait(e->barrier);
    CHECK(!ioctl(e->fd, SBK_IOC_EXPORT_REGION, e->region));
    return NULL;
}
static void catalog_export_many(int fd, struct sbk_rdma_region *regions, unsigned count)
{
    if(getenv("SBK_TEST_BATCH_EXPORT")) {
        struct sbk_export_batch batch={.count=count,.workers=4};
        CHECK(count&&count<=SBK_MAX_BATCH);
        memcpy(batch.regions,regions,count*sizeof(*regions));
        CHECK(!ioctl(fd,SBK_IOC_EXPORT_BATCH,&batch));
        CHECK(batch.completed==count&&batch.peak&&batch.peak<=4);
        memcpy(regions,batch.regions,count*sizeof(*regions));
        printf("SBK_PEER_BATCH_EXPORT count=%u workers=4 peak=%u\n",count,batch.peak);
        return;
    }
    pthread_t threads[8]; struct catalog_export args[8];
    pthread_barrier_t start;
    CHECK(count && count<=8 && !pthread_barrier_init(&start,NULL,count));
    for (unsigned i=0;i<count;i++) {
        args[i]=(struct catalog_export){fd,&regions[i],&start};
        CHECK(!pthread_create(&threads[i],NULL,catalog_export_worker,&args[i]));
    }
    for (unsigned i=0;i<count;i++) CHECK(!pthread_join(threads[i],NULL));
    CHECK(!pthread_barrier_destroy(&start));
    for (unsigned i=0;i<count;i++) {
        CHECK(regions[i].id && regions[i].rkey);
        for (unsigned j=0;j<i;j++) CHECK(regions[i].id!=regions[j].id);
    }
}
static void test_rdma_catalog(void)
{
    enum { REGIONS = 4, NPAGES = 128 };
    struct fixture f[REGIONS];
    struct sbk_rdma_region remote[REGIONS];
    struct sbk_rdma_setup source = {.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=2000,.slots={4,2,2}}, target;
    struct sbk_config cfg = {.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
        .pages=NPAGES,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
        .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
    const char *gid=getenv("SBK_GID_INDEX"), *dev=getenv("SBK_RDMA_DEVICE");
    if(gid) source.gid_index=atoi(gid);
    snprintf(source.device,sizeof(source.device),"%s",dev?dev:"rxe0");
    target=source;target.role=SBK_RDMA_DESTINATION;
    int src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    int dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    CHECK(src>=0&&dst>=0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CREATE,&source)==0);
    CHECK(ioctl(dst,SBK_IOC_RDMA_CREATE,&target)==0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CONNECT,&target.local)==0);
    CHECK(ioctl(dst,SBK_IOC_RDMA_CONNECT,&source.local)==0);
    if (use_token_pool && use_anon) {
        uint64_t reserve = REGIONS * NPAGES + 17;
        CHECK(!ioctl(dst, SBK_IOC_TOKEN_RESERVE, &reserve));
    }
    for(int r=0;r<REGIONS;r++) {
        /* Guard pages make these independent, nonmergeable address ranges. */
        unsigned char *guard=mmap(NULL,(NPAGES+2)*P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(guard!=MAP_FAILED);
        f[r]=(struct fixture){.n=NPAGES,.src=guard+P,
            .fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC)};
        CHECK(f[r].fd>=0&&mprotect(f[r].src,NPAGES*P,PROT_READ|PROT_WRITE)==0);
        for(size_t p=0;p<NPAGES;p++)for(size_t b=0;b<P;b++)
            f[r].src[p*P+b]=pattern(p+r*997,b);
        remote[r]=(struct sbk_rdma_region){.address=(uintptr_t)f[r].src,.pages=NPAGES};
    }
    pthread_t exporters[REGIONS];
    struct catalog_export args[REGIONS];
    pthread_barrier_t exports_ready;
    CHECK(!pthread_barrier_init(&exports_ready, NULL, REGIONS));
    for (int r=0; r<REGIONS; r++) {
        args[r]=(struct catalog_export){src, &remote[r], &exports_ready};
        CHECK(!pthread_create(&exporters[r], NULL, catalog_export_worker, &args[r]));
    }
    for (int r=0; r<REGIONS; r++) CHECK(!pthread_join(exporters[r], NULL));
    CHECK(!pthread_barrier_destroy(&exports_ready));
    for (int r=0; r<REGIONS; r++) {
        CHECK(remote[r].id && remote[r].id <= REGIONS && remote[r].rkey);
        for (int q=0; q<r; q++) CHECK(remote[r].id != remote[q].id);
        struct sbk_region_bind bind={.session_fd=dst,.remote=remote[r]};
        bind.session_fd=f[r].fd;
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)<0&&errno==EINVAL);
        bind.session_fd=-1;
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)<0&&errno==EBADF);
        bind.session_fd=src;
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)<0&&errno==EINVAL);
        bind.session_fd=dst;bind.remote.pages=0;
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)<0&&errno==EINVAL);
        bind.remote=remote[r];
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)==0);
        CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)<0&&errno==EBUSY);
        cfg.pages=NPAGES-1;
        CHECK(ioctl(f[r].fd,SBK_IOC_CONFIG,&cfg)<0&&errno==EINVAL);
        cfg.pages=NPAGES;
        CHECK(ioctl(f[r].fd,SBK_IOC_CONFIG,&cfg)==0);
        exercise_stage(&f[r]);
        f[r].dst=map_destination(f[r].fd,NPAGES);
        CHECK(f[r].dst!=MAP_FAILED);
    }
    if (use_token_pool && use_anon) {
        struct sbk_token_pool_stats pool=pool_stats(dst);
        CHECK(pool.claimed==REGIONS*NPAGES && pool.available==17 && !pool.fallback);
        struct sbk_token_pool_stats child=pool_stats(f[2].fd);
        CHECK(!memcmp(&pool,&child,sizeof(pool)));
    }
    CHECK(close(dst)==0); /* Every view now exclusively retains the session. */
    CHECK(ioctl(f[0].fd,SBK_IOC_CANCEL)==0);
    expect_sigbus(&f[0],64);
    CHECK(munmap(f[0].dst,NPAGES*P)==0&&close(f[0].fd)==0);
    /* Closing an armed view also cannot destroy siblings or its own markers. */
    CHECK(close(f[3].fd)==0);f[3].fd=-1;
    for(int r=1;r<3;r++) {
        CHECK(f[r].dst[64*P]==f[r].src[64*P]);
        await_completed(f[r].fd,8); /* 4 PS - 1 invalidated + demand and 4 FT. */
        uint64_t hot[]={100,101,102,103,104,105,106,107};
        background(f[r].fd,hot,8);
    }
    pthread_t threads[REGIONS-1];
    for(int r=1;r<REGIONS;r++)CHECK(!pthread_create(&threads[r-1],NULL,catalog_verify,&f[r]));
    for(int r=1;r<REGIONS;r++)CHECK(!pthread_join(threads[r-1],NULL));
    for(int r=1;r<3;r++) {
        await_completed(f[r].fd,NPAGES);
        struct sbk_stats st=stats(f[r].fd);
        CHECK(!st.errors&&st.pretransferred==4&&st.invalidated==1);
        CHECK(st.fetched[0]>0&&st.fetched[1]>=4&&st.fetched[2]>0);
        CHECK(st.fetched[0]+st.fetched[1]+st.fetched[2]+3==NPAGES);
        printf("PASS catalog_region=%d PS=%llu invalid=%llu PF=%llu FT=%llu BG=%llu\n",r,
            (unsigned long long)st.pretransferred,(unsigned long long)st.invalidated,
            (unsigned long long)st.fetched[0],(unsigned long long)st.fetched[1],
            (unsigned long long)st.fetched[2]);
    }
    for(int r=1;r<REGIONS;r++) {
        CHECK(munmap(f[r].dst,NPAGES*P)==0);
        if(f[r].fd>=0)CHECK(close(f[r].fd)==0);
    }
    CHECK(ioctl(src,SBK_IOC_REVOKE_SOURCE)==0&&close(src)==0);
    for(int r=0;r<REGIONS;r++)CHECK(munmap(f[r].src-P,(NPAGES+2)*P)==0);
    puts("PASS RDMA_catalog_scattered_VMA_shared_QPs_parent_close_sibling_cancel");
}

/* A source controller passes its open catalog into the owning process. The
 * region must pin that process's mm, not the controller that created the PD. */
static void test_rdma_catalog_child_export(void)
{
    enum { NPAGES=48 };
    struct sbk_rdma_setup source={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=2000,.slots={2,2,2}},target;
    struct sbk_rdma_region remote;
    const char *gid=getenv("SBK_GID_INDEX"),*dev=getenv("SBK_RDMA_DEVICE");
    if(gid)source.gid_index=atoi(gid);
    snprintf(source.device,sizeof(source.device),"%s",dev?dev:"rxe0");
    target=source;target.role=SBK_RDMA_DESTINATION;
    int src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    int dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC), channel[2];
    CHECK(src>=0&&dst>=0&&pipe(channel)==0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CREATE,&source)==0);
    CHECK(ioctl(dst,SBK_IOC_RDMA_CREATE,&target)==0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CONNECT,&target.local)==0);
    CHECK(ioctl(dst,SBK_IOC_RDMA_CONNECT,&source.local)==0);
    pid_t child=fork();CHECK(child>=0);
    if(!child) {
        CHECK(close(channel[0])==0&&close(dst)==0);
        unsigned char *p=mmap(NULL,NPAGES*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED);
        for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)p[i*P+b]=pattern(i+891,b);
        struct sbk_rdma_region d={.address=(uintptr_t)p,.pages=NPAGES};
        CHECK(ioctl(src,SBK_IOC_EXPORT_REGION,&d)==0);
        CHECK(write(channel[1],&d,sizeof(d))==sizeof(d));
        /* RDMA MR pins outlive this mapping and this process. */
        CHECK(munmap(p,NPAGES*P)==0&&close(src)==0);
        _exit(0);
    }
    CHECK(close(channel[1])==0);
    CHECK(read(channel[0],&remote,sizeof(remote))==sizeof(remote));
    CHECK(close(channel[0])==0);
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    if (use_token_pool && use_anon) {
        uint64_t reserve=NPAGES+11;CHECK(!ioctl(dst,SBK_IOC_TOKEN_RESERVE,&reserve));
    }
    int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(fd>=0);
    struct sbk_region_bind bind={.session_fd=dst,.remote=remote};
    CHECK(ioctl(fd,SBK_IOC_BIND_REGION,&bind)==0&&close(dst)==0);
    struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
        .pages=NPAGES,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
        .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
    CHECK(ioctl(fd,SBK_IOC_CONFIG,&cfg)==0);
    unsigned char *p=map_destination(fd,NPAGES);CHECK(p!=MAP_FAILED);
    CHECK(close(fd)==0);
    for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)CHECK(p[i*P+b]==pattern(i+891,b));
    CHECK(munmap(p,NPAGES*P)==0);
    CHECK(ioctl(src,SBK_IOC_REVOKE_SOURCE)==0&&close(src)==0);
    puts("PASS RDMA_catalog_export_from_child_mm_process_exit_pinned_source");
}
static int catalog_peer_test(int source_side)
{
    enum { REGIONS=4, NPAGES=256 };
    struct fixture f[REGIONS];struct sbk_rdma_region regions[REGIONS];
    struct sbk_rdma_setup setup={.role=source_side?SBK_RDMA_SOURCE:SBK_RDMA_DESTINATION,
        .port=1,.gid_index=1,.timeout_ms=10000,.slots={4,2,2}};
    struct sbk_rdma_endpoint peer;
    const char *dev=getenv("SBK_RDMA_DEVICE"), *gid=getenv("SBK_GID_INDEX");
    const char *source_ip=getenv("SBK_PEER_SOURCE_IP");
    snprintf(setup.device,sizeof(setup.device),"%s",dev?dev:"rxe0");
    if (gid) setup.gid_index=atoi(gid);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(24567)};
    CHECK(inet_pton(AF_INET,source_ip?source_ip:"192.0.2.11",&addr.sin_addr)==1);
    int sock=socket(AF_INET,SOCK_STREAM,0),yes=1;CHECK(sock>=0);
    CHECK(setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes))==0);
    if(source_side) {
        CHECK(bind(sock,(struct sockaddr*)&addr,sizeof(addr))==0&&listen(sock,1)==0);
        int accepted=accept(sock,NULL,NULL);CHECK(accepted>=0);close(sock);sock=accepted;
    } else {
        int ok=0;
        for(int i=0;i<500;i++){if(!connect(sock,(struct sockaddr*)&addr,sizeof(addr))){ok=1;break;}usleep(20000);}
        CHECK(ok);
    }
    struct timeval timeout={.tv_sec=20};
    CHECK(!setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    CHECK(!setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)));
    int session=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(session>=0);
    CHECK(ioctl(session,SBK_IOC_RDMA_CREATE,&setup)==0);
    /* Distinct per-region contents catch address/offset/rkey cross-talk. */
    for(int r=0;r<REGIONS;r++) {
        unsigned char *guard=mmap(NULL,(NPAGES+2)*P,PROT_NONE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(guard!=MAP_FAILED&&mprotect(guard+P,NPAGES*P,PROT_READ|PROT_WRITE)==0);
        f[r]=(struct fixture){.n=NPAGES,.src=guard+P,.fd=-1};
        for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)f[r].src[i*P+b]=pattern(i+r*997,b);
        if(source_side){
            regions[r]=(struct sbk_rdma_region){.address=(uintptr_t)f[r].src,.pages=NPAGES};

        }
    }
    if(source_side) {
        catalog_export_many(session, regions, REGIONS);
        exchange_bytes(sock,&peer,sizeof(peer),0);
        CHECK(ioctl(session,SBK_IOC_RDMA_CONNECT,&peer)==0);
        exchange_bytes(sock,&setup.local,sizeof(setup.local),1);
        exchange_bytes(sock,regions,sizeof(regions),1);
        char done;exchange_bytes(sock,&done,1,0);CHECK(done=='P');
        /* PS is complete. Simulate final source COW/remap and quiescence. */
        for(int r=0;r<REGIONS;r++) {
            CHECK(mmap(f[r].src,NPAGES*P,PROT_READ|PROT_WRITE,
                MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==f[r].src);
            for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)f[r].src[i*P+b]=pattern(i+r*997,b);
            f[r].src[P]^=0x5a;f[r].src[64*P]^=0x3c;
            regions[r]=(struct sbk_rdma_region){.address=(uintptr_t)f[r].src,.pages=NPAGES};

        }
        catalog_export_many(session, regions, REGIONS);
        exchange_bytes(sock,regions,sizeof(regions),1);
        exchange_bytes(sock,&done,1,0);CHECK(done=='D');
        CHECK(ioctl(session,SBK_IOC_REVOKE_SOURCE)==0&&close(session)==0);
        puts("SBK_PEER_SOURCE_PASS catalog_regions=4 parallel_export=1");
    } else {
        exchange_bytes(sock,&setup.local,sizeof(setup.local),1);
        exchange_bytes(sock,&peer,sizeof(peer),0);
        CHECK(ioctl(session,SBK_IOC_RDMA_CONNECT,&peer)==0);
        exchange_bytes(sock,regions,sizeof(regions),0);
        for(int r=0;r<REGIONS;r++) {
            f[r].fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(f[r].fd>=0);
            struct sbk_region_bind bind={.session_fd=session,.remote=regions[r]};
            CHECK(ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind)==0);
            struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
                .pages=NPAGES,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
                .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
            CHECK(ioctl(f[r].fd,SBK_IOC_CONFIG,&cfg)==0);
            uint64_t pre[]={0,1,2,3,4,5,6,7};
            struct sbk_hot_list list={.indices=(uintptr_t)pre,.count=8};
            CHECK(ioctl(f[r].fd,SBK_IOC_PRETRANSFER,&list)==0);
        }
        CHECK(close(session)==0);
        char staged='P';exchange_bytes(sock,&staged,1,1);
        exchange_bytes(sock,regions,sizeof(regions),0);
        for(int r=0;r<REGIONS;r++) {
            uint64_t dirty[]={1,64};
            struct sbk_region_seal seal={.remote=regions[r],.dirty={.indices=(uintptr_t)dirty,.count=2}};
            CHECK(ioctl(f[r].fd,SBK_IOC_SEAL_REGION,&seal)==0);
            f[r].src[P]^=0x5a;f[r].src[64*P]^=0x3c;
            f[r].dst=map_destination(f[r].fd,NPAGES);CHECK(f[r].dst!=MAP_FAILED);
        }
        for(int r=0;r<REGIONS;r++){
            CHECK(f[r].dst[64*P]==f[r].src[64*P]);await_completed(f[r].fd,12);
            uint64_t hot[]={200,201,202,203,204,205,206,207};background(f[r].fd,hot,8);
        }
        pthread_t threads[REGIONS];
        for(int r=0;r<REGIONS;r++)CHECK(!pthread_create(&threads[r],NULL,catalog_verify,&f[r]));
        for(int r=0;r<REGIONS;r++)CHECK(!pthread_join(threads[r],NULL));
        for(int r=0;r<REGIONS;r++){
            await_completed(f[r].fd,NPAGES);struct sbk_stats st=stats(f[r].fd);
            CHECK(!st.errors&&st.pretransferred==8&&st.invalidated==1&&st.fetched[0]>0&&st.fetched[1]>=4&&st.fetched[2]>0);
            CHECK(st.fetched[0]+st.fetched[1]+st.fetched[2]+st.pretransferred-st.invalidated==NPAGES);
            printf("SBK_CATALOG_PEER_REGION=%d PS=%llu invalid=%llu PF=%llu FT=%llu BG=%llu completed=%llu\n",r,
                (unsigned long long)st.pretransferred,(unsigned long long)st.invalidated,(unsigned long long)st.fetched[0],
                (unsigned long long)st.fetched[1],(unsigned long long)st.fetched[2],
                (unsigned long long)st.completed);
            CHECK(munmap(f[r].dst,NPAGES*P)==0&&close(f[r].fd)==0);
        }
        char done='D';exchange_bytes(sock,&done,1,1);
        puts("SBK_PEER_DESTINATION_PASS catalog_regions=4 all_four_paths=1 final_MR_remap=1");
    }
    for(int r=0;r<REGIONS;r++)CHECK(munmap(f[r].src-P,(NPAGES+2)*P)==0);
    CHECK(close(sock)==0);return 0;
}
static void test_rdma_catalog_final_mr(void)
{
    enum { NPAGES=16 };
    struct sbk_rdma_setup source={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=2000,.slots={2,2,2}},target;
    const char *gid=getenv("SBK_GID_INDEX"),*dev=getenv("SBK_RDMA_DEVICE");
    if(gid)source.gid_index=atoi(gid);
    snprintf(source.device,sizeof(source.device),"%s",dev?dev:"rxe0");
    target=source;target.role=SBK_RDMA_DESTINATION;
    int src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    int dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    CHECK(src>=0&&dst>=0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CREATE,&source)==0&&ioctl(dst,SBK_IOC_RDMA_CREATE,&target)==0);
    CHECK(ioctl(src,SBK_IOC_RDMA_CONNECT,&target.local)==0&&ioctl(dst,SBK_IOC_RDMA_CONNECT,&source.local)==0);
    struct fixture f={.n=NPAGES,.fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC)};
    f.src=mmap(NULL,NPAGES*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(f.fd>=0&&f.src!=MAP_FAILED);
    for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)f.src[i*P+b]=pattern(i,b);
    struct sbk_rdma_region old={.address=(uintptr_t)f.src,.pages=NPAGES};
    CHECK(ioctl(src,SBK_IOC_EXPORT_REGION,&old)==0);
    struct sbk_region_bind bind={.session_fd=dst,.remote=old};
    CHECK(ioctl(f.fd,SBK_IOC_BIND_REGION,&bind)==0);
    struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
        .pages=NPAGES,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
        .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
    CHECK(ioctl(f.fd,SBK_IOC_CONFIG,&cfg)==0);
    uint64_t pre[]={0,1,4},dirty[]={1,1};
    struct sbk_hot_list list={.indices=(uintptr_t)pre,.count=3};
    CHECK(ioctl(f.fd,SBK_IOC_PRETRANSFER,&list)==0);
    /* Replace every backing PFN while the old source MR still pins old pages. */
    CHECK(mmap(f.src,NPAGES*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==f.src);
    for(size_t i=0;i<NPAGES;i++)for(size_t b=0;b<P;b++)f.src[i*P+b]=pattern(i,b);
    f.src[P]^=0x5a;f.src[2*P]^=0x3c;
    int stale=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(stale>=0);
    CHECK(ioctl(stale,SBK_IOC_BIND_REGION,&bind)==0&&ioctl(stale,SBK_IOC_CONFIG,&cfg)==0);
    unsigned char *old_view=map_destination(stale,NPAGES);CHECK(old_view!=MAP_FAILED);
    CHECK(old_view[P]==pattern(1,0)&&old_view[P]!=f.src[P]);
    CHECK(munmap(old_view,NPAGES*P)==0&&close(stale)==0);
    struct sbk_rdma_region fresh={.address=(uintptr_t)f.src,.pages=NPAGES};
    CHECK(ioctl(src,SBK_IOC_EXPORT_REGION,&fresh)==0&&fresh.id!=old.id);
    int imported=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);CHECK(imported>=0);
    bind.remote=fresh;
    CHECK(!ioctl(imported,SBK_IOC_BIND_REGION,&bind)&&!ioctl(imported,SBK_IOC_CONFIG,&cfg));
    struct sbk_ps_slice slice={.source_fd=f.fd,.pages=NPAGES+1};
    CHECK(ioctl(imported,SBK_IOC_IMPORT_PS,&slice)<0&&errno==EINVAL);
    CHECK(stats(f.fd).completed==3&&stats(imported).completed==0);
    slice.pages=NPAGES;slice.source_fd=imported;
    CHECK(ioctl(imported,SBK_IOC_IMPORT_PS,&slice)<0&&errno==EINVAL);
    slice.source_fd=f.fd;
    CHECK(!ioctl(imported,SBK_IOC_IMPORT_PS,&slice));
    CHECK(stats(f.fd).completed==0&&stats(f.fd).pretransferred==0);
    CHECK(stats(imported).completed==3&&stats(imported).pretransferred==3);
    CHECK(ioctl(imported,SBK_IOC_IMPORT_PS,&slice)<0&&errno==EEXIST);
    CHECK(!close(f.fd)&&!close(dst));f.fd=imported;
    struct sbk_region_seal seal={.remote=fresh,.dirty={.indices=(uintptr_t)dirty,.count=2}};
    CHECK(ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal)<0&&errno==EINVAL); /* duplicates */
    CHECK(stats(f.fd).completed==3&&stats(f.fd).invalidated==0);
    seal.dirty.count=1;seal.remote.pages=NPAGES-1;
    CHECK(ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal)<0&&errno==EINVAL);
    seal.remote=fresh;
    CHECK(ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal)==0);
    CHECK(stats(f.fd).completed==2&&stats(f.fd).invalidated==1);
    f.dst=map_destination(f.fd,NPAGES);CHECK(f.dst!=MAP_FAILED);
    CHECK(ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal)<0&&errno==EINVAL);
    CHECK(memcmp(f.dst,f.src,NPAGES*P)==0);
    struct sbk_stats st=stats(f.fd);
    CHECK(!st.errors&&st.completed==NPAGES&&st.pretransferred==3&&st.invalidated==1);
    CHECK(st.fetched[0]+st.fetched[1]+st.fetched[2]+2==NPAGES);
    finish(&f);CHECK(ioctl(src,SBK_IOC_REVOKE_SOURCE)==0&&close(src)==0);
    puts("PASS RDMA_catalog_final_MR_remap_old_PFN_staleness_dirty_seal");
    puts("PASS PS_import_bounds_same_fd_duplicate_rejection_ownership_move_source_close");
}
static struct sbk_drain_status drain_status(int fd)
{
    struct sbk_drain_status d;
    CHECK(ioctl(fd,SBK_IOC_DRAIN_STATUS,&d)==0);return d;
}
static void wait_drain_event(int fd)
{
    struct pollfd p={.fd=fd,.events=POLLIN};uint64_t value;
    CHECK(poll(&p,1,10000)==1&&(p.revents&POLLIN));
    CHECK(read(fd,&value,sizeof(value))==sizeof(value)&&value==1);
}
static void test_anonymous_source_retirement(void)
{
    struct fixture f=rdma_fixture(32);
    int event=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK),to_child[2],from_child[2];
    CHECK(event>=0&&!pipe(to_child)&&!pipe(from_child));
    CHECK(!ioctl(f.fd,SBK_IOC_WATCH_DRAIN,&event));
    CHECK(ioctl(f.fd,SBK_IOC_WATCH_DRAIN,&event)<0&&errno==EALREADY);
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        close(to_child[1]);close(from_child[0]);close(event);close(rdma_source_fd);close(f.fd);
        char cmd;CHECK(read(to_child[0],&cmd,1)==1&&cmd=='F');
        verify(&f);cmd='D';CHECK(write(from_child[1],&cmd,1)==1);
        CHECK(read(to_child[0],&cmd,1)==1&&cmd=='G');
        /* Source MR is revoked now; both present reads and ordinary COW work. */
        verify(&f);pid_t gc=fork();CHECK(gc>=0);
        if(!gc){verify(&f);f.dst[0]^=0xff;CHECK(f.dst[0]!=(unsigned char)pattern(0,0));_exit(0);}
        int status;CHECK(waitpid(gc,&status,0)==gc&&WIFEXITED(status)&&!WEXITSTATUS(status));
        verify(&f);CHECK(!munmap(f.dst,f.n*P));_exit(0);
    }
    close(to_child[0]);close(from_child[1]);
    /* Leave background disabled: only parent demand faults have resolved. */
    verify(&f);
    struct sbk_drain_status d=drain_status(f.fd);
    CHECK(d.armed&&d.pages==f.n&&!d.retired_tokens&&!d.drained);
    CHECK(stats(f.fd).completed==f.n); /* Fetched is NOT safe source retirement. */
    struct pollfd p={.fd=event,.events=POLLIN};CHECK(poll(&p,1,0)==0);
    char cmd='F';CHECK(write(to_child[1],&cmd,1)==1);
    CHECK(read(from_child[0],&cmd,1)==1&&cmd=='D');wait_drain_event(event);
    d=drain_status(f.fd);CHECK(d.drained&&d.retired_tokens==f.n);
    CHECK(!ioctl(rdma_source_fd,SBK_IOC_REVOKE_SOURCE));
    cmd='G';CHECK(write(to_child[1],&cmd,1)==1);
    int status;CHECK(waitpid(child,&status,0)==child&&WIFEXITED(status)&&!WEXITSTATUS(status));
    verify(&f);CHECK(stats(f.fd).fetched[0]+stats(f.fd).fetched[1]+stats(f.fd).fetched[2]==2*f.n);
    CHECK(!close(event)&&!close(to_child[1])&&!close(from_child[0]));
    finish(&f);CHECK(!close(rdma_source_fd));rdma_source_fd=-1;
    puts("PASS anonymous_drain_waits_for_fork_markers_source_revoke_then_COW");
    /* Registration after all marker resolution must still deliver an event. */
    f=fixture(8,0,1,0,SBK_NO_FAILURE);verify(&f);
    event=eventfd(0,EFD_CLOEXEC|EFD_NONBLOCK);CHECK(event>=0);
    CHECK(!ioctl(f.fd,SBK_IOC_WATCH_DRAIN,&event));wait_drain_event(event);
    d=drain_status(f.fd);CHECK(d.drained&&d.retired_tokens==f.n);
    CHECK(!close(event));finish(&f);
    puts("PASS anonymous_drain_late_event_registration");
}

static void test_anonymous_background_fork_move(int rdma)
{
    struct fixture f = rdma ? rdma_fixture(64) : fixture(64, 0, 2, 0, SBK_NO_FAILURE);
    int ready[2], go[2], event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    CHECK(event >= 0 && !pipe(ready) && !pipe(go));
    CHECK(!ioctl(f.fd, SBK_IOC_WATCH_DRAIN, &event));
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) {
        close(ready[0]); close(go[1]);
        unsigned char *old = f.dst;
        unsigned char *moved = mmap(NULL, f.n * P, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(moved != MAP_FAILED);
        CHECK(mremap(f.dst, f.n * P, f.n * P, MREMAP_MAYMOVE | MREMAP_FIXED, moved) == moved);
        f.dst = moved;
        CHECK(mmap(old, f.n * P, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == old);
        memset(old, 0xb6, f.n * P);
        CHECK(!mprotect(moved + 8 * P, 8 * P, PROT_NONE));
        char cmd = 'R'; CHECK(write(ready[1], &cmd, 1) == 1);
        CHECK(read(go[0], &cmd, 1) == 1 && cmd == 'G');
        /* No application access to these markers until the source is revoked. */
        CHECK(!mprotect(moved + 8 * P, 8 * P, PROT_READ | PROT_WRITE));
        verify(&f);
        for (size_t i = 0; i < f.n * P; i++) CHECK(old[i] == 0xb6);
        f.dst[0] ^= 0xff; CHECK(f.dst[0] != pattern(0, 0));
        _exit(0);
    }
    close(ready[1]); close(go[0]);
    char cmd; CHECK(read(ready[0], &cmd, 1) == 1 && cmd == 'R');
    /* Move the parent's mapping independently; both old addresses are stale. */
    unsigned char *moved = mmap(NULL, f.n * P, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(moved != MAP_FAILED);
    CHECK(mremap(f.dst, f.n * P, f.n * P, MREMAP_MAYMOVE | MREMAP_FIXED, moved) == moved);
    f.dst = moved;
    background(f.fd, NULL, 0);
    wait_drain_event(event);
    struct sbk_drain_status d = drain_status(f.fd);
    CHECK(d.drained && d.retired_tokens == f.n);
    struct sbk_stats s = stats(f.fd);
    CHECK(!s.errors && !s.faults && s.fetched[SBK_BACKGROUND] == 2 * f.n);
    if (rdma) CHECK(!ioctl(rdma_source_fd, SBK_IOC_REVOKE_SOURCE));
    cmd = 'G'; CHECK(write(go[1], &cmd, 1) == 1);
    int status; CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) && !WEXITSTATUS(status));
    verify(&f); CHECK(!stats(f.fd).faults);
    CHECK(!close(event) && !close(ready[0]) && !close(go[1]));
    finish(&f);
    if (rdma) { CHECK(!close(rdma_source_fd)); rdma_source_fd = -1; }
    printf("PASS anonymous_background_fork_mremap_guard_source_retirement backend=%s\n", rdma ? "RDMA" : "LOOPBACK_TEST");
}

/* Revoke must cover registrations racing on the shared source session. The
 * first MR per worker is guaranteed; the remaining ones race the revoker. */
struct export_race {
    int fd;
    unsigned char *source;
    pthread_barrier_t *barrier;
    unsigned success, cancelled;
};
static void *export_race_worker(void *arg)
{
    struct export_race *w = arg;
    struct sbk_rdma_region d = {.address=(uintptr_t)w->source, .pages=64};
    CHECK(!ioctl(w->fd, SBK_IOC_EXPORT_REGION, &d));
    w->success++;
    pthread_barrier_wait(w->barrier);
    for (unsigned i=0; i<16; i++) {
        d=(struct sbk_rdma_region){.address=(uintptr_t)w->source, .pages=64};
        if (!ioctl(w->fd, SBK_IOC_EXPORT_REGION, &d)) w->success++;
        else { CHECK(errno==ECANCELED); w->cancelled++; break; }
    }
    CHECK(!close(w->fd));
    return NULL;
}
static void test_rdma_export_revoke(void)
{
    enum { WORKERS=8, ROUNDS=4 };
    unsigned successes=0, cancelled=0;
    for (int round=0; round<ROUNDS; round++) {
        struct sbk_rdma_setup setup={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
            .timeout_ms=2000,.slots={1,1,1}};
        const char *dev=getenv("SBK_RDMA_DEVICE"), *gid=getenv("SBK_GID_INDEX");
        snprintf(setup.device,sizeof(setup.device),"%s",dev?dev:"rxe0");
        if (gid) setup.gid_index=atoi(gid);
        int fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
        CHECK(fd>=0 && !ioctl(fd,SBK_IOC_RDMA_CREATE,&setup));
        struct export_race args[WORKERS]; pthread_t threads[WORKERS];
        pthread_barrier_t start; CHECK(!pthread_barrier_init(&start,NULL,WORKERS+1));
        for (int i=0;i<WORKERS;i++) {
            unsigned char *source=mmap(NULL,64*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
            CHECK(source!=MAP_FAILED);
            memset(source,i+round,64*P);
            args[i]=(struct export_race){.fd=dup(fd),.source=source,.barrier=&start};
            CHECK(args[i].fd>=0 && !pthread_create(&threads[i],NULL,export_race_worker,&args[i]));
        }
        int revoker=dup(fd); CHECK(revoker>=0 && !close(fd));
        /* A registered MR whose descriptor cannot be copied back is still
         * owned by the session and must be included in revoke/teardown. */
        struct sbk_rdma_region *badout=mmap(NULL,P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(badout!=MAP_FAILED);
        *badout=(struct sbk_rdma_region){.address=(uintptr_t)args[0].source,.pages=64};
        CHECK(!mprotect(badout,P,PROT_READ));
        CHECK(ioctl(revoker,SBK_IOC_EXPORT_REGION,badout)<0 && errno==EFAULT);
        CHECK(!munmap(badout,P));
        pthread_barrier_wait(&start);
        CHECK(!ioctl(revoker,SBK_IOC_REVOKE_SOURCE));
        /* The writer has returned: no subsequent MR may escape this fence. */
        for (int i=0;i<WORKERS;i++) {
            CHECK(!pthread_join(threads[i],NULL));
            successes+=args[i].success; cancelled+=args[i].cancelled;
            struct sbk_rdma_region d={.address=(uintptr_t)args[i].source,.pages=64};
            CHECK(ioctl(revoker,SBK_IOC_EXPORT_REGION,&d)<0 && errno==ECANCELED);
            CHECK(!munmap(args[i].source,64*P));
        }
        CHECK(!ioctl(revoker,SBK_IOC_REVOKE_SOURCE));
        CHECK(!close(revoker) && !pthread_barrier_destroy(&start));
    }
    CHECK(successes>=ROUNDS*WORKERS && cancelled);
    printf("PASS RDMA_parallel_export_revoke rounds=%u workers=%u registered=%u cancelled=%u late_rejected=1 original_fd_closed=1 copyout_error_reclaimed=1\n",
        ROUNDS,WORKERS,successes,cancelled);
}
