/* Global DMA key correctness: real RXE READs, no CPU-copy transport fallback. */
static void dma_pair(int *src, int *dst)
{
    struct sbk_rdma_setup a={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=500,.slots={2,2,2}}, b;
    const char *dev=getenv("SBK_RDMA_DEVICE"), *gid=getenv("SBK_GID_INDEX");
    snprintf(a.device,sizeof(a.device),"%s",dev?dev:"rxe0");
    if(gid) a.gid_index=atoi(gid);
    b=a; b.role=SBK_RDMA_DESTINATION;
    *src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    *dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    CHECK(*src>=0 && *dst>=0);
    struct sbk_capabilities cap;
    CHECK(!ioctl(*src,SBK_IOC_CAPABILITIES,&cap) && (cap.features&SBK_FEATURE_DMA_MR));
    CHECK(!ioctl(*src,SBK_IOC_RDMA_CREATE,&a));
    CHECK(!ioctl(*dst,SBK_IOC_RDMA_CREATE,&b));
    CHECK(!ioctl(*src,SBK_IOC_DMA_ENABLE));
    CHECK(!ioctl(*dst,SBK_IOC_DMA_ENABLE));
    CHECK(ioctl(*src,SBK_IOC_DMA_ENABLE)<0 && errno==EBUSY);
    CHECK(!ioctl(*src,SBK_IOC_RDMA_CONNECT,&b.local));
    CHECK(!ioctl(*dst,SBK_IOC_RDMA_CONNECT,&a.local));
    CHECK(ioctl(*dst,SBK_IOC_DMA_ENABLE)<0 && errno==EBUSY);
}
static void dma_exchange(int src,int dst,const struct sbk_rdma_region *remote,int invalid)
{
    uint64_t *addresses=calloc(remote->pages,sizeof(*addresses)); CHECK(addresses);
    struct sbk_dma_map m={.region=*remote,.addresses=(uintptr_t)addresses};
    CHECK(!ioctl(src,SBK_IOC_DMA_EXPORT_MAP,&m));
    if(invalid) {
        struct sbk_dma_map bad=m; bad.addresses=UINT64_MAX;
        CHECK(ioctl(src,SBK_IOC_DMA_EXPORT_MAP,&bad)<0 && errno==EFAULT);
        CHECK(ioctl(dst,SBK_IOC_DMA_IMPORT_MAP,&bad)<0 && errno==EFAULT);
        bad=m; bad.region.address+=P;
        CHECK(ioctl(src,SBK_IOC_DMA_EXPORT_MAP,&bad)<0 && errno==ENOENT);
        addresses[0]++;
        CHECK(ioctl(dst,SBK_IOC_DMA_IMPORT_MAP,&m)<0 && errno==EINVAL);
        addresses[0]--;
    }
    CHECK(!ioctl(dst,SBK_IOC_DMA_IMPORT_MAP,&m));
    CHECK(ioctl(dst,SBK_IOC_DMA_IMPORT_MAP,&m)<0 && errno==EEXIST);
    free(addresses);
}
static struct fixture dma_view(int root,struct sbk_rdma_region remote,int prefetch)
{
    struct fixture f={.n=remote.pages,.fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC)};
    struct sbk_region_bind bind={.session_fd=root,.remote=remote};
    struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
        .pages=remote.pages,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
        .prefetch_enabled=prefetch,.test_fail_page=SBK_NO_FAILURE};
    CHECK(f.fd>=0 && !ioctl(f.fd,SBK_IOC_BIND_REGION,&bind));
    CHECK(!ioctl(f.fd,SBK_IOC_CONFIG,&cfg));
    return f;
}
static void test_dma_catalog(void)
{
    enum {N=64}; int src,dst; dma_pair(&src,&dst);
    unsigned char *bytes=mmap(NULL,N*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(bytes!=MAP_FAILED);
    for(size_t p=0;p<N;p++) for(size_t b=0;b<P;b++) bytes[p*P+b]=pattern(p,b);
    struct sbk_rdma_region ps={.address=(uintptr_t)bytes,.pages=N};
    CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&ps));
    int missing=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC); CHECK(missing>=0);
    struct sbk_region_bind bind={.session_fd=dst,.remote=ps};
    CHECK(ioctl(missing,SBK_IOC_BIND_REGION,&bind)<0 && errno==EINVAL);
    dma_exchange(src,dst,&ps,1);
    bind.remote.rkey^=1;
    CHECK(ioctl(missing,SBK_IOC_BIND_REGION,&bind)<0 && errno==EINVAL);
    CHECK(!close(missing));
    struct fixture f=dma_view(dst,ps,1); f.src=bytes;
    uint64_t pre[]={0,2,5}, dirty[]={2};
    struct sbk_hot_list list={.indices=(uintptr_t)pre,.count=3};
    CHECK(!ioctl(f.fd,SBK_IOC_PRETRANSFER,&list));
    bytes[2*P]^=0x6b;
    struct sbk_rdma_region final={.address=(uintptr_t)bytes,.pages=N};
    CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&final));
    CHECK(final.rkey==ps.rkey && final.id!=ps.id);
    dma_exchange(src,dst,&final,0);
    struct sbk_region_seal seal={.remote=final,.dirty={.indices=(uintptr_t)dirty,.count=1}};
    CHECK(!ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal));
    f.dst=map_destination(f.fd,N); CHECK(f.dst!=MAP_FAILED);
    CHECK(!close(dst)); /* Region retains the immutable vector and transport. */
    CHECK(f.dst[32*P]==pattern(32,0));
    await_completed(f.fd,7); /* two valid PS + demand + four prefetch */
    uint64_t hot[]={50,51,52,53}; background(f.fd,hot,4);
    await_completed(f.fd,N);
    CHECK(!memcmp(f.dst,bytes,N*P));
    struct sbk_stats s=stats(f.fd);
    CHECK(!s.errors && s.pretransferred==3 && s.invalidated==1);
    CHECK(s.fetched[0]>0 && s.fetched[1]>=4 && s.fetched[2]>0);
    CHECK(s.fetched[0]+s.fetched[1]+s.fetched[2]+2==N);
    CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE));
    CHECK(!memcmp(f.dst,bytes,N*P));
    CHECK(!close(src)); finish(&f);
    printf("PASS DMA_MR_four_paths PS=3 invalid=1 PF=%llu FT=%llu BG=%llu\n",
        (unsigned long long)s.fetched[0],(unsigned long long)s.fetched[1],(unsigned long long)s.fetched[2]);

    /* The old VA can disappear or be reused; pins must still protect its old
     * physical/DMA pages. This is WHY a global key cannot replace lifetime. */
    dma_pair(&src,&dst);
    bytes=mmap(NULL,N*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(bytes!=MAP_FAILED);
    for(size_t p=0;p<N;p++) for(size_t b=0;b<P;b++) bytes[p*P+b]=pattern(p,b);
    final=(struct sbk_rdma_region){.address=(uintptr_t)bytes,.pages=N};
    CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&final)); dma_exchange(src,dst,&final,0);
    f=dma_view(dst,final,0); f.src=bytes;
    CHECK(mmap(bytes,N*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED,-1,0)==bytes);
    memset(bytes,0xa9,N*P);
    f.dst=map_destination(f.fd,N); CHECK(f.dst!=MAP_FAILED);
    CHECK(f.dst[0]==pattern(0,0));
    CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE));
    expect_sigbus(&f,N-1);
    CHECK(stats(f.fd).errors>0);
    CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE)); CHECK(!close(src)); CHECK(!close(dst)); finish(&f);
    puts("PASS DMA_MR_source_unmap_remap_retains_old_pages_and_revoke_contains_fault");
}
static void test_dma_revoke_race(void)
{
    enum {W=8}; int src,dst; dma_pair(&src,&dst);
    struct export_race args[W]; pthread_t workers[W]; pthread_barrier_t gate;
    CHECK(!pthread_barrier_init(&gate,NULL,W+1));
    for(int i=0;i<W;i++) {
        unsigned char *p=mmap(NULL,64*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(p!=MAP_FAILED); memset(p,i+1,64*P);
        args[i]=(struct export_race){.fd=dup(src),.source=p,.barrier=&gate};
        CHECK(args[i].fd>=0 && !pthread_create(&workers[i],NULL,export_race_worker,&args[i]));
    }
    struct sbk_rdma_region *bad=mmap(NULL,P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    CHECK(bad!=MAP_FAILED); *bad=(struct sbk_rdma_region){.address=(uintptr_t)args[0].source,.pages=64};
    CHECK(!mprotect(bad,P,PROT_READ));
    CHECK(ioctl(src,SBK_IOC_EXPORT_REGION,bad)<0 && errno==EFAULT); CHECK(!munmap(bad,P));
    pthread_barrier_wait(&gate); CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE));
    unsigned completed=0,cancelled=0;
    for(int i=0;i<W;i++) {
        CHECK(!pthread_join(workers[i],NULL)); completed+=args[i].success; cancelled+=args[i].cancelled;
        struct sbk_rdma_region d={.address=(uintptr_t)args[i].source,.pages=64};
        CHECK(ioctl(src,SBK_IOC_EXPORT_REGION,&d)<0 && errno==ECANCELED);
        CHECK(!munmap(args[i].source,64*P));
    }
    CHECK(completed>=W && cancelled);
    CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE) && !close(src) && !close(dst));
    CHECK(!pthread_barrier_destroy(&gate));
    printf("PASS DMA_MR_parallel_export_revoke registered=%u cancelled=%u copyout_failure_owned=1\n",completed,cancelled);
}

