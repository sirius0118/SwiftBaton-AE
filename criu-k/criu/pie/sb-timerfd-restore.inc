/* Included by the actual PIE restorer and by the native syscall regression.
 * SB_TFD_* wrappers in the native test call the real Linux syscalls. */
static int sb_restore_timerfd(int fd, const struct sb_timerfd_state *state)
{
	struct itimerspec arm;
	struct timespec now = { 0, 0 };
	int ret, wait_expired;

	if ((state->flags & SB_TFD_ABSTIME) && !state->has_realtime &&
	    (state->val.it_value.tv_sec || state->val.it_value.tv_nsec)) {
		ret = SB_TFD_CLOCK(state->clockid, &now);
		if (ret)
			return ret;
	}
	ret = sb_tfd_plan(state, now, &arm, &wait_expired);
	if (ret)
		return ret;
	ret = SB_TFD_SET(fd, state->flags, &arm);
	if (ret)
		return ret;
	if (wait_expired) {
		/* No ticks have been injected yet. Readability proves the actual
		 * first hrtimer callback completed, so SET_TICKS cannot race it. */
		ret = SB_TFD_WAIT(fd);
		if (ret)
			return ret;
	}
	if (state->ticks)
		return SB_TFD_TICKS(fd, &state->ticks);
	return 0;
}
