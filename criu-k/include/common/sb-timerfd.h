#ifndef SB_TIMERFD_H
#define SB_TIMERFD_H

#include <errno.h>
#include <stdint.h>
#include <time.h>

#define SB_TFD_ABSTIME 1
#define SB_TFD_CANCEL_ON_SET 2
#define SB_TFD_NSEC 1000000000LL
#define SB_TFD_CAPTURE_MAX_NS 1000000LL

/* Shared by the dump, PIE restorer and native syscall tests. fdinfo values
 * remain relative for old readers; new images additionally carry the source
 * CLOCK_REALTIME sample. MONOTONIC/BOOTTIME retain the legacy pause semantics
 * because independent hosts/time namespaces do not share their clock epoch. */
struct sb_timerfd_state {
	int clockid;
	int flags;
	uint64_t ticks;
	struct itimerspec val;
	int has_realtime;
	struct timespec realtime;
};

static inline int sb_tfd_to_ns(struct timespec t, int64_t *ns)
{
	if (t.tv_sec < 0 || t.tv_nsec < 0 || t.tv_nsec >= SB_TFD_NSEC ||
	    (uint64_t)t.tv_sec > (INT64_MAX - (uint64_t)t.tv_nsec) / SB_TFD_NSEC)
		return -EINVAL;
	*ns = (int64_t)t.tv_sec * SB_TFD_NSEC + t.tv_nsec;
	return 0;
}

static inline struct timespec sb_tfd_from_ns(int64_t ns)
{
	struct timespec t = { ns / SB_TFD_NSEC, ns % SB_TFD_NSEC };
	return t;
}

/* Construct an arm operation without modifying the image or its saved ticks.
 * A periodic timer with unread expirations is armed at the preceding (already
 * expired) boundary. Wait for that first callback before SET_TICKS. It then
 * remains stopped until read/gettime forwards it, just like native timerfd;
 * overruns during the migration are counted by Linux, with no SET_TICKS race. */
static inline int sb_tfd_plan(const struct sb_timerfd_state *s, struct timespec now,
			      struct itimerspec *arm, int *wait_expired)
{
	int64_t value, interval, origin;
	if (sb_tfd_to_ns(s->val.it_value, &value) ||
	    sb_tfd_to_ns(s->val.it_interval, &interval) ||
	    (s->flags & ~(SB_TFD_ABSTIME | SB_TFD_CANCEL_ON_SET)))
		return -EINVAL;
	*arm = s->val;
	*wait_expired = 0;
	if (s->has_realtime && (s->clockid != CLOCK_REALTIME ||
	    !(s->flags & SB_TFD_ABSTIME) || (s->flags & SB_TFD_CANCEL_ON_SET)))
		return -EINVAL;
	if (!(s->flags & SB_TFD_ABSTIME) || !value)
		return 0; /* Zero must stay disarmed, including an expired one-shot. */
	if (sb_tfd_to_ns(s->has_realtime ? s->realtime : now, &origin) ||
	    value > INT64_MAX - origin)
		return -EOVERFLOW;
	value += origin;
	if (s->has_realtime && s->ticks) {
		/* fdinfo cannot describe manually injected ticks on an active
		 * one-shot, or a periodic timer whose first deadline is still ahead.
		 * Refuse that ambiguous state instead of losing/duplicating a tick. */
		if (!interval || value <= interval || value - interval > origin)
			return -EOPNOTSUPP;
		value -= interval;
		*wait_expired = 1;
	}
	arm->it_value = sb_tfd_from_ns(value);
	return 0;
}

#endif