/* One source/one destination: controlled physical transport check and matched
 * cold-load comparison. Ordinary mode uses identical catalog/QP/test setup. */
static void dma_peer_region(int sock,int session,struct sbk_rdma_region *d,int sending,int dma)
{
    exchange_bytes(sock,d,sizeof(*d),sending);
    CHECK(d->pages==1024 && d->id && d->id<=SBK_MAX_REGIONS);
    if(!dma) return;
    uint64_t addresses[1024];
    struct sbk_dma_map map={.region=*d,.addresses=(uintptr_t)addresses};
    if(sending) CHECK(!ioctl(session,SBK_IOC_DMA_EXPORT_MAP,&map));
    exchange_bytes(sock,addresses,sizeof(addresses),sending);
    if(!sending) CHECK(!ioctl(session,SBK_IOC_DMA_IMPORT_MAP,&map));
}
static int dma_peer_test(int source)
{
    const size_t n=1024;
    int dma=!getenv("SBK_TEST_DMA_ORDINARY"),probe=!!getenv("SBK_FAULT_PROBE");
    struct sbk_rdma_setup setup={.role=source?SBK_RDMA_SOURCE:SBK_RDMA_DESTINATION,
        .port=1,.gid_index=1,.timeout_ms=2000,.slots={4,2,2}};
    const char *dev=getenv("SBK_RDMA_DEVICE"), *gid=getenv("SBK_GID_INDEX"), *ip=getenv("SBK_PEER_SOURCE_IP");
    snprintf(setup.device,sizeof(setup.device),"%s",dev?dev:"rxe0");
    if(gid) setup.gid_index=atoi(gid);
    struct sockaddr_in addr={.sin_family=AF_INET,.sin_port=htons(24568)};
    CHECK(inet_pton(AF_INET,ip?ip:"192.0.2.11",&addr.sin_addr)==1);
    int sock=socket(AF_INET,SOCK_STREAM,0),yes=1; CHECK(sock>=0);
    CHECK(!setsockopt(sock,SOL_SOCKET,SO_REUSEADDR,&yes,sizeof(yes)));
    if(source) {
        CHECK(!bind(sock,(struct sockaddr*)&addr,sizeof(addr)) && !listen(sock,1));
        int peer=accept(sock,NULL,NULL); CHECK(peer>=0); close(sock); sock=peer;
    } else {
        int ok=0;
        for(int i=0;i<500;i++) { if(!connect(sock,(struct sockaddr*)&addr,sizeof(addr))){ok=1;break;} usleep(20000); }
        CHECK(ok);
    }
    struct timeval timeout={.tv_sec=20};
    CHECK(!setsockopt(sock,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)));
    CHECK(!setsockopt(sock,SOL_SOCKET,SO_SNDTIMEO,&timeout,sizeof(timeout)));
    uint32_t mode=dma+2*probe,remote_mode;
    exchange_bytes(sock,&mode,sizeof(mode),1); exchange_bytes(sock,&remote_mode,sizeof(remote_mode),0);
    CHECK(mode==remote_mode);
    int root=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC); CHECK(root>=0);
    CHECK(!ioctl(root,SBK_IOC_RDMA_CREATE,&setup));
    uint64_t started=now_raw();
    if(dma) CHECK(!ioctl(root,SBK_IOC_DMA_ENABLE));
    printf("SBK_DMA_ENABLE source=%d dma=%d elapsed_ns=%llu\n",source,dma,(unsigned long long)(now_raw()-started));
    struct sbk_rdma_endpoint peer;
    exchange_bytes(sock,&setup.local,sizeof(setup.local),1); exchange_bytes(sock,&peer,sizeof(peer),0);
    CHECK(!ioctl(root,SBK_IOC_RDMA_CONNECT,&peer));
    struct sbk_rdma_region region={0}; struct fixture f={.n=n,.fd=-1}; char cmd;
    if(source) {
        f.src=mmap(NULL,n*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0); CHECK(f.src!=MAP_FAILED);
        for(size_t i=0;i<n;i++) for(size_t b=0;b<P;b++) f.src[i*P+b]=pattern(i,b);
        region=(struct sbk_rdma_region){.address=(uintptr_t)f.src,.pages=n};
        started=now_raw(); CHECK(!ioctl(root,SBK_IOC_EXPORT_REGION,&region));
        printf("SBK_DMA_EXPORT dma=%d phase=initial pages=%zu elapsed_ns=%llu\n",dma,n,(unsigned long long)(now_raw()-started));
        dma_peer_region(sock,root,&region,1,dma);
        if(!probe) {
            exchange_bytes(sock,&cmd,1,0); CHECK(cmd=='P');
            f.src[2*P]^=0x6b;
            region=(struct sbk_rdma_region){.address=(uintptr_t)f.src,.pages=n};
            started=now_raw(); CHECK(!ioctl(root,SBK_IOC_EXPORT_REGION,&region));
            printf("SBK_DMA_EXPORT dma=%d phase=final pages=%zu elapsed_ns=%llu\n",dma,n,(unsigned long long)(now_raw()-started));
            dma_peer_region(sock,root,&region,1,dma);
        }
        exchange_bytes(sock,&cmd,1,0); CHECK(cmd=='D');
        CHECK(!ioctl(root,SBK_IOC_REVOKE_SOURCE)); CHECK(!close(root));
        CHECK(!munmap(f.src,n*P));
        printf("SBK_DMA_PEER_SOURCE_PASS dma=%d probe=%d\n",dma,probe);
    } else {
        dma_peer_region(sock,root,&region,0,dma); f=dma_view(root,region,!probe);
        if(!probe) {
            uint64_t pre[]={0,2,5},dirty[]={2};
            struct sbk_hot_list list={.indices=(uintptr_t)pre,.count=3};
            CHECK(!ioctl(f.fd,SBK_IOC_PRETRANSFER,&list));
            cmd='P'; exchange_bytes(sock,&cmd,1,1);
            dma_peer_region(sock,root,&region,0,dma);
            struct sbk_region_seal seal={.remote=region,.dirty={.indices=(uintptr_t)dirty,.count=1}};
            CHECK(!ioctl(f.fd,SBK_IOC_SEAL_REGION,&seal));
        }
        f.dst=map_destination(f.fd,n); CHECK(f.dst!=MAP_FAILED);
        if(probe) fault_probe(&f);
        else {
            CHECK(f.dst[200*P]==pattern(200,0)); await_completed(f.fd,7);
            uint64_t hot[]={800,801,802,803}; background(f.fd,hot,4); await_completed(f.fd,n);
            for(size_t i=0;i<n;i++) for(size_t b=0;b<P;b++)
                CHECK(f.dst[i*P+b]==(unsigned char)(pattern(i,b)^((i==2 && b==0)?0x6b:0)));
            struct sbk_stats s=stats(f.fd);
            CHECK(!s.errors && s.pretransferred==3 && s.invalidated==1);
            CHECK(s.fetched[0] && s.fetched[1]>=4 && s.fetched[2] && s.completed==n);
            CHECK(s.fetched[0]+s.fetched[1]+s.fetched[2]+2==n);
            printf("SBK_DMA_PEER_PATHS PS=3 invalid=1 PF=%llu FT=%llu BG=%llu pages=%zu errors=0\n",
                (unsigned long long)s.fetched[0],(unsigned long long)s.fetched[1],(unsigned long long)s.fetched[2],n);
        }
        CHECK(!munmap(f.dst,n*P)); CHECK(!close(f.fd)); CHECK(!close(root));
        cmd='D'; exchange_bytes(sock,&cmd,1,1);
        printf("SBK_DMA_PEER_DESTINATION_PASS dma=%d probe=%d\n",dma,probe);
    }
    close(sock); return 0;
}
