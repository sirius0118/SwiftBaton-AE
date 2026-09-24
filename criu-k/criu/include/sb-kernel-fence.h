/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_FENCE_H
#define CR_SB_KERNEL_FENCE_H
#include <errno.h>
#include <signal.h>
#include <stddef.h>
#include <sys/ptrace.h>
#include <sys/types.h>

enum sbk_source_disposition { SBK_SOURCE_RESUME, SBK_SOURCE_STOP, SBK_SOURCE_KILL };

/* Called by the actual ptrace owner, after compel_wait_task has established
 * its base options. No subsequent SETOPTIONS may remove EXITKILL. Linux kills
 * each still-attached tracee if this owner thread or the whole controller dies. */
static inline int sbk_source_arm_exitkill(pid_t tid, unsigned long base_options)
{
  if (tid <= 0) return -EINVAL;
  return ptrace(PTRACE_SETOPTIONS, tid, NULL, base_options | PTRACE_O_EXITKILL) ? -errno : 0;
}

/* Only the ptrace owner may release its threads, after parasite cure. Stopping
 * is requested before detaching; EXITKILL protects every task still attached.
 * If any release step fails, kill the whole thread group rather than leave a
 * mixture of runnable and traced tasks. The caller must abort the controller
 * on error so its other owners also fail closed. */
static inline int sbk_source_release(pid_t leader, const pid_t *tids, size_t count,
                                      enum sbk_source_disposition how, int stop_signal)
{
  int error;
  if (leader <= 0 || !tids || !count || tids[0] != leader ||
      how < SBK_SOURCE_RESUME || how > SBK_SOURCE_KILL) return -EINVAL;
  for (size_t i = 0; i < count; i++) if (tids[i] <= 0) return -EINVAL;
  if (how == SBK_SOURCE_KILL)
    return kill(leader, SIGKILL) && errno != ESRCH ? -errno : 0;
  if (how == SBK_SOURCE_STOP) {
    if (stop_signal != SIGSTOP && stop_signal != SIGTSTP &&
        stop_signal != SIGTTIN && stop_signal != SIGTTOU) stop_signal = SIGSTOP;
    if (kill(leader, stop_signal)) goto fail;
  }
  for (size_t i = 0; i < count; i++)
    if (ptrace(PTRACE_DETACH, tids[i], NULL, NULL)) goto fail;
  return 0;
fail:
  error = errno;
  kill(leader, SIGKILL);
  return -error;
}
#endif
