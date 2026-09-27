// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <pthread.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <poll.h>
#include <arpa/inet.h>
#include "sbk_uapi.h"

#define P 4096UL
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL %s:%d %s errno=%d\n", __func__, __LINE__, #x, errno); exit(1); } } while (0)
struct fixture { int fd; size_t n; unsigned char *src, *dst; };
static int use_stage, use_anon, defer_mapping, use_token_pool;
static int block_speculative_qp = -1;
static unsigned char *map_destination(int fd, size_t n)
{
    unsigned char *p;
    if (defer_mapping) return NULL;
    if (!use_anon) return mmap(NULL, n * P, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    p = mmap(NULL, n * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(p != MAP_FAILED);
    if (use_token_pool) {
        struct sbk_token_pool_stats pool = {0};
        CHECK(!ioctl(fd, SBK_IOC_TOKEN_POOL_STATS, &pool));
        if (!pool.sealed && !pool.prepared) {
            uint64_t pages = n;
            CHECK(!ioctl(fd, SBK_IOC_TOKEN_RESERVE, &pages));
        }
    }
    struct sbk_anon_arm a = {.address = (uintptr_t)p};
    CHECK(ioctl(fd, SBK_IOC_ARM_ANON, &a) == 0);
    return p;
}
static void exercise_stage(struct fixture *f)
{
    uint64_t pre[] = {3, 4, 7, 9}, dirty[] = {4};
    struct sbk_hot_list l = {.indices = (uintptr_t)pre, .count = 4};
    CHECK(ioctl(f->fd, SBK_IOC_PRETRANSFER, &l) == 0);
    CHECK(mmap(NULL, f->n * P, PROT_READ, MAP_PRIVATE, f->fd, 0) == MAP_FAILED);
    /* Controller supplies the page changed after PS; source is now quiescent. */
    f->src[4 * P] ^= 0x5a;
    l.indices = (uintptr_t)dirty; l.count = 1;
    CHECK(ioctl(f->fd, SBK_IOC_SEAL, &l) == 0);
    CHECK(ioctl(f->fd, SBK_IOC_PRETRANSFER, &l) < 0 && errno == EINVAL);
}
static unsigned char pattern(size_t page, size_t byte) { return (page * 37 + byte * 13 + 19) % 251; }
static struct fixture fixture(size_t n, int prefetch, int workers, int delay, uint64_t fail)
{
    struct fixture f = {.n = n};
    struct sbk_config c = {.version = SBK_ABI_VERSION, .backend = SBK_BACKEND_LOOPBACK_TEST,
        .pages = n, .prefetch_workers = 4, .background_workers = workers, .batch_pages = 8,
        .prefetch_enabled = prefetch, .test_fail_page = fail, .test_delay_us = delay};
    f.fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
    CHECK(f.fd >= 0);
    f.src = mmap(NULL, n * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(f.src != MAP_FAILED);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < P; ++j) f.src[i * P + j] = pattern(i, j);
    c.source_address = (uintptr_t)f.src;
    CHECK(ioctl(f.fd, SBK_IOC_CONFIG, &c) == 0);
    if (use_stage) exercise_stage(&f);
    f.dst = map_destination(f.fd, n);
    CHECK(f.dst != MAP_FAILED);
    return f;
}
static void finish(struct fixture *f)
{
    if (f->dst) CHECK(munmap(f->dst, f->n * P) == 0);
    if (f->fd >= 0) CHECK(close(f->fd) == 0);
    CHECK(munmap(f->src, f->n * P) == 0);
}
static struct sbk_stats stats(int fd)
{
    struct sbk_stats s;
    CHECK(ioctl(fd, SBK_IOC_STATS, &s) == 0);
    return s;
}
static void verify(struct fixture *f)
{
    for (size_t i = 0; i < f->n; ++i)
        for (size_t j = 0; j < P; ++j) CHECK(f->dst[i * P + j] == pattern(i, j));
}
static void background(int fd, uint64_t *hot, size_t n)
{
    struct sbk_hot_list l = {.indices = (uintptr_t)hot, .count = n};
    CHECK(ioctl(fd, SBK_IOC_BACKGROUND, &l) == 0);
}
static void await_completed(int fd, size_t n)
{
    for (int i = 0; i < 10000; ++i) {
        if (stats(fd).completed == n) return;
        usleep(1000);
    }
    CHECK(!"completion timeout");
}
static void test_private_cow(void)
{
    struct fixture f = fixture(64, 0, 1, 0, SBK_NO_FAILURE);
    verify(&f);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) {
        verify(&f);
        for (size_t i = 0; i < f.n; ++i) f.dst[i * P] ^= 0xff;
        for (size_t i = 0; i < f.n; ++i) CHECK(f.dst[i * P] == (unsigned char)(pattern(i, 0) ^ 0xff));
        finish(&f);
        _exit(0);
    }
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    verify(&f);
    f.dst[0] = 237;
    CHECK(f.src[0] == pattern(0, 0));
    CHECK(stats(f.fd).completed == 64);
    finish(&f);
    puts("PASS private_fork_cow");
}
struct reader { struct fixture *f; pthread_barrier_t *barrier; size_t index; int all; };
static void *read_worker(void *arg)
{
    struct reader *r = arg;
    pthread_barrier_wait(r->barrier);
    if (r->all) {
        for (size_t k = 0; k < r->f->n; ++k) {
            size_t i = (k * 17 + r->index) % r->f->n;
            CHECK(r->f->dst[i * P] == pattern(i, 0));
        }
    } else CHECK(r->f->dst[r->index * P] == pattern(r->index, 0));
    return NULL;
}
static void readers(struct fixture *f, int all)
{
    pthread_t threads[16]; struct reader args[16]; pthread_barrier_t barrier;
    CHECK(pthread_barrier_init(&barrier, NULL, 16) == 0);
    for (int i = 0; i < 16; ++i) {
        args[i] = (struct reader){.f = f, .barrier = &barrier, .index = all ? i : 7, .all = all};
        CHECK(pthread_create(&threads[i], NULL, read_worker, &args[i]) == 0);
    }
    for (int i = 0; i < 16; ++i) CHECK(pthread_join(threads[i], NULL) == 0);
    pthread_barrier_destroy(&barrier);
}
static void test_concurrency(void)
{
    struct fixture f = fixture(32, 0, 1, 5000, SBK_NO_FAILURE);
    readers(&f, 0);
    struct sbk_stats s = stats(f.fd);
    CHECK(s.completed == 1 && s.fetched[SBK_DEMAND] == 1 && (use_anon || s.waits > 0));
    finish(&f);
    f = fixture(512, 1, 4, 100, SBK_NO_FAILURE);
    uint64_t hot[] = {400, 401, 402, 403, 404, 405, 406, 407};
    background(f.fd, hot, 8);
    readers(&f, 1);
    await_completed(f.fd, f.n); verify(&f);
    s = stats(f.fd);
    CHECK(s.errors == 0 && s.fetched[0] + s.fetched[1] + s.fetched[2] == f.n);
    printf("PASS concurrent_paths fetched=%llu/%llu/%llu waits=%llu\n",
        (unsigned long long)s.fetched[0], (unsigned long long)s.fetched[1],
        (unsigned long long)s.fetched[2], (unsigned long long)s.waits);
    finish(&f);
}
static void test_prefetch(void)
{
    struct fixture f = fixture(32, 1, 1, 500, SBK_NO_FAILURE);
    CHECK(f.dst[16 * P] == pattern(16, 0));
    await_completed(f.fd, 5);
    struct sbk_stats s = stats(f.fd);
    CHECK(s.fetched[SBK_DEMAND] == 1 && s.fetched[SBK_PREFETCH] == 4);
    for (int i = 0; i < 1000 && stats(f.fd).installed_ahead < 4; ++i) usleep(1000);
    s = stats(f.fd);
    CHECK(s.installed_ahead == 4 && s.faults == 1 && s.hits == 0);
    for (size_t i = 14; i <= 18; ++i) CHECK(f.dst[i * P] == pattern(i, 0));
    CHECK(stats(f.fd).faults == 1);
    uint64_t histogram_total = 0;
    for (int i = 0; i < 32; ++i) histogram_total += s.hist_ns_pow2[i];
    CHECK(histogram_total == s.faults && s.fault_ns >= s.fault_max_ns);
    finish(&f);
    puts("PASS neighbor_prefetch_kernel_install");
}
static void test_hot(void)
{
    struct fixture f = fixture(32, 0, 1, 100, SBK_NO_FAILURE);
    uint64_t hot[] = {25, 17, 29, 19, 24, 16, 28, 18};
    background(f.fd, hot, 8);
    await_completed(f.fd, f.n);
    struct sbk_page_info first_cold = {.index = 0}, last_hot = {.index = 18};
    CHECK(ioctl(f.fd, SBK_IOC_PAGE, &first_cold) == 0);
    CHECK(ioctl(f.fd, SBK_IOC_PAGE, &last_hot) == 0);
    CHECK(last_hot.completed_ns <= first_cold.started_ns);
    CHECK(stats(f.fd).fetched[SBK_BACKGROUND] == f.n);
    verify(&f); finish(&f);
    puts("PASS hot_first_background");
}
/* A scheduling-order regression, not a hardware latency measurement. The
 * loopback transport deliberately holds the demand copy for 100 ms so FT
 * workers can prove they start before it completes, without timing thresholds. */
static void test_prefetch_overlaps_demand(void)
{
    struct fixture f = fixture(32, 1, 1, 100000, SBK_NO_FAILURE);
    CHECK(f.dst[16 * P] == pattern(16, 0));
    await_completed(f.fd, 5);
    struct sbk_page_info demand = {.index = 16};
    CHECK(ioctl(f.fd, SBK_IOC_PAGE, &demand) == 0);
    for (size_t i = 14; i <= 18; ++i) {
        if (i == 16) continue;
        struct sbk_page_info neighbor = {.index = i};
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &neighbor) == 0);
        printf("SBK_PREFETCH_OVERLAP page=%zu demand_done_ns=%llu neighbor_start_ns=%llu\n",
            i, (unsigned long long)demand.completed_ns,
            (unsigned long long)neighbor.started_ns);
        CHECK(neighbor.started_ns > 0 && neighbor.started_ns < demand.completed_ns);
        CHECK(f.dst[i * P] == pattern(i, 0));
    }
    struct sbk_stats s = stats(f.fd);
    CHECK(s.fetched[SBK_DEMAND] == 1 && s.fetched[SBK_PREFETCH] == 4 && !s.errors);
    finish(&f);
    puts("PASS prefetch_overlaps_inflight_demand scheduling_only_not_hardware_latency");
}
static void test_demand_does_not_wait_for_batch_tail(void)
{
    struct fixture f = fixture(8, 0, 1, 100000, SBK_NO_FAILURE);
    struct sbk_page_info first = {.index = 0}, tail = {.index = 7};
    background(f.fd, NULL, 0);
    for (int i = 0; i < 1000; ++i) {
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &first) == 0);
        if (first.started_ns) break;
        usleep(1000);
    }
    CHECK(first.started_ns);
    CHECK(*(volatile unsigned char *)f.dst == pattern(0, 0));
    CHECK(ioctl(f.fd, SBK_IOC_PAGE, &first) == 0);
    CHECK(ioctl(f.fd, SBK_IOC_PAGE, &tail) == 0);
    printf("SBK_BATCH_PF_RELEASE first_completed_ns=%llu tail_completed_ns=%llu\n",
        (unsigned long long)first.completed_ns, (unsigned long long)tail.completed_ns);
    CHECK(first.completed_ns && !tail.completed_ns);
    struct sbk_stats s = stats(f.fd);
    CHECK(s.completed > 0 && s.completed < f.n && !s.fetched[SBK_DEMAND]);
    await_completed(f.fd, f.n); verify(&f);
    CHECK(stats(f.fd).fetched[SBK_BACKGROUND] == f.n && !stats(f.fd).errors);
    finish(&f);
    puts("PASS demand_reuses_completed_BG_page_before_batch_tail scheduling_only_not_hardware_latency");
}
static void test_mapping_lifetime(void)
{
    struct fixture f = fixture(64, 1, 2, 500, SBK_NO_FAILURE);
    unsigned char *dest = mmap(NULL, f.n * P, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(dest != MAP_FAILED);
    background(f.fd, NULL, 0);
    CHECK(mremap(f.dst, f.n * P, f.n * P, MREMAP_MAYMOVE | MREMAP_FIXED, dest) == dest);
    f.dst = dest;
    CHECK(mprotect(dest + 16 * P, 16 * P, PROT_READ) == 0);
    verify(&f);
    CHECK(mprotect(dest + 16 * P, 16 * P, PROT_READ | PROT_WRITE) == 0);
    CHECK(close(f.fd) == 0); f.fd = -1;
    CHECK(munmap(dest + 24 * P, 8 * P) == 0);
    unsigned char *replacement = mmap(dest + 24 * P, 8 * P, PROT_READ | PROT_WRITE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    CHECK(replacement == dest + 24 * P);
    memset(replacement, 0xb6, 8 * P);
    usleep(20000);
    for (size_t i = 0; i < 8 * P; ++i) CHECK(replacement[i] == 0xb6);
    CHECK(dest[0] == pattern(0, 0) && dest[63 * P] == pattern(63, 0));
    finish(&f);
    puts("PASS split_move_unmap_close_fd");
}
static void test_guard_background(void)
{
    struct fixture f = fixture(32, 0, 2, 0, SBK_NO_FAILURE);
    CHECK(mprotect(f.dst, f.n * P, PROT_NONE) == 0);
    background(f.fd, NULL, 0);
    await_completed(f.fd, f.n);
    struct sbk_drain_status d;
    for (int i = 0; i < 2000; i++) {
        CHECK(ioctl(f.fd, SBK_IOC_DRAIN_STATUS, &d) == 0);
        if (d.drained) break;
        usleep(1000);
    }
    CHECK(d.drained && d.retired_tokens == f.n);
    pid_t child = fork(); CHECK(child >= 0);
    if (!child) { volatile unsigned char byte = f.dst[0]; (void)byte; _exit(99); }
    int status; CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status)==SIGSEGV);
    CHECK(mprotect(f.dst, f.n * P, PROT_READ|PROT_WRITE) == 0);
    verify(&f);
    CHECK(stats(f.fd).faults == 0 && stats(f.fd).fetched[SBK_BACKGROUND] == f.n);
    finish(&f);
    puts("PASS anonymous_PROT_NONE_background_drain_preserves_protection_and_content");
}
static void expect_sigbus(struct fixture *f, size_t index)
{
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) { volatile unsigned char v = f->dst[index * P]; (void)v; _exit(99); }
    int status; CHECK(waitpid(pid, &status, 0) == pid);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS);
}
static void test_failure_cancel(void)
{
    struct fixture f = fixture(32, 0, 1, 0, 7);
    expect_sigbus(&f, 7);
    CHECK(stats(f.fd).errors == 1);
    finish(&f);
    f = fixture(256, 1, 4, 2000, SBK_NO_FAILURE);
    background(f.fd, NULL, 0);
    CHECK(ioctl(f.fd, SBK_IOC_CANCEL) == 0);
    expect_sigbus(&f, 255);
    finish(&f);
    /* Exit with queued work and inherited unfaulted VMAs. */
    for (int i = 0; i < 40; ++i) {
        pid_t pid = fork(); CHECK(pid >= 0);
        if (!pid) {
            struct fixture child = fixture(128, 1, 4, 1000, SBK_NO_FAILURE);
            background(child.fd, NULL, 0);
            CHECK(child.dst[87 * P] == pattern(87, 0));
            _exit(0);
        }
        int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    }
    puts("PASS error_cancel_process_exit");
}
static void test_invalid(void)
{
    int fd = open("/dev/swiftbaton_k", O_RDWR); CHECK(fd >= 0);
    struct sbk_config c = {0};
    CHECK(ioctl(fd, SBK_IOC_CONFIG, &c) < 0 && errno == EINVAL);
    CHECK(ioctl(fd, 0xdead) < 0 && errno == ENOTTY);
    CHECK(mmap(NULL, P, PROT_READ, MAP_PRIVATE, fd, 0) == MAP_FAILED);
    CHECK(close(fd) == 0);
    struct fixture f = fixture(16, 0, 1, 0, SBK_NO_FAILURE);
    uint64_t duplicate[] = {2, 2}, invalid[] = {16};
    struct sbk_hot_list l = {.indices = (uintptr_t)duplicate, .count = 2};
    CHECK(ioctl(f.fd, SBK_IOC_BACKGROUND, &l) < 0 && errno == EINVAL);
    l.indices = (uintptr_t)invalid; l.count = 1;
    CHECK(ioctl(f.fd, SBK_IOC_BACKGROUND, &l) < 0 && errno == EINVAL);
    l.indices = 0xffffffffffffffffULL;
    CHECK(ioctl(f.fd, SBK_IOC_BACKGROUND, &l) < 0 && errno == EFAULT);
    CHECK(mmap(NULL, 16 * P, PROT_READ, MAP_PRIVATE, f.fd, 0) == MAP_FAILED);
    background(f.fd, NULL, 0); await_completed(f.fd, f.n); verify(&f);
    finish(&f);
    puts("PASS input_validation");
}
static int rdma_source_fd = -1;
static struct fixture rdma_fixture_opts(size_t n, int prefetch, size_t peer_offset_pages,
                                       unsigned batch_pages, unsigned workers)
{
    struct fixture f = {.n = n};
    struct sbk_rdma_setup source = {.role = SBK_RDMA_SOURCE, .port = 1, .gid_index = 1,
        .timeout_ms = 250, .slots = {4, 2, 2}, .pages = n};
    struct sbk_rdma_setup target;
    struct sbk_config c = {.version = SBK_ABI_VERSION, .backend = SBK_BACKEND_RDMA,
        .pages = n, .prefetch_workers = 2, .background_workers = workers, .batch_pages = batch_pages,
        .prefetch_enabled = prefetch, .test_fail_page = SBK_NO_FAILURE};
    const char *gid = getenv("SBK_GID_INDEX"), *device = getenv("SBK_RDMA_DEVICE");
    if (gid) source.gid_index = atoi(gid);
    snprintf(source.device, sizeof(source.device), "%s", device ? device : "rxe0");
    if (block_speculative_qp >= 0) {
        /* Only used in the isolated RXE fixture; no host NIC or real peer. */
        CHECK(!strcmp(source.device, "rxe0"));
        source.slots[block_speculative_qp] = 1;
        if (block_speculative_qp == SBK_PREFETCH) c.prefetch_workers = 4;
    }
    target = source; target.role = SBK_RDMA_DESTINATION;
    f.src = mmap(NULL, n * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(f.src != MAP_FAILED);
    for (size_t i = 0; i < n; ++i)
        for (size_t j = 0; j < P; ++j) f.src[i * P + j] = pattern(i, j);
    source.source_address = (uintptr_t)f.src;
    rdma_source_fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
    f.fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
    CHECK(rdma_source_fd >= 0 && f.fd >= 0);
    CHECK(ioctl(rdma_source_fd, SBK_IOC_RDMA_CREATE, &source) == 0);
    CHECK(ioctl(f.fd, SBK_IOC_RDMA_CREATE, &target) == 0);
    CHECK(ioctl(rdma_source_fd, SBK_IOC_RDMA_CONNECT, &target.local) == 0);
    if (block_speculative_qp >= 0) {
        CHECK(source.local.qpn[block_speculative_qp][0] != 0xffffff);
        source.local.qpn[block_speculative_qp][0] = 0xffffff; /* No responder; PF QPs stay valid. */
    }
    source.local.address += peer_offset_pages * P;
    CHECK(ioctl(f.fd, SBK_IOC_RDMA_CONNECT, &source.local) == 0);
    CHECK(ioctl(f.fd, SBK_IOC_CONFIG, &c) == 0);
    if (use_stage) exercise_stage(&f);
    f.dst = map_destination(f.fd, n);
    CHECK(f.dst != MAP_FAILED);
    return f;
}
static struct fixture rdma_fixture(size_t n)
{
    return rdma_fixture_opts(n, 1, 0, 8, 2);
}
static void test_rdma_admission_steal(int lane)
{
    const char *device = getenv("SBK_RDMA_DEVICE");
    if (device && strcmp(device, "rxe0")) return;
    block_speculative_qp = lane;
    struct fixture f = rdma_fixture_opts(64, lane == SBK_PREFETCH, 0, 8, 8);
    block_speculative_qp = -1;
    const unsigned queued_state = lane == SBK_BACKGROUND ? 1 : 2;
    const unsigned expected_claimed = lane == SBK_BACKGROUND ? 8 : 1;
    const unsigned expected_queued = lane == SBK_BACKGROUND ? 56 : 3;
    if (lane == SBK_BACKGROUND) background(f.fd, NULL, 0);
    else CHECK(f.dst[32*P] == pattern(32, 0)); /* Triggers exactly +/-1,+/-2. */
    unsigned claimed = 0, queued = 0;
    size_t victim = 0;
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        claimed = queued = 0;
        for (size_t i = 0; i < f.n; i++) {
            struct sbk_page_info info = {.index = i};
            CHECK(ioctl(f.fd, SBK_IOC_PAGE, &info) == 0);
            claimed += info.state == 3; /* INFLIGHT; timestamps publish only at completion. */
            if (info.state == queued_state) {
                queued++;
                victim = i;
            }
        }
        if (claimed) break;
        usleep(1000);
    }
    /* Let all workers run while their sole speculative QP cannot complete.
     * The old code claims all candidates before waiting for this QP. */
    usleep(3000);
    claimed = queued = 0;
    for (size_t i = 0; i < f.n; i++) {
        struct sbk_page_info info = {.index = i};
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &info) == 0);
        claimed += info.state == 3;
        if (info.state == queued_state) { queued++; victim = i; }
    }
    printf("ADMISSION_OBSERVED lane=%d claimed=%u queued=%u slot_limit=1\n", lane, claimed, queued);
    CHECK(claimed == expected_claimed && queued == expected_queued && !stats(f.fd).errors);
    /* A real demand RDMA read must succeed while the unrelated QP is stuck.
     * No timing threshold is used as correctness evidence. */
    for (size_t i = 0; i < P; i++) CHECK(f.dst[victim*P+i] == pattern(victim, i));
    struct sbk_stats s = stats(f.fd);
    CHECK(s.fetched[SBK_DEMAND] == (lane == SBK_BACKGROUND ? 1U : 2U) &&
          !s.fetched[SBK_BACKGROUND] && !s.fetched[SBK_PREFETCH] && !s.errors);
    CHECK(ioctl(f.fd, SBK_IOC_CANCEL) == 0);
    finish(&f);
    CHECK(close(rdma_source_fd) == 0);
    rdma_source_fd = -1;
    puts("PASS RDMA_admission_leaves_queued_pages_demand_stealable_and_cancel_releases_slots");
}
static void test_rdma_partial_batch_failure(void)
{
    /* Real RXE protection failure: shifted advertisement leaves the final WR
     * outside its actual MR; the preceding 31 WRs contain known valid pages. */
    struct fixture f = rdma_fixture_opts(32, 0, 1, 32, 1);
    struct sbk_page_info first = {.index=0}, tail = {.index=31};
    background(f.fd, NULL, 0);
    for (int i=0; i<1000; i++) {
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &first)==0);
        if (first.started_ns) break;
        usleep(1000);
    }
    CHECK(first.started_ns);
    CHECK(f.dst[0]==pattern(1,0));
    CHECK(ioctl(f.fd,SBK_IOC_PAGE,&tail)==0);
    int discarded_before_tail = !tail.completed_ns;
    if (use_anon) {
        CHECK(madvise(f.dst,P,MADV_DONTNEED)==0);
        CHECK(f.dst[0]==0);
        memset(f.dst,0xb6,P);
    }
    for (int i=0; i<1000 && !stats(f.fd).errors; i++) usleep(1000);
    struct sbk_stats s=stats(f.fd);
    CHECK(s.completed==31 && s.fetched[SBK_BACKGROUND]==31 && s.errors==1);
    for (size_t i=use_anon?1:0; i<31; i++)
        for (size_t j=0; j<P; j++) CHECK(f.dst[i*P+j]==pattern(i+1,j));
    expect_sigbus(&f,31);
    if (use_anon) for (size_t j=0;j<P;j++) CHECK(f.dst[j]==0xb6);
    printf("PASS RDMA_partial_batch_success_prefix_then_MR_failure completed=31 failed=1 discarded_before_tail=%d\n",
        discarded_before_tail);
    finish(&f);CHECK(close(rdma_source_fd)==0);rdma_source_fd=-1;
}
static void test_stage(int rdma)
{
    use_stage = 1;
    struct fixture f = rdma ? rdma_fixture(48) : fixture(48, 1, 2, 0, SBK_NO_FAILURE);
    use_stage = 0;
    struct sbk_stats s = stats(f.fd);
    CHECK(s.pretransferred == 4 && s.invalidated == 1 && s.completed == 3);
    CHECK(f.dst[4 * P] == f.src[4 * P]);
    background(f.fd, NULL, 0); await_completed(f.fd, f.n);
    CHECK(memcmp(f.dst, f.src, f.n * P) == 0);
    s = stats(f.fd);
    CHECK(s.fetched[0] + s.fetched[1] + s.fetched[2] + s.pretransferred - s.invalidated == f.n);
    finish(&f);
    if (rdma) { CHECK(close(rdma_source_fd) == 0); rdma_source_fd = -1; }
    printf("PASS PS_pretransfer_dirty_invalidation backend=%s\n", rdma ? "RDMA" : "LOOPBACK_TEST");
}
static void test_parallel_stage(int rdma)
{
    defer_mapping = 1;
    struct fixture f = rdma ? rdma_fixture_opts(96, 0, 0, 8, 3) :
        fixture(96, 0, 3, 10000, SBK_NO_FAILURE);
    defer_mapping = 0;
    uint64_t indices[96];
    for (size_t i = 0; i < f.n; i++) indices[i] = i * 5 % f.n;
    struct sbk_hot_list list = {.indices = (uintptr_t)indices, .count = f.n};
    struct sbk_capabilities caps;
    CHECK(ioctl(f.fd, SBK_IOC_CAPABILITIES, &caps) == 0 &&
        (caps.features & SBK_FEATURE_PARALLEL_PS));
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER, &list) < 0 && errno == EINVAL);
    uint64_t saved = indices[95];
    indices[95] = indices[0]; /* Duplicate across batches must be rejected atomically. */
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) < 0 && errno == EINVAL);
    CHECK(stats(f.fd).completed == 0 && stats(f.fd).errors == 0);
    indices[95] = f.n;
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) < 0 && errno == EINVAL);
    CHECK(stats(f.fd).completed == 0);
    indices[95] = saved;
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) == 0);
    struct sbk_stats s = stats(f.fd);
    CHECK(s.pretransferred == f.n && s.completed == f.n && !s.errors);
    CHECK(s.fetched[0] + s.fetched[1] + s.fetched[2] == 0);
    if (!rdma) {
        struct sbk_page_info a = {.index=indices[0]}, b = {.index=indices[8]},
            a_tail = {.index=indices[7]}, b_tail = {.index=indices[15]};
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &a) == 0 && ioctl(f.fd, SBK_IOC_PAGE, &b) == 0);
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &a_tail) == 0 && ioctl(f.fd, SBK_IOC_PAGE, &b_tail) == 0);
        CHECK(a.started_ns < b_tail.completed_ns && b.started_ns < a_tail.completed_ns);
        printf("PASS PS_batches_overlap first=%llu second=%llu first_tail=%llu second_tail=%llu\n",
            (unsigned long long)a.started_ns, (unsigned long long)b.started_ns,
            (unsigned long long)a_tail.completed_ns, (unsigned long long)b_tail.completed_ns);
    }
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) == 0);
    CHECK(stats(f.fd).pretransferred == f.n); /* Repeating a valid list never refetches. */
    CHECK(mmap(NULL, f.n * P, PROT_READ, MAP_PRIVATE, f.fd, 0) == MAP_FAILED);
    uint64_t dirty[] = {0, 47, 95};
    for (size_t i = 0; i < 3; i++) f.src[dirty[i]*P] ^= 0x6b;
    list.indices = (uintptr_t)dirty; list.count = 3;
    CHECK(ioctl(f.fd, SBK_IOC_SEAL, &list) == 0);
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) < 0 && errno == EINVAL);
    f.dst = map_destination(f.fd, f.n); CHECK(f.dst != MAP_FAILED);
    CHECK(memcmp(f.dst, f.src, f.n * P) == 0);
    s = stats(f.fd);
    CHECK(s.pretransferred == f.n && s.invalidated == 3 && s.completed == f.n &&
        s.fetched[SBK_DEMAND] == 3 && !s.errors);
    finish(&f);
    if (rdma) { CHECK(close(rdma_source_fd) == 0); rdma_source_fd = -1; }
    printf("PASS parallel_PS_full_list_seal_invalidation backend=%s\n", rdma ? "RDMA" : "LOOPBACK_TEST");
}
static void test_parallel_stage_failure(void)
{
    defer_mapping = 1;
    struct fixture f = fixture(128, 0, 4, 1000, 7);
    defer_mapping = 0;
    uint64_t indices[128];
    for (size_t i=0; i<f.n; i++) indices[i]=i;
    struct sbk_hot_list list = {.indices=(uintptr_t)indices, .count=f.n};
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) < 0 && errno == EIO);
    struct sbk_stats before=stats(f.fd); CHECK(before.errors == 1);
    usleep(100000);
    struct sbk_stats after=stats(f.fd);
    CHECK(before.completed == after.completed && before.pretransferred == after.pretransferred);
    for (size_t i=0; i<f.n; i++) {
        struct sbk_page_info p={.index=i};
        CHECK(ioctl(f.fd, SBK_IOC_PAGE, &p) == 0);
        CHECK(!p.started_ns || p.completed_ns); /* No read survives the ioctl's return. */
    }
    CHECK(ioctl(f.fd, SBK_IOC_PRETRANSFER_MANY, &list) < 0 && errno == EIO);
    CHECK(mmap(NULL, f.n*P, PROT_READ, MAP_PRIVATE, f.fd, 0) == MAP_FAILED);
    finish(&f);
    puts("PASS parallel_PS_failure_drains_all_workers_without_exposure");
}
static void test_anonymous_fork_and_discard(int rdma)
{
    struct fixture f=rdma?rdma_fixture(16):fixture(16,0,1,0,SBK_NO_FAILURE);
    pid_t pid=fork();CHECK(pid>=0);
    if(!pid){verify(&f);f.dst[0]=231;CHECK(f.dst[0]==231);_exit(0);}
    int status;CHECK(waitpid(pid,&status,0)==pid&&WIFEXITED(status)&&!WEXITSTATUS(status));
    verify(&f);
    struct sbk_stats st=stats(f.fd);
    CHECK(st.completed==16&&st.fetched[0]+st.fetched[1]+st.fetched[2]==32);
    CHECK(madvise(f.dst,2*P,MADV_DONTNEED)==0);
    for(size_t i=0;i<2*P;i++)CHECK(f.dst[i]==0);
    CHECK(stats(f.fd).faults==st.faults);
    CHECK(f.src[0]==pattern(0,0));
    finish(&f);if(rdma){CHECK(close(rdma_source_fd)==0);rdma_source_fd=-1;}
    printf("PASS anonymous_fork_unresolved_and_discard backend=%s\n",rdma?"RDMA":"LOOPBACK_TEST");
}
static void test_rdma(void)
{
    struct fixture f = rdma_fixture(512);
    CHECK(f.dst[200 * P] == pattern(200, 0));
    await_completed(f.fd, 5);
    uint64_t hot[] = {400, 401, 402, 403, 404, 405, 406, 407};
    background(f.fd, hot, 8);
    readers(&f, 1); await_completed(f.fd, f.n); verify(&f);
    struct sbk_stats s = stats(f.fd);
    CHECK(s.fetched[0] > 0 && s.fetched[1] >= 4 && s.fetched[2] > 0 && !s.errors);
    CHECK(s.fetched[0] + s.fetched[1] + s.fetched[2] == f.n);
    pid_t pid = fork(); CHECK(pid >= 0);
    if (!pid) { f.dst[0] = 241; CHECK(f.dst[0] == 241); _exit(0); }
    int status; CHECK(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    verify(&f);
    printf("PASS RDMA_read_kernel_paths fetched=%llu/%llu/%llu completed=%llu\n",
        (unsigned long long)s.fetched[0], (unsigned long long)s.fetched[1],
        (unsigned long long)s.fetched[2], (unsigned long long)s.completed);
    finish(&f); CHECK(close(rdma_source_fd) == 0); rdma_source_fd = -1;
    f = rdma_fixture(32);
    CHECK(ioctl(rdma_source_fd, SBK_IOC_REVOKE_SOURCE) == 0);
    CHECK(close(rdma_source_fd) == 0); rdma_source_fd = -1;
    expect_sigbus(&f, 7);
    CHECK(stats(f.fd).errors > 0);
    finish(&f);
    puts("PASS RDMA_source_revocation_containment");
}
/* Actual load-to-resumption samples, distinct from provider/transport timing.
 * RXE + KASAN results are instrumentation evidence, never ConnectX performance. */
static uint64_t now_raw(void)
{
    struct timespec t; CHECK(clock_gettime(CLOCK_MONOTONIC_RAW,&t)==0);
    return (uint64_t)t.tv_sec*1000000000ULL+t.tv_nsec;
}
static int compare_ns(const void *a,const void *b)
{
    uint64_t x=*(const uint64_t *)a,y=*(const uint64_t *)b;return (x>y)-(x<y);
}
static void fault_probe(struct fixture *f)
{
    uint64_t *samples=calloc(f->n,sizeof(*samples)),*sorted=calloc(f->n,sizeof(*sorted));
    CHECK(samples&&sorted);uint64_t total=0,clock_total=0;
    if(use_anon){
        unsigned char *resident=calloc(f->n,1);CHECK(resident);
        CHECK(mincore(f->dst,f->n*P,resident)==0);
        for(size_t i=0;i<f->n;i++)CHECK(!(resident[i]&1));
        free(resident);
    }
    for(size_t i=0;i<f->n;i++){
        uint64_t start=now_raw();clock_total+=now_raw()-start;
    }
    for(size_t i=0;i<f->n;i++){
        size_t index=(i*17)%f->n;uint64_t start=now_raw();
        unsigned char v=*(volatile unsigned char *)(f->dst+index*P);
        samples[i]=now_raw()-start;CHECK(v==pattern(index,0));total+=samples[i];
    }
    struct sbk_stats st=stats(f->fd);
    CHECK(st.faults==f->n&&st.fetched[0]==f->n&&!st.fetched[1]&&!st.fetched[2]&&!st.errors);
    if(use_token_pool) {
        struct sbk_token_pool_stats pool={0};
        CHECK(!ioctl(f->fd,SBK_IOC_TOKEN_POOL_STATS,&pool));
        CHECK(pool.prepared==f->n&&pool.claimed==f->n&&!pool.fallback&&!pool.available&&pool.sealed);
        puts("SBK_APPLICATION_FAULT_PROBE_TOKEN_POOL verified");
    }
    verify(f);memcpy(sorted,samples,f->n*sizeof(*samples));qsort(sorted,f->n,sizeof(*sorted),compare_ns);
    printf("SBK_APPLICATION_FAULT_PROBE mapping=%s n=%zu mean_ns=%llu p50_ns=%llu p95_ns=%llu p99_ns=%llu max_ns=%llu clock_pair_mean_ns=%llu handler_mean_ns=%llu handler_max_ns=%llu\n",
        use_anon?"anonymous_PTE":"file_fixture",f->n,(unsigned long long)(total/f->n),
        (unsigned long long)sorted[(f->n*50+99)/100-1],(unsigned long long)sorted[(f->n*95+99)/100-1],
        (unsigned long long)sorted[(f->n*99+99)/100-1],(unsigned long long)sorted[f->n-1],
        (unsigned long long)(clock_total/f->n),(unsigned long long)(st.fault_ns/st.faults),
        (unsigned long long)st.fault_max_ns);
    printf("SBK_APPLICATION_FAULT_SAMPLES_NS=[");
    for(size_t i=0;i<f->n;i++)printf("%s%llu",i?",":"",(unsigned long long)samples[i]);
    puts("]");free(samples);free(sorted);
}
/* Isolated two-VM integration test; fixed same-ABI control records, not production wire format. */
static void exchange_bytes(int fd, void *buf, size_t n, int sending)
{
    size_t off = 0;
    while (off < n) {
        ssize_t rc = sending ? send(fd, (char *)buf + off, n - off, MSG_NOSIGNAL) :
            recv(fd, (char *)buf + off, n - off, 0);
        if (rc < 0 && errno == EINTR) continue;
        CHECK(rc > 0); off += rc;
    }
}
static int peer_test(int source_side)
{
    int probe=getenv("SBK_FAULT_PROBE")!=NULL;
    const size_t n = 1024;
    struct fixture f = {.n = n};
    struct sbk_rdma_setup setup = {.role = source_side ? SBK_RDMA_SOURCE : SBK_RDMA_DESTINATION,
        .port = 1, .gid_index = 1, .timeout_ms = 2000, .slots = {4, 2, 2}, .pages = n};
    struct sbk_rdma_endpoint peer;
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(24567)};
    int sock = socket(AF_INET, SOCK_STREAM, 0); CHECK(sock >= 0);
    int yes = 1; CHECK(setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == 0);
    const char *source_ip = getenv("SBK_PEER_SOURCE_IP");
    const char *device = getenv("SBK_RDMA_DEVICE"), *gid = getenv("SBK_GID_INDEX");
    const char *deadline = getenv("SBK_RDMA_TIMEOUT_MS");
    if (deadline) setup.timeout_ms = strtoul(deadline,NULL,10);
    CHECK(inet_pton(AF_INET, source_ip ? source_ip : "192.0.2.11", &addr.sin_addr) == 1);
    snprintf(setup.device, sizeof(setup.device), "%s", device ? device : "rxe0");
    if (gid) setup.gid_index = atoi(gid);
    if (source_side) {
        CHECK(bind(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0);
        CHECK(listen(sock, 1) == 0);
        int accepted = accept(sock, NULL, NULL); CHECK(accepted >= 0); close(sock); sock = accepted;
    } else {
        int connected = 0;
        for (int i = 0; i < 500; ++i) {
            if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) { connected = 1; break; }
            usleep(20000);
        }
        CHECK(connected);
    }
    struct timeval timeout = {.tv_sec = 15};
    CHECK(setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) == 0);
    CHECK(setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) == 0);
    f.fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC); CHECK(f.fd >= 0);
    if (source_side) {
        f.src = mmap(NULL, n * P, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(f.src != MAP_FAILED);
        for (size_t i = 0; i < n; ++i)
            for (size_t j = 0; j < P; ++j) f.src[i * P + j] = pattern(i, j);
        setup.source_address = (uintptr_t)f.src;
    }
    CHECK(ioctl(f.fd, SBK_IOC_RDMA_CREATE, &setup) == 0);
    if (source_side) {
        exchange_bytes(sock, &peer, sizeof(peer), 0);
        CHECK(ioctl(f.fd, SBK_IOC_RDMA_CONNECT, &peer) == 0);
        exchange_bytes(sock, &setup.local, sizeof(setup.local), 1);
        char done; exchange_bytes(sock, &done, 1, 0); CHECK(done == 'D');
        CHECK(close(f.fd) == 0); CHECK(munmap(f.src, n * P) == 0);
        puts("SBK_PEER_SOURCE_PASS");
    } else {
        exchange_bytes(sock, &setup.local, sizeof(setup.local), 1);
        exchange_bytes(sock, &peer, sizeof(peer), 0);
        CHECK(ioctl(f.fd, SBK_IOC_RDMA_CONNECT, &peer) == 0);
        struct sbk_config c = {.version = SBK_ABI_VERSION, .backend = SBK_BACKEND_RDMA,
            .pages = n, .prefetch_workers = 2, .background_workers = 2, .batch_pages = 8,
            .prefetch_enabled = !probe, .test_fail_page = SBK_NO_FAILURE};
        CHECK(ioctl(f.fd, SBK_IOC_CONFIG, &c) == 0);
        if(probe){
            f.dst=map_destination(f.fd,n);CHECK(f.dst!=MAP_FAILED);
            fault_probe(&f);
            CHECK(munmap(f.dst,n*P)==0);CHECK(close(f.fd)==0);
            char done='D';exchange_bytes(sock,&done,1,1);
            puts("SBK_PEER_DESTINATION_PASS probe_only=1");
            close(sock);return 0;
        }
        struct sbk_capabilities caps;
        CHECK(ioctl(f.fd, SBK_IOC_CAPABILITIES, &caps) == 0);
        int parallel_ps = !!(caps.features & SBK_FEATURE_PARALLEL_PS);
        size_t pre_count = parallel_ps ? 128 : 8;
        uint64_t pre[128];
        for (size_t i=0; i<pre_count; i++) pre[i]=i;
        struct sbk_hot_list list = {.indices = (uintptr_t)pre, .count = pre_count};
        CHECK(ioctl(f.fd, parallel_ps ? SBK_IOC_PRETRANSFER_MANY : SBK_IOC_PRETRANSFER, &list) == 0);
        list.count = 0; CHECK(ioctl(f.fd, SBK_IOC_SEAL, &list) == 0);
        f.dst = map_destination(f.fd, n);
        CHECK(f.dst != MAP_FAILED);
        CHECK(f.dst[200 * P] == pattern(200, 0)); await_completed(f.fd, pre_count + 5);
        uint64_t hot[] = {800, 801, 802, 803, 804, 805, 806, 807};
        background(f.fd, hot, 8); readers(&f, 1); await_completed(f.fd, n); verify(&f);
        struct sbk_stats s = stats(f.fd);
        CHECK(!s.errors && s.pretransferred == pre_count && s.fetched[0] > 0 && s.fetched[1] >= 4 && s.fetched[2] > 0);
        CHECK(s.fetched[0] + s.fetched[1] + s.fetched[2] + s.pretransferred == n);
        printf("SBK_PEER_DESTINATION_PASS PS=%llu PF=%llu FT=%llu BG=%llu completed=%llu parallel_ps=%d\n",
            (unsigned long long)s.pretransferred, (unsigned long long)s.fetched[0],
            (unsigned long long)s.fetched[1], (unsigned long long)s.fetched[2], (unsigned long long)s.completed, parallel_ps);
        CHECK(munmap(f.dst, n * P) == 0); CHECK(close(f.fd) == 0);
        char done = 'D'; exchange_bytes(sock, &done, 1, 1);
    }
    close(sock);
    return 0;
}
#include "token-pool-test.inc"
#include "catalog-test.inc"
#include "export-batch-test.inc"
#include "session-dispatch-test.inc"
#include "dma-mr-test.inc"
#include "creator-drop-test.inc"

