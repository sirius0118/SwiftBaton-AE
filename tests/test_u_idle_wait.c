#define _GNU_SOURCE
#include "sb-idle.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

/* Exercise memory publication without a wake syscall, as with an RDMA ring:
 * twelve idle pollers share two CPUs, one receives requests, all must stop. */
static unsigned request, response, stop;
static void *poller(void *opaque)
{
    struct sb_idle idle = {0};
    unsigned seen = 0;
    while (!__atomic_load_n(&stop, __ATOMIC_ACQUIRE)) {
        unsigned head = __atomic_load_n(&request, __ATOMIC_ACQUIRE);
        bool progress = !opaque && head != seen;
        if (progress) {
            seen = head;
            __atomic_store_n(&response, seen, __ATOMIC_RELEASE);
        }
        sb_idle_poll(&idle, progress);
    }
    assert(idle.cooperative);
    return NULL;
}
static uint64_t ns(clockid_t clock)
{
    struct timespec t;
    assert(!clock_gettime(clock, &t));
    return (uint64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
int main(void)
{
    cpu_set_t original, mask;
    pthread_t threads[12];
    struct sb_idle policy = {0};
    unsigned chosen = 0;
    assert(!sched_getaffinity(0, sizeof(original), &original));
    if (CPU_COUNT(&original) > 2) {
        sb_idle_init(&policy);
        assert(!policy.cooperative);
    }
    CPU_ZERO(&mask);
    for (int c = 0; c < CPU_SETSIZE && chosen < 2; c++)
        if (CPU_ISSET(c, &original)) { CPU_SET(c, &mask); chosen++; }
    assert(chosen && !sched_setaffinity(0, sizeof(mask), &mask));
    alarm(5);
    uint64_t begun = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_PROCESS_CPUTIME_ID);
    for (uintptr_t i = 0; i < 12; i++) assert(!pthread_create(&threads[i], NULL, poller, (void *)i));
    usleep(200000);
    for (unsigned i = 1; i <= 1000; i++) {
        __atomic_store_n(&request, i, __ATOMIC_RELEASE);
        struct sb_idle idle = {0};
        while (__atomic_load_n(&response, __ATOMIC_ACQUIRE) != i) sb_idle_poll(&idle, false);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < 12; i++) assert(!pthread_join(threads[i], NULL));
    printf("U_IDLE_PUBLICATION_PASS requests=%u cpus=%u wall_ms=%.3f cpu_ms=%.3f\n",
           response, chosen, (ns(CLOCK_MONOTONIC)-begun)/1e6, (ns(CLOCK_PROCESS_CPUTIME_ID)-cpu)/1e6);
    assert(!sched_setaffinity(0, sizeof(original), &original));
    return 0;
}
