/* SB_TFD_READ must read the actual fdinfo state without consuming ticks. */
static int sb_capture_timerfd(int fd, struct sb_timerfd_state *state)
{
	struct timespec before, after;
	struct itimerspec ignored, arm;
	int64_t lo, hi, value;
	int ret, attempt, wait_expired;

	ret = SB_TFD_READ(fd, state);
	if (ret)
		return ret;
	state->has_realtime = 0;
	if (state->clockid != CLOCK_REALTIME || !(state->flags & SB_TFD_ABSTIME))
		return 0;
	/* fdinfo does not expose an outstanding ECANCELED condition. Saving only
	 * the flag/ticks could turn a canceled timer into a successful expiration. */
	if (state->flags & SB_TFD_CANCEL_ON_SET)
		return -EOPNOTSUPP;
	for (attempt = 0; attempt < 16; attempt++) {
		/* gettime forwards an expired periodic timer and accounts overruns,
		 * without consuming its unread ticks. fdinfo alone does neither. */
		ret = SB_TFD_GET(fd, &ignored);
		if (ret)
			return ret;
		if (SB_TFD_CLOCK(CLOCK_REALTIME, &before))
			return -EIO;
		ret = SB_TFD_READ(fd, state);
		if (ret)
			return ret;
		if (SB_TFD_CLOCK(CLOCK_REALTIME, &after))
			return -EIO;
		if (sb_tfd_to_ns(before, &lo) || sb_tfd_to_ns(after, &hi) ||
		    sb_tfd_to_ns(state->val.it_value, &value))
			return -EINVAL;
		if (hi < lo || hi - lo > SB_TFD_CAPTURE_MAX_NS)
			continue;
		if (!value && (state->val.it_interval.tv_sec || state->val.it_interval.tv_nsec)) {
			/* Disarmed periodic timers have zero ticks. An expired periodic
			 * timer needs another gettime/read to expose its next boundary. */
			if (state->ticks && (ignored.it_value.tv_sec || ignored.it_value.tv_nsec))
				continue;
		}
		/* fdinfo reads ticks separately from remaining time. Do not accept
		 * an active snapshot whose expiration could fall inside this read. */
		if (value && value <= hi - lo)
			continue;
		state->has_realtime = 1;
		state->realtime = sb_tfd_from_ns(lo + (hi - lo) / 2);
		ret = sb_tfd_plan(state, after, &arm, &wait_expired);
		if (ret)
			return ret;
		return 0;
	}
	return -EAGAIN;
}
