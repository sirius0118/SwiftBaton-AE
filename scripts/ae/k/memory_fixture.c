#define _GNU_SOURCE
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define PORT 16520
#define ADDRESS UINT64_C(0x500000000000)
#define MAX_WORKERS 16
static uint64_t *memory, pages, seed, operations[MAX_WORKERS];
static unsigned workers, paused;
static bool pause_work;
static bool page_probe_enabled;
static uint64_t corrupt;
static void fail(const char *why) { perror(why); exit(1); }
#include "fd_state_fixture.h"
static void spin(void) { __asm__ volatile("pause" ::: "memory"); }
static uint64_t mix(uint64_t x) { x ^= x >> 30; x *= UINT64_C(0xbf58476d1ce4e5b9); x ^= x >> 27; x *= UINT64_C(0x94d049bb133111eb); return x ^ (x >> 31); }
static uint64_t pattern(uint64_t page, unsigned word) { return mix(seed ^ (page * 512 + word)); }
#define DYNAMIC_ADDRESS UINT64_C(0x520000000000)
#define MOVED_ADDRESS UINT64_C(0x530000000000)
#define DYNAMIC_PAGES 4096UL
#define QUARTER (DYNAMIC_PAGES / 4)
static uint64_t *dynamic_memory;
static bool dynamic_enabled, dynamic_done, dynamic_fork_ok;
static uint64_t dynamic_bad;
static uint64_t dynamic_pattern(uint64_t p, unsigned w){return pattern(p+UINT64_C(0x1000000),w);}
static uint64_t verify_dynamic(bool after, bool fork_child)
{
    uint64_t bad=0;
    for(uint64_t page=0;page<DYNAMIC_PAGES;page++) {
        uint64_t *data=(uint64_t *)(DYNAMIC_ADDRESS+page*4096);
        if(after&&page>=2*QUARTER&&page<3*QUARTER)
            data=(uint64_t *)(MOVED_ADDRESS+(page-2*QUARTER)*4096);
        for(unsigned word=1;word<512;word++) {
            uint64_t expected=dynamic_pattern(page,word);
            if(after&&page<QUARTER)expected=UINT64_C(0x345aef19);
            else if(after&&page<2*QUARTER)expected=UINT64_C(0x765efb28);
            else if(after&&!fork_child&&page==3*QUARTER&&word==1)expected=UINT64_C(0xabcdef1298);
            bad+=data[word]!=expected;
        }
    }
    return bad;
}
static void *change_after_restore(void *unused)
{
    uint64_t epoch=0,bad=0;
    (void)unused;
    /* This test-only bind-mounted marker exists exclusively on knode3. It
     * triggers real application syscalls after the restored threads run. */
    while(access("/ae-control/target",F_OK)) {
        for(uint64_t p=0;p<DYNAMIC_PAGES;p++)dynamic_memory[p*512]=++epoch;
        usleep(1000);
    }
    if(madvise((void*)DYNAMIC_ADDRESS,QUARTER*4096,MADV_DONTNEED))fail("dynamic discard");
    for(uint64_t i=0;i<QUARTER*512;i++){bad+=dynamic_memory[i]!=0;dynamic_memory[i]=UINT64_C(0x345aef19);}
    void *second=(void*)(DYNAMIC_ADDRESS+QUARTER*4096);
    if(munmap(second,QUARTER*4096))fail("dynamic unmap");
    if(mmap(second,QUARTER*4096,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0)!=second)fail("dynamic replace");
    for(uint64_t i=0;i<QUARTER*512;i++)((uint64_t*)second)[i]=UINT64_C(0x765efb28);
    uint64_t from=DYNAMIC_ADDRESS+2*QUARTER*4096;
    if(mremap((void*)from,QUARTER*4096,QUARTER*4096,MREMAP_MAYMOVE|MREMAP_FIXED,(void*)MOVED_ADDRESS)!=(void*)MOVED_ADDRESS)fail("dynamic remap");
    int pair[2],status;char value='R';
    if(socketpair(AF_UNIX,SOCK_SEQPACKET|SOCK_CLOEXEC,0,pair))fail("dynamic fork channel");
    pid_t descendant=fork();if(descendant<0)fail("dynamic fork");
    if(!descendant){
        close(pair[0]);
        if(read(pair[1],&value,1)!=1)_exit(2);
        uint64_t failures=verify_dynamic(true,true);
        value=failures?'N':'Y';
        if(write(pair[1],&value,1)!=1)_exit(3);
        _exit(failures?1:0);
    }
    close(pair[1]);
    dynamic_memory[3*QUARTER*512+1]=UINT64_C(0xabcdef1298);
    if(write(pair[0],&value,1)!=1||read(pair[0],&value,1)!=1)fail("dynamic child response");
    close(pair[0]);
    if(waitpid(descendant,&status,0)!=descendant)fail("dynamic child wait");
    dynamic_fork_ok=value=='Y'&&WIFEXITED(status)&&WEXITSTATUS(status)==0;
    bad+=verify_dynamic(true,false);
    dynamic_bad=bad;
    __atomic_store_n(&dynamic_done,true,__ATOMIC_RELEASE);
    return NULL;
}

