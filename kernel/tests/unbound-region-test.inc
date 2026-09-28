/* Real RXE: target allocation/PTE preparation precedes source MR existence. */
static void test_unbound_regions(void)
{
    for (int dma = 0; dma <= 1; dma++) {
        enum { N = 128 };
        int src, dst;
        if (dma) dma_pair(&src, &dst);
        else {
            struct sbk_rdma_setup a = {.role=SBK_RDMA_SOURCE,.port=1,.gid_index=1,
                .timeout_ms=1000,.slots={2,2,2}}, b;
            snprintf(a.device,sizeof(a.device),"%s",getenv("SBK_RDMA_DEVICE")?getenv("SBK_RDMA_DEVICE"):"rxe0");
            if(getenv("SBK_GID_INDEX")) a.gid_index=atoi(getenv("SBK_GID_INDEX"));
            b=a; b.role=SBK_RDMA_DESTINATION;
            src=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC); dst=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
            CHECK(src>=0&&dst>=0&&!ioctl(src,SBK_IOC_RDMA_CREATE,&a)&&!ioctl(dst,SBK_IOC_RDMA_CREATE,&b));
            CHECK(!ioctl(src,SBK_IOC_RDMA_CONNECT,&b.local)&&!ioctl(dst,SBK_IOC_RDMA_CONNECT,&a.local));
        }
        struct sbk_capabilities caps; CHECK(!ioctl(dst,SBK_IOC_CAPABILITIES,&caps));
        CHECK((caps.features&(SBK_FEATURE_UNBOUND_REGION|SBK_FEATURE_PREPARED_ARM))==
              (SBK_FEATURE_UNBOUND_REGION|SBK_FEATURE_PREPARED_ARM));
        int view=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC), orphan=open("/dev/swiftbaton_k",O_RDWR|O_CLOEXEC);
        CHECK(view>=0&&orphan>=0);
        CHECK(ioctl(view,SBK_IOC_BIND_UNBOUND,&view)<0&&errno==EINVAL);
        CHECK(ioctl(view,SBK_IOC_BIND_UNBOUND,&src)<0&&errno==EINVAL);
        CHECK(!ioctl(orphan,SBK_IOC_BIND_UNBOUND,&dst));
        CHECK(!ioctl(orphan,SBK_IOC_CANCEL)&&!close(orphan));
        CHECK(!ioctl(view,SBK_IOC_BIND_UNBOUND,&dst)); /* Canceled sibling did not stop QPs. */
        CHECK(ioctl(view,SBK_IOC_BIND_UNBOUND,&dst)<0&&errno==EBUSY);
        CHECK(ioctl(view,SBK_IOC_DMA_ENABLE)<0&&errno==EBUSY);
        struct sbk_rdma_region bad_export={.address=0,.pages=1};
        CHECK(ioctl(view,SBK_IOC_EXPORT_REGION,&bad_export)<0&&errno==EINVAL);
        struct sbk_config cfg={.version=SBK_ABI_VERSION,.backend=SBK_BACKEND_RDMA,
            .pages=N,.prefetch_workers=2,.background_workers=2,.batch_pages=8,
            .prefetch_enabled=1,.test_fail_page=SBK_NO_FAILURE};
        CHECK(!ioctl(view,SBK_IOC_CONFIG,&cfg));
        uint64_t reserve=N; CHECK(!ioctl(dst,SBK_IOC_TOKEN_RESERVE,&reserve));
        unsigned char *target=mmap(NULL,N*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        unsigned char *data=mmap(NULL,N*P,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
        CHECK(target!=MAP_FAILED&&data!=MAP_FAILED);
        for(size_t i=0;i<N;i++)for(size_t j=0;j<P;j++)data[i*P+j]=pattern(i,j);
        struct sbk_anon_arm arm={.address=(uintptr_t)target};
        CHECK(!ioctl(view,SBK_IOC_PREPARE_ANON,&arm));
        struct sbk_hot_list none={0};
        CHECK(ioctl(view,SBK_IOC_PRETRANSFER,&none)<0&&errno==EINVAL);
        CHECK(ioctl(view,SBK_IOC_PRETRANSFER_MANY,&none)<0&&errno==EINVAL);
        CHECK(ioctl(view,SBK_IOC_SEAL,&none)<0&&errno==EINVAL);
        CHECK(ioctl(view,SBK_IOC_ARM_ANON,&arm)<0&&errno==EINVAL);
        CHECK(mmap(NULL,N*P,PROT_READ,MAP_PRIVATE,view,0)==MAP_FAILED);
        struct sbk_rdma_region ps={.address=(uintptr_t)data,.pages=N};
        CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&ps));
        if(dma) dma_exchange(src,dst,&ps,0);
        struct fixture cache=dma_view(dst,ps,1);
        uint64_t selected[]={0,2,5},dirty[]={2};
        struct sbk_hot_list list={.indices=(uintptr_t)selected,.count=3};
        CHECK(!ioctl(cache.fd,SBK_IOC_PRETRANSFER,&list));
        struct sbk_ps_slice slice={.source_fd=cache.fd,.pages=N};
        CHECK(!ioctl(view,SBK_IOC_IMPORT_PS,&slice));
        CHECK(stats(view).pretransferred==3&&stats(cache.fd).completed==0);
        data[2*P]^=0x69;
        struct sbk_rdma_region final={.address=(uintptr_t)data,.pages=N};
        CHECK(!ioctl(src,SBK_IOC_EXPORT_REGION,&final));
        struct sbk_region_seal seal={.remote=final,.dirty={.indices=(uintptr_t)dirty,.count=1}};
        if(dma) {
            CHECK(ioctl(view,SBK_IOC_SEAL_REGION,&seal)<0&&errno==EINVAL);
            dma_exchange(src,dst,&final,0);
        }
        seal.remote.pages--;
        CHECK(ioctl(view,SBK_IOC_SEAL_REGION,&seal)<0&&errno==EINVAL);
        seal.remote=final;
        CHECK(!ioctl(view,SBK_IOC_SEAL_REGION,&seal));
        CHECK(!ioctl(view,SBK_IOC_ARM_ANON,&arm));
        CHECK(!close(cache.fd)&&!close(dst));
        CHECK(target[32*P]==data[32*P]); await_completed(view,7);
        uint64_t hot[]={50,51,52,53}; background(view,hot,4); await_completed(view,N);
        CHECK(!memcmp(target,data,N*P));
        struct sbk_stats st=stats(view);
        CHECK(!st.errors&&st.pretransferred==3&&st.invalidated==1&&st.fetched[0]>0&&st.fetched[1]>=4&&st.fetched[2]>0);
        CHECK(st.fetched[0]+st.fetched[1]+st.fetched[2]+2==N);
        CHECK(!munmap(target,N*P)&&!close(view));
        CHECK(!ioctl(src,SBK_IOC_REVOKE_SOURCE)&&!close(src)&&!munmap(data,N*P));
        printf("PASS UNBOUND_PS_PREPARE_FINAL_BIND dma=%d PS=3 invalid=1 PF=%llu FT=%llu BG=%llu\n",dma,
            (unsigned long long)st.fetched[0],(unsigned long long)st.fetched[1],(unsigned long long)st.fetched[2]);
    }
}
