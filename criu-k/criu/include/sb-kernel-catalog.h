/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_CATALOG_H
#define CR_SB_KERNEL_CATALOG_H
#include "common/sbk-criu.h"
#include <stddef.h>
#include <stdint.h>
struct sbk_catalog;
struct sbk_catalog_record {
  uint32_t source_pid, restore_pid;
  uint64_t address;
  struct sbk_rdma_region remote;
};
struct sbk_catalog_final {
  struct sbk_catalog_record record;
  const uint64_t *dirty, *hot;
  size_t dirty_count, hot_count;
};
/* Functions return zero/success or a negative errno; poll returns 1 once all
 * final ranges retired their markers and drained workers. Content/transport
 * errors are reported through stats; process health is a separate requirement.
 */
struct sbk_catalog *sbk_catalog_create(int session_fd,
                                       const struct sbk_config *config);
void sbk_catalog_destroy(struct sbk_catalog *catalog);
/* Stage calls may run concurrently. The coordinator must join every stage
 * call before seal, destroy, serve, poll or totals; no other API is concurrent. */
int sbk_catalog_stage(struct sbk_catalog *catalog,
                      const struct sbk_catalog_record *record,
                      const uint64_t *indices, size_t count);
int sbk_catalog_seal(struct sbk_catalog *catalog,
                     const struct sbk_catalog_final *records, size_t count);
/* Serialized local control connection. Receiver owns returned fds and malloc
 * table; every error closes received descriptors. No fd number goes on wire. */
int sbk_catalog_serve(struct sbk_catalog *catalog, int socket_fd);
int sbk_catalog_receive(int socket_fd, uint32_t pid,
                        struct sbk_restore_region **table, unsigned int *count,
                        int *ready_fd);
/* Arm-ready events start hot-first background work; drained events gate source
 * retirement. Caller may poll its connection separately to serve more tasks. */
int sbk_catalog_poll(struct sbk_catalog *catalog, int timeout_ms);
int sbk_catalog_totals(struct sbk_catalog *catalog, struct sbk_stats *stats);
#endif
