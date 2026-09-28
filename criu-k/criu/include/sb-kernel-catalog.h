/* SPDX-License-Identifier: GPL-2.0 */
#ifndef CR_SB_KERNEL_CATALOG_H
#define CR_SB_KERNEL_CATALOG_H
#include "common/sbk-criu.h"
#include <stddef.h>
#include <stdint.h>
struct sbk_catalog;
struct sbk_catalog_timing {
  uint64_t validate_ns, prepare_ns, seal_ns;
  uint64_t launch_ns, allocation_sum_ns, allocation_max_ns;
  unsigned workers_started, allocation_peak;
  uint64_t seal_import_ns, seal_io_ns;
  unsigned seal_workers_started, seal_peak;
  unsigned import_workers_started, import_peak;
  uint64_t ps_plan_pages, reused_plan_pages, discarded_plan_pages;
  unsigned ps_plans, reused_plans, discarded_plans;
};
void sbk_catalog_get_timing(const struct sbk_catalog *catalog,
                            struct sbk_catalog_timing *timing);
struct sbk_catalog_record {
  uint32_t source_pid, restore_pid;
  uint64_t address;
  struct sbk_rdma_region remote;
};
/* Advisory PS shape only: no remote byte/address/key is implied by this. */
struct sbk_catalog_layout { uint32_t source_pid, reserved; uint64_t address, pages; };
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
/* Bounded final CONFIG preparation; call before seal. Default is serial. */
int sbk_catalog_prepare_workers(struct sbk_catalog *catalog, unsigned workers);
/* Coordinator only, before stage workers. Requires prepared/unbound features.
 * Final catalog still defines exact coverage and dirty/PFN validation. */
int sbk_catalog_prepare_layout(struct sbk_catalog *catalog,
                               const struct sbk_catalog_layout *layout, size_t count);
void sbk_catalog_destroy(struct sbk_catalog *catalog);
/* Stage calls may run concurrently. The coordinator must join every stage
 * call before seal, destroy, serve, poll or totals; no other API is concurrent. */
int sbk_catalog_stage(struct sbk_catalog *catalog,
                      const struct sbk_catalog_record *record,
                      const uint64_t *indices, size_t count);
/* After all PS workers join, move staged PS pages into prepared layout
 * contexts while the source still runs. Final dirty validation remains at
 * seal, before any destination PTE is exposed. */
int sbk_catalog_import_ps_early(struct sbk_catalog *catalog);
int sbk_catalog_seal(struct sbk_catalog *catalog,
                     const struct sbk_catalog_final *records, size_t count);
int sbk_catalog_start_rsocket_proxy(struct sbk_catalog *catalog,
                                   int control_fd, int port, unsigned workers);
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
/* Read-only post-drain diagnostics. Count only pages whose last remote marker
 * retired before a fetch; any unexplained deficit remains a hard failure. */
struct sbk_catalog_audit {
  struct sbk_catalog_record record;
  struct sbk_stats stats;
  struct sbk_drain_status drain;
  uint64_t states[8]; /* current kernel 0..6; 7 records unknown states */
};
int sbk_catalog_audit_deficits(struct sbk_catalog *catalog,
                              void (*report)(const struct sbk_catalog_audit *),
                              uint64_t *retired_unfetched);
#endif
