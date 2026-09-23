#ifndef __CR_SB_TRACE_H__
#define __CR_SB_TRACE_H__
#include <time.h>
#include <unistd.h>
#include "log.h"

/* Monotonic timestamps compare phases on one host. Realtime correlates hosts
 * only to the accuracy of their clock synchronization; it is not downtime. */
static inline void sb_trace(const char *phase)
{
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC_RAW, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    pr_info("SB_PHASE phase=%s pid=%d mono_ns=%llu real_ns=%llu\n", phase,
            getpid(), (unsigned long long)mono.tv_sec * 1000000000ULL + mono.tv_nsec,
            (unsigned long long)real.tv_sec * 1000000000ULL + real.tv_nsec);
}

/* A dumper thread operates on a different subject PID; retain it so traces
 * from parallel tasks can be paired without mixing their intervals. */
static inline void sb_trace_task(const char *phase, pid_t task)
{
    struct timespec mono, real;
    clock_gettime(CLOCK_MONOTONIC_RAW, &mono);
    clock_gettime(CLOCK_REALTIME, &real);
    pr_info("SB_PHASE phase=%s pid=%d mono_ns=%llu real_ns=%llu task=%d\n", phase,
            getpid(), (unsigned long long)mono.tv_sec * 1000000000ULL + mono.tv_nsec,
            (unsigned long long)real.tv_sec * 1000000000ULL + real.tv_nsec, task);
}
#endif
