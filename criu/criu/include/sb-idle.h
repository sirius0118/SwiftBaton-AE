/* Cooperative polling for CRIU workers sharing at most four CPUs. RDMA writes
 * do not wake a futex, so idle waits are bounded and always recheck the original
 * acquire-load predicate. Active work never waits; normal CPU masks keep PAUSE.
 * This header is userspace-only, including its per-thread affinity snapshot. */
#ifndef __CR_SB_IDLE_H__
#define __CR_SB_IDLE_H__
#include <stdbool.h>
#include <sched.h>
#include <sys/prctl.h>
#include <time.h>

#define SB_IDLE_COOPERATIVE_MAX_CPUS 4
#define SB_IDLE_SPINS 32U
#define SB_IDLE_SLEEP_NS 10000L
#define SB_IDLE_MAX_SLEEP_NS 100000L

struct sb_idle {
	bool initialized, cooperative;
	unsigned spins;
	long sleep_ns;
};

static inline void sb_idle_init(struct sb_idle *idle)
{
	cpu_set_t mask;
	idle->initialized = true;
	idle->sleep_ns = SB_IDLE_SLEEP_NS;
	idle->cooperative = !sched_getaffinity(0, sizeof(mask), &mask) && CPU_COUNT(&mask) <= SB_IDLE_COOPERATIVE_MAX_CPUS;
	/* Default timer slack can exceed the entire polling interval. Only the
	 * calling low-core worker changes its slack, never a restored application. */
	if (idle->cooperative)
		(void)prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
}

static inline void sb_idle_poll(struct sb_idle *idle, bool progress)
{
	if (!idle->initialized)
		sb_idle_init(idle);
	if (progress) {
		idle->spins = 0;
		idle->sleep_ns = SB_IDLE_SLEEP_NS;
		return;
	}
	__asm__ volatile("pause" ::: "memory");
	if (idle->cooperative && ++idle->spins >= SB_IDLE_SPINS) {
		const struct timespec delay = { .tv_sec = 0, .tv_nsec = idle->sleep_ns };
		idle->spins = 0;
		/* EINTR also returns to the predicate, rather than restarting a wait. */
		(void)nanosleep(&delay, NULL);
		if (idle->sleep_ns < SB_IDLE_MAX_SLEEP_NS) {
			idle->sleep_ns *= 2;
			if (idle->sleep_ns > SB_IDLE_MAX_SLEEP_NS)
				idle->sleep_ns = SB_IDLE_MAX_SLEEP_NS;
		}
	}
}
#endif
