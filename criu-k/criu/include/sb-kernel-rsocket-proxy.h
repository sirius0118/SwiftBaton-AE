/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_RSOCKET_PROXY_H
#define CR_SB_KERNEL_RSOCKET_PROXY_H
#include "sb-kernel-catalog.h"
#include <stddef.h>

struct sbk_proxy_region {
    int fd;
    struct sbk_catalog_record record;
};
int sbk_rsocket_source_start(int control_fd, int port,
                            const struct sbk_catalog_final *records, size_t count,
                            unsigned workers);
void sbk_rsocket_source_stop(void);
int sbk_rsocket_target_start(int control_fd, int port,
                            const struct sbk_proxy_region *regions, size_t count,
                            unsigned workers);
void sbk_rsocket_target_stop(void);
#endif
