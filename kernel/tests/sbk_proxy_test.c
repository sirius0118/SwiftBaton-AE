// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#include "sbk_uapi.h"

#define P 4096UL
#define N 16UL
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "proxy fail %s:%d %s errno=%d\n", \
    __func__, __LINE__, #x, errno); exit(1); } } while (0)

struct worker { int fd; uint8_t *source; unsigned int count; int error; };
static uint8_t pattern(unsigned long page, unsigned long byte)
{
    return (page * 41 + byte * 17 + 13) % 251;
}
static void *serve(void *arg)
{
    struct worker *w = arg;
    unsigned int i;
    for (i = 0; i < w->count; i++) {
        struct sbk_proxy_request req = {};
        struct sbk_proxy_completion done = {};
        struct pollfd pfd = {.fd = w->fd, .events = POLLIN};
        if (poll(&pfd, 1, 3000) != 1 || !(pfd.revents & POLLIN)) {
            w->error = ETIMEDOUT; return NULL;
        }
        if (ioctl(w->fd, SBK_IOC_PROXY_NEXT, &req)) { w->error = errno; return NULL; }
        if (req.index >= N || req.lane != SBK_DEMAND || !req.id || req.reserved) {
            w->error = EPROTO; return NULL;
        }
        done.id = req.id;
        done.data = (uintptr_t)(w->source + req.index * P);
        if (ioctl(w->fd, SBK_IOC_PROXY_COMPLETE, &done)) { w->error = errno; return NULL; }
        if (ioctl(w->fd, SBK_IOC_PROXY_COMPLETE, &done) != -1 || errno != ENOENT) {
            w->error = EPROTO; return NULL;
        }
    }
    return NULL;
}
static void run(int anonymous)
{
    int fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
    uint8_t *src, *dst;
    struct sbk_capabilities caps = {};
    struct sbk_config cfg = {.version = SBK_ABI_VERSION,
        .backend = SBK_BACKEND_RSOCKET_PROXY, .pages = N,
        .prefetch_workers = 1, .background_workers = 1,
        .batch_pages = 1, .test_fail_page = SBK_NO_FAILURE};
    struct worker w = {.fd = fd, .count = N};
    struct sbk_stats stats = {};
    pthread_t thread;
    unsigned long i, j;
    CHECK(fd >= 0);
    CHECK(!ioctl(fd, SBK_IOC_CAPABILITIES, &caps));
    CHECK(caps.features & SBK_FEATURE_RSOCKET_PROXY);
    src = mmap(NULL, N * P, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    CHECK(src != MAP_FAILED);
    for (i = 0; i < N; i++)
        for (j = 0; j < P; j++) src[i * P + j] = pattern(i, j);
    w.source = src;
    CHECK(!ioctl(fd, SBK_IOC_CONFIG, &cfg));
    CHECK(!fcntl(fd, F_SETFL, O_NONBLOCK));
    struct sbk_proxy_request empty = {};
    CHECK(ioctl(fd, SBK_IOC_PROXY_NEXT, &empty) == -1 && errno == EAGAIN);
    if (anonymous) {
        struct sbk_anon_arm arm;
        dst = mmap(NULL, N * P, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        CHECK(dst != MAP_FAILED);
        arm.address = (uintptr_t)dst;
        CHECK(!ioctl(fd, SBK_IOC_ARM_ANON, &arm));
    } else {
        dst = mmap(NULL, N * P, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
        CHECK(dst != MAP_FAILED);
    }
    CHECK(!pthread_create(&thread, NULL, serve, &w));
    for (i = 0; i < N; i++)
        for (j = 0; j < P; j++) CHECK(dst[i * P + j] == pattern(i, j));
    CHECK(!pthread_join(thread, NULL));
    CHECK(!w.error);
    CHECK(!ioctl(fd, SBK_IOC_STATS, &stats));
    CHECK(stats.fetched[SBK_DEMAND] == N);
    CHECK(stats.errors == 0);
    CHECK(!munmap(dst, N * P));
    CHECK(!munmap(src, N * P));
    CHECK(!close(fd));
    printf("SBK_PROXY_%s_PASS\n", anonymous ? "ANON" : "FILE");
}
static void cancel_waiting_fault(int dispatched)
{
    int fd = open("/dev/swiftbaton_k", O_RDWR | O_CLOEXEC);
    struct sbk_config cfg = {.version = SBK_ABI_VERSION,
        .backend = SBK_BACKEND_RSOCKET_PROXY, .pages = 1,
        .prefetch_workers = 1, .background_workers = 1,
        .batch_pages = 1, .test_fail_page = SBK_NO_FAILURE};
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    struct sbk_proxy_request req = {};
    struct sbk_proxy_completion late = {};
    uint8_t *dst;
    int status;
    pid_t child;
    CHECK(fd >= 0);
    CHECK(!ioctl(fd, SBK_IOC_CONFIG, &cfg));
    dst = mmap(NULL, P, PROT_READ, MAP_PRIVATE, fd, 0);
    CHECK(dst != MAP_FAILED);
    child = fork();
    CHECK(child >= 0);
    if (!child) {
        volatile uint8_t value = dst[0];
        (void)value;
        _exit(2);
    }
    CHECK(poll(&pfd, 1, 3000) == 1 && (pfd.revents & POLLIN));
    if (dispatched) CHECK(!ioctl(fd, SBK_IOC_PROXY_NEXT, &req));
    CHECK(!ioctl(fd, SBK_IOC_CANCEL));
    if (dispatched) {
        late.id = req.id;
        late.status = -EIO;
        CHECK(ioctl(fd, SBK_IOC_PROXY_COMPLETE, &late) == -1 && errno == ENOENT);
    }
    CHECK(waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS);
    CHECK(!munmap(dst, P));
    CHECK(!close(fd));
    puts(dispatched ? "SBK_PROXY_INFLIGHT_CANCEL_PASS" : "SBK_PROXY_CANCEL_PASS");
}
int main(void)
{
    run(0);
    run(1);
    cancel_waiting_fault(0);
    cancel_waiting_fault(1);
    puts("SBK_PROXY_TESTS_PASS");
    return 0;
}
