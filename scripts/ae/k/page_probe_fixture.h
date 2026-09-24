/* Diagnostic workload: real accesses and mincore observations, no synthetic
 * latency. Intended for one static memory child, not dynamic/COW fixtures. */
static bool page_probe_done;
static const char *page_probe_root;
static char page_probe_target[512];
static uint64_t page_probe_bad, page_probe_reads;
struct page_probe_row { uint64_t address, begin, end, group, position, resident, bad; };
static uint64_t page_probe_clock(void)
{
    struct timespec t;
    if(clock_gettime(CLOCK_MONOTONIC,&t))fail("probe clock");
    return (uint64_t)t.tv_sec*1000000000+t.tv_nsec;
}
static uint64_t page_probe_gcd(uint64_t a,uint64_t b)
{ while(b){uint64_t r=a%b;a=b;b=r;}return a; }
static void *page_probe_run(void *unused)
{
    (void)unused;
    while(access(page_probe_target,F_OK))usleep(1000);
    __atomic_store_n(&pause_work,true,__ATOMIC_RELEASE);
    while(__atomic_load_n(&paused,__ATOMIC_ACQUIRE)!=workers)spin();
    const char *trace_tag=getenv("SB_AE_TRACE_TAG");
    if(trace_tag && (strlen(trace_tag)>15 || pthread_setname_np(pthread_self(),trace_tag)))fail("probe thread tag");
    uint64_t groups=pages/5,count=groups*5,stride=17,bad=0,next=0;
    while(page_probe_gcd(stride,groups)!=1)stride+=2;
    size_t size=count*sizeof(struct page_probe_row);
    struct page_probe_row *rows=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);
    if(rows==MAP_FAILED)fail("probe rows");
    /* No trace-buffer page faults or allocation work inside a measured read. */
    memset(rows,0,size);
    const int offsets[5]={0,1,2,-1,-2};
    for(uint64_t j=0;j<groups;j++) {
        uint64_t group=groups-1-(j*stride)%groups,center=group*5+2;
        for(unsigned position=0;position<5;position++) {
            uint64_t page=center+offsets[position],address=ADDRESS+page*4096;
            unsigned char resident;
            if(mincore((void*)address,4096,&resident))fail("probe mincore");
            uint64_t expected=pattern(page,1),begin=page_probe_clock();
            uint64_t value=*(volatile uint64_t *)(address+sizeof(uint64_t));
            uint64_t end=page_probe_clock();
            bool wrong=value!=expected;bad+=wrong;
            rows[next++]=(struct page_probe_row){address,begin,end,j,position,resident&1,wrong};
        }
    }
    char filename[640];snprintf(filename,sizeof(filename),"%s/probe-%d.csv",page_probe_root,getpid());
    FILE *out=fopen(filename,"wx");if(!out)fail("probe output");
    fprintf(out,"origin_pid,address,before_ns,after_ns,group,position,resident_before,bad\n");
    for(uint64_t i=0;i<count;i++) {
        struct page_probe_row *r=&rows[i];
        fprintf(out,"%d,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
                getpid(),r->address,r->begin,r->end,r->group,r->position,r->resident,r->bad);
    }
    if(fclose(out))fail("probe close");
    munmap(rows,size);page_probe_bad=bad;page_probe_reads=count;
    __atomic_store_n(&pause_work,false,__ATOMIC_RELEASE);
    while(__atomic_load_n(&paused,__ATOMIC_ACQUIRE))spin();
    __atomic_store_n(&page_probe_done,true,__ATOMIC_RELEASE);
    return NULL;
}
