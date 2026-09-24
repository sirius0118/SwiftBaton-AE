#ifndef __SB_LIFECYCLE_H__
#define __SB_LIFECYCLE_H__
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#define SB_FAULT_LANES 4U
#define SB_FAULT_BUCKETS 32U
#define SB_FAULT_LOCAL 3U
struct sb_lifecycle;
struct sb_lifecycle_range { uint64_t start, end; };
struct sb_lifecycle_stats {
    uint64_t remaps, removes, unmaps, forks, copies, zeroes, discarded, retries;
    uint64_t fault_events, fault_tracked, fault_coalesced, fault_resolved_before_read, fault_retired;
    uint64_t fault_installed[SB_FAULT_LANES], fault_total_ns[SB_FAULT_LANES], fault_max_ns[SB_FAULT_LANES];
    uint64_t fault_hist[SB_FAULT_LANES][SB_FAULT_BUCKETS];
    uint64_t event_gate_count, event_gate_total_ns, event_gate_max_ns;
};
struct sb_lifecycle *sb_lifecycle_create(void);
/* Diagnostic ablation; call before adding families. Default gives pending
 * lifecycle/fault readers the writer lock before new background installers. */
int sb_lifecycle_reader_preference(struct sb_lifecycle *, int enabled);
/* Opt-in busy waiting with pending-writer priority; configure before add().
 * Mutually exclusive with the reader-preference ablation. */
int sb_lifecycle_spin_gate(struct sb_lifecycle *, int enabled);
/* Offline identity of each pthread gate, used to match futex observations.
 * Query before starting workers. Returns 1=item, 0=end, negative errno. */
struct sb_gate_info { pid_t pid; uintptr_t address; size_t bytes; unsigned spin; };
int sb_lifecycle_gate_info(struct sb_lifecycle *, unsigned index, struct sb_gate_info *);
/* Optional diagnostic history. Enable before adding any UFFD. No file I/O
 * occurs on the page path. Export only after all callers have quiesced. */
int sb_lifecycle_enable_trace(struct sb_lifecycle *);
int sb_lifecycle_write_trace(struct sb_lifecycle *, int fd);
/* Takes ownership of fd on success. Call before starting pager workers. */
int sb_lifecycle_add(struct sb_lifecycle *, pid_t origin_pid, int fd,
                     const struct sb_lifecycle_range *, size_t count);
/* Install immutable source bytes at every still-pending descendant mapping.
 * Returns 0, EEXIST, ENODATA (no surviving consumer), or a positive errno.
 * Reads/events and installs synchronize per original UFFD; different pages
 * can install concurrently. Forks inherit the exact pending-page bitmap. */
int sb_lifecycle_install(struct sb_lifecycle *, pid_t origin_pid,
                         uint64_t origin_address, const void *page);
/* Observation lane affects statistics only; no scheduling/ownership change.
 * 0=background, 1=prefetch, 2=demand, 3=local/unclassified. Timings start when
 * userspace reads the UFFD event, not when the application enters the kernel. */
int sb_lifecycle_install_tagged(struct sb_lifecycle *, pid_t, uint64_t, const void *, unsigned lane);
/* Optional per-call wall-clock attribution, owned by the calling installer.
 * Durations do not overlap. COPY time includes any sleep/descheduling inside
 * the syscall; it is not a measurement of pure kernel CPU execution. */
struct sb_install_profile {
    uint64_t ready_ns, read_gate_ns, write_gate_ns, copy_ns, drain_ns, retire_ns;
    uint64_t copies, contexts, retries;
};
int sb_lifecycle_install_profiled(struct sb_lifecycle *, pid_t, uint64_t,
                                  const void *, unsigned, struct sb_install_profile *);
/* Drains lifecycle events and translates faults to the immutable source
 * identity. Returns 1=request, 0=none, or negative errno. Zero-fill faults are
 * resolved locally. Never invoke install from inside an event callback. */
int sb_lifecycle_next(struct sb_lifecycle *, pid_t origin_pid, uint64_t *address);
/* Collect at most capacity translated original addresses during each gate
 * acquisition. Mapping events are drained before translation; returned pages
 * retain no target pointers or lock references. Returns count or -errno. */
int sb_lifecycle_next_batch(struct sb_lifecycle *, pid_t, uint64_t *, unsigned capacity);
/* All original source pages must be delivered/adopted or explicitly discarded
 * before finish. Closing every owned descriptor also unregisters remapped and
 * newly grown ranges and fork descendants, without stale address guesses. */
int sb_lifecycle_finish(struct sb_lifecycle *);
void sb_lifecycle_stats(struct sb_lifecycle *, struct sb_lifecycle_stats *);
/* Fault observations for all original UFFDs of one process, including their
 * post-restore fork descendants. Non-fault lifecycle fields remain zero. */
int sb_lifecycle_pid_stats(struct sb_lifecycle *, pid_t, struct sb_lifecycle_stats *);
void sb_lifecycle_destroy(struct sb_lifecycle *);
#endif
