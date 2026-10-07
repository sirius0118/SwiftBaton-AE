#define _GNU_SOURCE
#include "sb-idle.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>

/* RDMA publication has no wake syscall. Seventeen workers share each small
 * CPU mask; one services requests and all must observe stop publication. */
#define POLLERS 17U
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

static bool select_cpus(const cpu_set_t *original, unsigned count)
{
    cpu_set_t mask;
    unsigned chosen = 0;
    CPU_ZERO(&mask);
    for (int c = 0; c < CPU_SETSIZE && chosen < count; c++)
        if (CPU_ISSET(c, original)) { CPU_SET(c, &mask); chosen++; }
    if (chosen != count) return false;
    assert(!sched_setaffinity(0, sizeof(mask), &mask));
    return true;
}

static void check_wait_state(void)
{
    struct sb_idle idle = {0};
    sb_idle_init(&idle);
    assert(idle.cooperative && idle.sleep_ns == SB_IDLE_SLEEP_NS);
    for (unsigned i = 0; i < SB_IDLE_SPINS * 10; i++) sb_idle_poll(&idle, false);
    assert(idle.sleep_ns == SB_IDLE_MAX_SLEEP_NS && !idle.spins);
    sb_idle_poll(&idle, false);
    assert(idle.spins == 1);
    sb_idle_poll(&idle, true);
    assert(!idle.spins && idle.sleep_ns == SB_IDLE_SLEEP_NS);
}

static void check_publication(unsigned cpus)
{
    pthread_t threads[POLLERS];
    request = response = stop = 0;
    check_wait_state();
    uint64_t begun = ns(CLOCK_MONOTONIC), cpu = ns(CLOCK_PROCESS_CPUTIME_ID);
    for (uintptr_t i = 0; i < POLLERS; i++)
        assert(!pthread_create(&threads[i], NULL, poller, (void *)i));
    usleep(200000);
    for (unsigned i = 1; i <= 1000; i++) {
        __atomic_store_n(&request, i, __ATOMIC_RELEASE);
        struct sb_idle idle = {0};
        while (__atomic_load_n(&response, __ATOMIC_ACQUIRE) != i)
            sb_idle_poll(&idle, false);
    }
    __atomic_store_n(&stop, 1, __ATOMIC_RELEASE);
    for (unsigned i = 0; i < POLLERS; i++) assert(!pthread_join(threads[i], NULL));
    assert(response == 1000);
    printf("U_IDLE_PUBLICATION_PASS requests=%u cpus=%u pollers=%u wall_ms=%.3f cpu_ms=%.3f\n",
           response, cpus, POLLERS, (ns(CLOCK_MONOTONIC)-begun)/1e6,
           (ns(CLOCK_PROCESS_CPUTIME_ID)-cpu)/1e6);
}

int main(void)
{
    cpu_set_t original;
    assert(!sched_getaffinity(0, sizeof(original), &original));
    alarm(10);
    for (unsigned cpus = 2; cpus <= 4; cpus *= 2) {
        if (!select_cpus(&original, cpus)) {
            printf("U_IDLE_SKIP cpus=%u available=%d\n", cpus, CPU_COUNT(&original));
            continue;
        }
        check_publication(cpus);
    }
    if (select_cpus(&original, 8)) {
        struct sb_idle idle = {0};
        for (unsigned i = 0; i < SB_IDLE_SPINS * 10; i++) sb_idle_poll(&idle, false);
        assert(!idle.cooperative && !idle.spins && idle.sleep_ns == SB_IDLE_SLEEP_NS);
        printf("U_IDLE_SPIN_POLICY_PASS cpus=8\n");
    }
    assert(!sched_setaffinity(0, sizeof(original), &original));
    return 0;
}
