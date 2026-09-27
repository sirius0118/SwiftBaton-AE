/* Hundreds of jobs share a fixed transport worker budget. Real RXE data path;
 * region cancellation must not flush or destroy its siblings' shared pools. */
static struct sbk_dispatch_stats dispatch_idle(int fd)
{
    struct sbk_dispatch_stats s;
    for (unsigned n=0;n<10000;n++) {
        CHECK(!ioctl(fd,SBK_IOC_DISPATCH_STATS,&s));
        if (!s.lane[0].active&&!s.lane[0].queued&&!s.lane[1].active&&!s.lane[1].queued) {
            for (unsigned l=0;l<2;l++) {
                CHECK(s.lane[l].workers && s.lane[l].peak<=s.lane[l].workers);
                CHECK(s.lane[l].submitted==s.lane[l].completed);
                CHECK(s.lane[l].quanta>=s.lane[l].completed);
            }
            return s;
        }
        usleep(1000);
    }
    CHECK(!"session worker drain timeout");
    return s;
}
static void test_session_dispatch(void)
{
    enum { REGIONS=128, NPAGES=128, LARGE=8192 };
    struct fixture f[REGIONS];
    struct sbk_rdma_setup source={.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
        .timeout_ms=2000,.slots={2,2,2}}, target;
    struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
        .prefetch_workers=4,.background_workers=4,.batch_pages=8,
        .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
    const char *gid=getenv("SBK_GID_INDEX"),*dev=getenv("SBK_RDMA_DEVICE");
    int src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    int dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
    struct sbk_capabilities caps;
    CHECK(src>=0&&dst>=0&&!ioctl(dst,SBK_IOC_CAPABILITIES,&caps));
    if (!(caps.features&SBK_FEATURE_SESSION_DISPATCH)) {
        close(src);close(dst);puts("SKIP session_dispatch disabled");return;
    }
    if(gid)source.gid_index=atoi(gid);
    snprintf(source.device,sizeof(source.device),"%s",dev?dev:"rxe0");
    target=source;target.role=SBK_RDMA_DESTINATION;
    CHECK(!ioctl(src,SBK_IOC_RDMA_CREATE,&source));
    CHECK(!ioctl(dst,SBK_IOC_RDMA_CREATE,&target));
    CHECK(!ioctl(src,SBK_IOC_RDMA_CONNECT,&target.local));
    CHECK(!ioctl(dst,SBK_IOC_RDMA_CONNECT,&source.local));
    for(unsigned r=0;r<REGIONS;r++) {
        size_t pages=r?NPAGES:LARGE;
        f[r]=(struct fixture){.n=pages,.fd=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC)};
        f[r].src=mmap(NULL,pages*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(f[r].fd>=0&&f[r].src!=MAP_FAILED);
        for(size_t i=0;i<pages;i++) memset(f[r].src+i*P,pattern(r,i),P);
        struct sbk_rdma_region region={.address=(uintptr_t)f[r].src,.pages=pages};
        CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&region));
        struct sbk_region_bind bind={.session_fd=dst,.remote=region};
        CHECK(!ioctl(f[r].fd,SBK_IOC_BIND_REGION,&bind));
        cfg.pages=pages;CHECK(!ioctl(f[r].fd,SBK_IOC_CONFIG,&cfg));
        /* Exercise PS on the same dispatcher before any VMA is exposed. */
        if(r==1) {
            uint64_t ids[NPAGES];for(unsigned i=0;i<NPAGES;i++)ids[i]=i;
            struct sbk_hot_list ps={.indices=(uintptr_t)ids,.count=NPAGES};
            CHECK(!ioctl(f[r].fd,SBK_IOC_PRETRANSFER_MANY,&ps));
            ps.count=0;CHECK(!ioctl(f[r].fd,SBK_IOC_SEAL,&ps));
        }
        f[r].dst=map_destination(f[r].fd,pages);CHECK(f[r].dst!=MAP_FAILED);
    }
    /* PF+FT run independently before the BG backlog starts. */
    CHECK(f[2].dst[64*P]==f[2].src[64*P]);
    await_completed(f[2].fd,5);
    for(unsigned r=0;r<REGIONS;r++) {
        uint64_t hot[]={100,101,102,103,104,105,106,107};
        background(f[r].fd,hot,8);
    }
    CHECK(!ioctl(f[0].fd,SBK_IOC_CANCEL));
    CHECK(!munmap(f[0].dst,f[0].n*P)&&!close(f[0].fd));
    f[0].dst=NULL;f[0].fd=-1;
    /* Force a later region to finish while the large cancelled owner reaps. */
    CHECK(!memcmp(f[REGIONS-1].dst,f[REGIONS-1].src,NPAGES*P));
    for(unsigned r=1;r<REGIONS;r++) {
        CHECK(!memcmp(f[r].dst,f[r].src,f[r].n*P));
        struct sbk_stats s=stats(f[r].fd);
        CHECK(!s.errors&&s.completed==f[r].n);
    }
    struct sbk_dispatch_stats d=dispatch_idle(dst);
    CHECK(d.lane[0].workers==2&&d.lane[1].workers==2);
    CHECK(d.lane[0].submitted>=4&&d.lane[1].submitted>=REGIONS);
    CHECK(d.lane[1].quanta>d.lane[1].completed); /* At least one bounded yield. */
    CHECK(d.lane[1].queue_peak>d.lane[1].workers); /* Actual contention, not idle cap. */
    for(unsigned l=0;l<2;l++)
        printf("PASS session_dispatch lane=%u regions=%u workers=%u peak=%u submitted=%llu quanta=%llu completed=%llu queue_peak=%llu\n",
            l,REGIONS,d.lane[l].workers,d.lane[l].peak,
            (unsigned long long)d.lane[l].submitted,(unsigned long long)d.lane[l].quanta,
            (unsigned long long)d.lane[l].completed,(unsigned long long)d.lane[l].queue_peak);
    for(unsigned r=0;r<REGIONS;r++) finish(&f[r]);
    CHECK(!close(dst)&&!ioctl(src,SBK_IOC_REVOKE_SOURCE)&&!close(src));
    puts("PASS session_dispatch_many_regions_PS_PF_FT_BG_cancel_sibling_and_join");
}
