/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_H
#define CR_SB_KERNEL_H
#include "common/sbk-criu.h"
struct parasite_ctl;
struct task_restore_args;
/* ctl is already infected and has at least sizeof(sbk_export_args) args space.
 * Failed calls require aborting this source catalog (partially pinned MRs).
 * session_fd stays owned by the controller; the injected copy is closed. */
int sb_kernel_export_seized(struct parasite_ctl *ctl, int session_fd,
                           struct sbk_rdma_region *regions, unsigned int count,
                           unsigned int workers, unsigned int *peak);
/* Transfer ownership of the region fds only on success. No allocations from
 * this arena are allowed after rst_mem_lock; call before that boundary. */
int sb_kernel_prepare_restore(struct task_restore_args *ta,
                              const struct sbk_restore_region *regions, unsigned int count, int ready_fd);
/* Receive a catalog transaction before locking the restore arena. */
int sb_kernel_receive_restore(struct task_restore_args *ta, int socket_fd, unsigned int pid);
#endif