int main(int argc, char **argv)
{
    struct rlimit limit = {0, 0}; setrlimit(RLIMIT_CORE, &limit);
    setvbuf(stdout, NULL, _IONBF, 0);
    use_anon = getenv("SBK_TEST_ANON") != NULL;
    use_token_pool = getenv("SBK_TEST_TOKEN_POOL") != NULL;
    printf("SBK_MAPPING_MODE=%s\n", use_anon ? "anonymous_PTE" : "file_fixture");
    if (getenv("SBK_TEST_CREATOR_LIFETIME")) {
        test_creator_drop_lifetime(); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--dma-peer-source")) return dma_peer_test(1);
    if (argc == 2 && !strcmp(argv[1], "--dma-peer-destination")) return dma_peer_test(0);
    if (getenv("SBK_TEST_DMA_ONLY")) {
        test_dma_catalog(); test_dma_revoke_race();
        puts("SBK_DMA_TESTS_PASS"); return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "--peer-source")) return getenv("SBK_TEST_CATALOG") ? catalog_peer_test(1) : peer_test(1);
    if (argc == 2 && !strcmp(argv[1], "--peer-destination")) return getenv("SBK_TEST_CATALOG") ? catalog_peer_test(0) : peer_test(0);
    if (use_anon) test_token_pool();
    test_invalid(); test_private_cow(); test_concurrency(); test_prefetch();
    test_hot(); test_prefetch_overlaps_demand(); test_demand_does_not_wait_for_batch_tail();
    test_mapping_lifetime(); test_failure_cancel();
    test_stage(0); test_parallel_stage(0); test_parallel_stage_failure();
    if(use_anon){test_anonymous_fork_and_discard(0);test_guard_background();test_anonymous_background_fork_move(0);}
    puts("SBK_VM_TESTS_PASS backend=LOOPBACK_TEST not_RDMA");
    if (getenv("SBK_TEST_RDMA")) {
        test_rdma_export_batch();
        test_rdma_catalog();
        test_session_dispatch();
        test_rdma_export_revoke();
        test_rdma_catalog_child_export();
        test_rdma_catalog_final_mr();
        test_rdma();
        test_rdma_partial_batch_failure();
        test_rdma_admission_steal(SBK_BACKGROUND);
        test_rdma_admission_steal(SBK_PREFETCH);
        test_stage(1); test_parallel_stage(1);
        if(use_anon){test_anonymous_fork_and_discard(1);test_anonymous_source_retirement();test_anonymous_background_fork_move(1);}
        puts("SBK_VM_RDMA_TESTS_PASS transport=RXE not_ConnectX_performance");
    }
    return 0;
}
