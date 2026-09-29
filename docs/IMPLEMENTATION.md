# Current implementation map

The main entry points are `scripts/run.py`, the two mode-specific `run_ae.py` drivers, and `configs/profiles.json`. The workflow does not invoke Fluid. RAM-backed CRIU state images use RDMA in both documented profiles; the bootstrap copies only control information between hosts.

## SwiftBaton-U

`criu/criu/sb-precopy.c` prepares PS snapshots and validates them against final soft-dirty/PFN information. `sb-stage.c` installs valid snapshots in a parent staging process for inheritance/remapping. `sb-vma-cache.c` and `sb-vma-monitor.bpf.c` implement PS-time VMA collection with mutation monitoring and final checks.

`sb-transfer.c`, `sb-sched.c`, `sb-lifecycle.c` and the associated headers coordinate separate demand, prefetch, and background paths. Per-page states and lifecycle synchronization prevent duplicate ownership and handle mapping changes. The reference profile uses one demand worker, one prefetch worker, a bounded prefetch window, parallel background copies/installers, and NUMA placement. Timing traces are buffered and emitted after the transfer stage rather than synchronously logged on each fault.

## SwiftBaton-K

`criu-k/criu/sb-kernel*.c` implements the CRIU coordinator, source region export, PS pipeline, restore plan, and drain/retirement protocol. `kernel/module/sbk_core.c` owns page/region state and the MM bridge; `sbk_rdma.c` handles RDMA transport; `sbk_dispatch.c` provides bounded session-wide FT/BG worker pools.

- **Pre-transfer:** PS snapshot buffers allow early transfer while the source runs; final validation discards stale pages. PS has a snapshot copy and is not described as entirely zero-copy.
- **Demand:** the kernel fault callback reads from the frozen source MR directly into the destination page using demand-specific QP/CQ slots, then adopts the page into anonymous memory. It does not round-trip through UFFD and userspace page installation.
- **Prefetch:** independent FT workers fetch nearby pages early. Queued speculative ownership can be taken by a demand request until the speculative RDMA is actually submitted. Current adjacent prefetch stays within each MR.
- **Hot-first batch:** sampled heat orders background work; regions share a fixed session-wide worker budget. Background work yields after bounded batch quanta, preventing a worker count multiplied by the number of VMAs.
- **Isolation:** demand does not enter the FT/BG task queues. Lane reservation precedes speculative INFLIGHT ownership and is released before PTE installation. Already-posted speculative RDMA is not preemptible. These properties reduce contention but do not imply zero waiting or NIC-level hardware priority.
- **Lifetime:** token references follow fork and moved aliases. A complete transfer requires no unresolved markers and no remaining background jobs; only then can source MRs be revoked and the source retired. Failure cleanup destroys the destination before releasing source exposure.

NUMA affinity and bounded BG4 are retained to favor demand latency. Increasing background workers is configurable, but may trade higher bandwidth for worse demand latency.

## Current source paths and optional acceleration

The current U/K drivers use `scripts/ae/{u,k}/buffered_cutover.py`,
`packet_gate.py`, `conntrack_gate.py`, and `nat_bindings.py` to coordinate
client traffic while changing the destination. The network lock is selected
by the run profile; reviewers can inspect a case's exact options with
`--dry-run`. These helpers do not require Fluid.

K's `sbk_rdma.c` supports PS-time source MR preparation, bounded region
cataloguing, and invalidation checks before reusing an export. The
`sbk_token_pool.c` source and optional prepared-ARM/batch kernel patches
reduce marker setup work. The module compiles against either the base PTE
patch or the optional prepared-ARM series; see [kernel setup](KERNEL.md) for
the patch order and build-tree matching rules. `sbk_core.c` also exposes
an rsocket page-proxy backend used by the demand-only remote-fork baseline.
It is separate from the default direct-RDMA K fault path.

The U tree includes rsocket image/snapshot and AS transport implementations
for the baseline profiles. `baseline/` contains their build/runner source,
while `experiments/` contains the workload, ablation, and plotting source.
The default U/K quickstart remains `scripts/run.py`; the full case map is in
[experiments/README.md](../experiments/README.md).

## State preparation and boundaries

The CRIU trees contain namespace preparation and the current state-image transport. K additionally retains expanded FD object preparation, identity/alias validation, deferred epoll references, and absolute timerfd fixes. Support is not universal: the tested 5.15 interface cannot recover every EFD_SEMAPHORE or queued-UDP case, and unsupported timerfd cancellation/injected-tick states are rejected rather than silently approximated. Arbitrary shared state and every paper workload are not covered by the Redis quickstart.

The implementation files and runnable instructions are versioned here. Performance records and historical debugging narratives belong in separately stored run directories.