static void *hammer(void *arg)
{
    unsigned id = (uintptr_t)arg;
    uint64_t random = mix(seed + id), own_pages = (pages - 1 - id) / workers + 1;
    for (;;) {
        if (__atomic_load_n(&pause_work, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_add(&paused, 1, __ATOMIC_RELEASE);
            while (__atomic_load_n(&pause_work, __ATOMIC_ACQUIRE)) spin();
            __atomic_fetch_sub(&paused, 1, __ATOMIC_RELEASE);
        }
        random ^= random << 13; random ^= random >> 7; random ^= random << 17;
        uint64_t hot = own_pages < 128 ? own_pages : 128;
        uint64_t local = random % 10 < 9 ? (page_probe_enabled ? own_pages - hot : 0) + random % hot : random % own_pages;
        uint64_t page = local * workers + id;
        unsigned word = 1 + (random >> 17) % 511;
        if (memory[page * 512 + word] != pattern(page, word))
            __atomic_fetch_add(&corrupt, 1, __ATOMIC_RELAXED);
        memory[page * 512]++;
        operations[id]++;
    }
    return NULL;
}
#include "page_probe_fixture.h"
static void serve(unsigned child, unsigned mib, unsigned threads, uint64_t base_seed, int ready)
{
    pthread_t pool[MAX_WORKERS];
    workers = threads; seed = mix(base_seed + child); pages = (uint64_t)mib * 256;
    memory = mmap((void *)ADDRESS, pages * 4096, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (memory == MAP_FAILED) fail("fixed memory map");
    if (madvise(memory, pages * 4096, MADV_NOHUGEPAGE)) fail("no huge pages");
    for (uint64_t page = 0; page < pages; page++)
        for (unsigned word = 1; word < 512; word++) memory[page * 512 + word] = pattern(page, word);
    /* Fork before any worker threads exist. These same-executable, same-VA
     * parent/child mappings exercise actual restore COW chains. Each process
     * then changes its own counters while retaining the inherited payload. */
    const char *cow = getenv("SB_AE_COW_DESCENDANTS");
    if (cow) {
        unsigned original_children = strtoul(cow, NULL, 10);
        int started[2]; char value;
        if (!original_children || original_children > 8 || pipe(started)) fail("COW child setup");
        pid_t descendant = fork(); if (descendant < 0) fail("COW child fork");
        if (!descendant) {
            close(started[0]); close(ready); ready = started[1];
            child += original_children;
        } else {
            close(started[1]);
            if (read(started[0], &value, 1) != 1 || value != 'R') fail("COW child startup");
            close(started[0]);
        }
    }
    page_probe_enabled=getenv("SB_AE_PAGE_PROBE")!=NULL;
    page_probe_root=getenv("SB_AE_PROBE_CONTROL");
    if(!page_probe_root)page_probe_root="/ae-control";
    if(snprintf(page_probe_target,sizeof(page_probe_target),"%s/target",page_probe_root)>=(int)sizeof(page_probe_target))fail("probe path");
    fd_state_init();
    struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(PORT + child) };
    dynamic_enabled = getenv("SB_AE_DYNAMIC") != NULL;
    if(dynamic_enabled) {
        dynamic_memory=mmap((void*)DYNAMIC_ADDRESS,DYNAMIC_PAGES*4096,PROT_READ|PROT_WRITE,
                            MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        if(dynamic_memory==MAP_FAILED||madvise(dynamic_memory,DYNAMIC_PAGES*4096,MADV_NOHUGEPAGE))fail("dynamic map");
        for(uint64_t page=0;page<DYNAMIC_PAGES;page++)
            for(unsigned word=1;word<512;word++)dynamic_memory[page*512+word]=dynamic_pattern(page,word);
        pthread_t changing;
        if(pthread_create(&changing,NULL,change_after_restore,NULL))fail("dynamic worker");
    }
    int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), yes = 1;
    if (listener < 0 || setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
        bind(listener, (void *)&address, sizeof(address)) || listen(listener, 8)) fail("listen");
    for (unsigned i = 0; i < workers; i++) if (pthread_create(&pool[i], NULL, hammer, (void *)(uintptr_t)i)) fail("worker");
    if(page_probe_enabled) {
        pthread_t probe;
        if(pthread_create(&probe,NULL,page_probe_run,NULL))fail("probe worker");
    }
    if (write(ready, "R", 1) != 1) fail("ready");
    close(ready);
    for (;;) {
        int client = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
        char command, response[1024];
        if (client < 0) { if (errno == EINTR) continue; fail("accept"); }
        if (read(client, &command, 1) != 1) { close(client); continue; }
        if(page_probe_enabled&&!access(page_probe_target,F_OK)) {
            for(unsigned tries=0;!__atomic_load_n(&page_probe_done,__ATOMIC_ACQUIRE)&&tries<30000;tries++)usleep(1000);
            if(!__atomic_load_n(&page_probe_done,__ATOMIC_ACQUIRE))fail("probe completion timeout");
        }
        __atomic_store_n(&pause_work, true, __ATOMIC_RELEASE);
        while (__atomic_load_n(&paused, __ATOMIC_ACQUIRE) != workers) spin();
        uint64_t sum = 0, total = 0, bad = __atomic_load_n(&corrupt, __ATOMIC_RELAXED);
        for (uint64_t page = 0; page < pages; page++) {
            sum += memory[page * 512];
            for (unsigned word = 1; word < 512; word++)
                bad += memory[page * 512 + word] != pattern(page, word);
        }
        for (unsigned i = 0; i < workers; i++) total += operations[i];
        bool after=dynamic_enabled&&!access("/ae-control/target",F_OK);
        if(after) {
            for(unsigned tries=0;!__atomic_load_n(&dynamic_done,__ATOMIC_ACQUIRE)&&tries<30000;tries++)usleep(1000);
            if(!__atomic_load_n(&dynamic_done,__ATOMIC_ACQUIRE))fail("dynamic completion timeout");
        }
        uint64_t dynamic_errors=dynamic_enabled?verify_dynamic(after,false)+(after?dynamic_bad:0):0;
        uint64_t fd_bad = fd_state_verify();
        bool ok = !page_probe_bad && !fd_bad && !bad && sum == total && !dynamic_errors && (!after||dynamic_fork_ok);
        int length = snprintf(response, sizeof(response), "{\"child\":%u,\"pid\":%d,\"parent_pid\":%d,\"address\":\"0x%" PRIx64 "\",\"bytes\":%" PRIu64 ",\"operations\":%" PRIu64 ",\"counter_sum\":%" PRIu64 ",\"bad_words\":%" PRIu64 ",\"ok\":%s}\n", child, getpid(), getppid(), ADDRESS, pages * 4096, total, sum, bad, ok ? "true" : "false");
        if(page_probe_enabled) {
            length-=2;
            length+=snprintf(response+length,sizeof(response)-length,
                ",\"probe_done\":%s,\"probe_reads\":%" PRIu64 ",\"probe_bad\":%" PRIu64 "}\n",
                __atomic_load_n(&page_probe_done,__ATOMIC_ACQUIRE)?"true":"false",page_probe_reads,page_probe_bad);
        }
        if(fd_group_count) {
            length-=2;
            length+=snprintf(response+length,sizeof(response)-length,
                ",\"fd_groups\":%u,\"fd_descriptors\":%u,\"fd_bad\":%" PRIu64 ",\"fd_churn_epoch\":%" PRIu64 "}\n",
                fd_group_count,15*fd_group_count+2,fd_bad,fd_verified_epoch);
            length-=2;
            length+=snprintf(response+length,sizeof(response)-length,
                ",\"fd_semaphore\":%s,\"fd_queued_udp\":%s}\n",
                fd_adversarial?"true":"false",fd_adversarial?"true":"false");
        }
        if(dynamic_enabled) {
            length-=2;
            length+=snprintf(response+length,sizeof(response)-length,
                ",\"dynamic_bytes\":%lu,\"dynamic_done\":%s,\"dynamic_bad_words\":%" PRIu64 ",\"dynamic_fork_ok\":%s}\n",
                DYNAMIC_PAGES*4096,after?"true":"false",dynamic_errors,dynamic_fork_ok?"true":"false");
        }
        __atomic_store_n(&pause_work, false, __ATOMIC_RELEASE);
        while (__atomic_load_n(&paused, __ATOMIC_ACQUIRE)) spin();
        if (send(client, response, length, MSG_NOSIGNAL) != length) perror("response");
        close(client);
    }
}
static int check(unsigned children)
{
    for (unsigned child = 0; child < children; child++) {
        struct sockaddr_in address = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK), .sin_port = htons(PORT + child) };
        struct timeval timeout = {.tv_sec = 30};
        char response[1024]; size_t received = 0;
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) ||
            connect(fd, (void *)&address, sizeof(address)) || send(fd, "V", 1, MSG_NOSIGNAL) != 1) fail("query");
        while (received < sizeof(response) - 1) {
            ssize_t n = read(fd, response + received, sizeof(response) - 1 - received);
            if (n < 0) fail("query read");
            if (!n) break;
            received += n;
        }
        close(fd); response[received] = 0; fputs(response, stdout);
        if (!strstr(response, "\"ok\":true")) return 1;
    }
    return 0;
}
int main(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "check")) return check(atoi(argv[2]));
    if (argc < 8 || strcmp(argv[1], "launch") || strcmp(argv[6], "--")) {
        fprintf(stderr, "Usage: %s launch CHILDREN MIB WORKERS SEED -- COMMAND ... | check CHILDREN\n", argv[0]); return 2;
    }
    unsigned children = atoi(argv[2]), mib = atoi(argv[3]), threads = atoi(argv[4]);
    uint64_t base_seed = strtoull(argv[5], NULL, 0);
    if (!children || children > 8 || !mib || mib > 1024 || !threads || threads > MAX_WORKERS) return 2;
    for (unsigned child = 0; child < children; child++) {
        int ready[2]; char value;
        if (pipe(ready)) fail("ready pipe");
        pid_t pid = fork();
        if (pid < 0) fail("fork");
        if (!pid) { close(ready[0]); serve(child, mib, threads, base_seed, ready[1]); _exit(1); }
        close(ready[1]);
        if (read(ready[0], &value, 1) != 1 || value != 'R') fail("child startup");
        close(ready[0]);
    }
    execvp(argv[7], argv + 7); fail("exec"); return 1;
}
