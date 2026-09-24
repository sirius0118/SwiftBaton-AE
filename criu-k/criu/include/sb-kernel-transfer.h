/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_TRANSFER_H
#define CR_SB_KERNEL_TRANSFER_H
#include "common/sbk_uapi.h"
struct parasite_ctl;
int sb_kernel_options(void);
int sb_kernel_connect(int socket_fd, int source);
int sb_kernel_register_final(struct parasite_ctl *ctl, int pid, int vpid,
                             struct sbk_rdma_region *regions, unsigned count);
int sb_kernel_send_ps(int socket_fd);
int sb_kernel_receive_ps(int socket_fd);
int sb_kernel_send_final(int socket_fd);
int sb_kernel_source_finish(void);
int sb_kernel_source_exposed(void);
int sb_kernel_client_receive(int socket_fd);
int sb_kernel_client_serve(int listen_fd, int socket_fd);
void sb_kernel_transfer_close(void);
#endif
