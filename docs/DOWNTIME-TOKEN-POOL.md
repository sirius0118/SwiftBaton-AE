# PS token preparation and final-validation overlap

This candidate follows commit `ba3c0089` on the independent downtime branch.
The immutable `swiftbaton-u-56ms-20260927` release remains unchanged. Both modes
have not reached 40 ms; the tables below record actual physical-host trials.

## K: reserve tokens before stopping the source

The destination now reserves anonymous PTE tokens during PS. A session pool
contains unbound tokens, so differences between PS and final VMA geometry do
not authorize reuse of an incorrect mapping. Final ARM binds each token once
to the final region entry before publishing its PTE. Missing capacity falls
back to the existing allocation path. No page-fault operation takes the pool
lock; normal callbacks gain one forwarding indirection.

The PS wire header version is 4 and carries a bounded virtual-page count hint.
It is only an allocation hint. Final PFN/dirty validation, region export and
ownership handoff remain mandatory. The additive feature bit and reserve/stats
ioctls use the existing K1 kernel bridge; this change does not modify the base
kernel or require a reboot. Use matching CRIU binaries on both hosts.

Pool, region, token and chunk lifetimes are separate. Bound regions retain the
session pool after its original FD closes. Unclaimed tokens do not contribute
to a region's retirement count. Token chunks defer freeing through the module
reaper because a last token reference may disappear under a PTE lock. Closing
the pool releases leftovers; shortage, partial allocation failure, fork,
unmap, failed ARM and retry are tested.

The first full physical run exposed a retirement race: normal removal of the
last marker set `stopping` before admitted background reads completed, recording
three false cancellation errors. That run remains a failure. Normal drain now
closes admission and joins existing work before marking the context stopped;
new BG quanta stop at retirement. Explicit cancellation and real transport
failures still fail. The source-retirement acknowledgment follows the join.

## U: overlap final validation with remaining dump work

The validator starts only after all task dumpers publish the final lazy-page
catalog and while source application threads remain stopped. Remaining MR and
image preparation can run concurrently. The job owns a duplicated image
directory FD and copied PID list. Final image publication and every error path
join the job before continuing. Failure to create its coordinator thread uses
the original synchronous path.

Dump and restore now use INFO logging instead of forced DEBUG logging in both
CRIU trees. Phase timestamps, page-path counters, warnings and errors remain.
The combined physical trial is not a separate causal ablation of logging and
validation overlap.

## Validation and measurement

Tests include KASAN/lock-debug RXE and anonymous-PTE suites, reservation failure
injection, shared-region pools, background work concurrent with unmap, complete
drain, source revocation and module unload. A second VM uses the exact physical
candidate module, performance kernel and OFED import CRCs. These VM timings are
not the performance evidence. Real ConnectX migrations run on knode2/knode3.

The host rollback procedure now checks the loaded ELF build ID. `modinfo -n`
alone previously selected an older installed module instead of the manually
loaded runtime; the parameter mismatch was detected and corrected. Original
runtime SHA-256 `4fcd86e6413ef7a239d4f121e3617f137d5182a617046ffa7686ecb2b7a85231`
and its parameters are verified after each subsequent trial. No host reboot
occurred. The candidate module hash is
`fab88287fd825b291e66114b1a564d58de342de0590ba6c2314ae9c5c3492bf5`.

The full workload remains 500,000 records × 10,240 bytes, 32 YCSB clients,
workload A, 25 Gbps QoS and an 8 MiB bytewise canary. Correctness checks cover
record presence/length/count, canary SHA-256, image checksums, page drain and
cleanup. The whole 5.12 GB payload is not bytewise hashed.

Client downtime is the intersection of inter-success intervals from all 32
workers on one JVM monotonic clock. It is not the gate ACK duration or sampled
zero-throughput bins. TTR90 uses the stable final destination reference, a
100 ms rolling window and one sustained second from observed service recovery.
Kernel callback times include cache hits and are not remote-only page-fault
latencies. U reports queue-to-install latency separately.

## Results

The machine-readable trial list accompanies the final experiment archive.
For context, the prior U candidate measured 46.59/46.95 ms and prior K measured
769.84/752.18 ms. The failed K run `174103` is excluded from successful results.

| Run | Mode / change | Client gap (ms) | TTR90 start / confirmed (s) |
|---|---|---:|---:|
| 175140 | K token pool, corrected drain | 638.017076 | 5.82 / 6.827 |
| 175551 | U final-validation overlap + INFO | 43.059336 | 1.09 / 2.09 |
| 175938 | K token pool, INFO repeat | 650.311930 | 3.35 / 4.35 |
| 180214 | U repeat | 51.676828 | 2.61 / 3.61 |
| 180658 | U repeat | 41.574479 | 1.54 / 2.54 |

All five full-workload rows passed data, image and cleanup checks with valid
steady references. All U runs had zero reconnects; both K runs had 32. The U
median is 43.059336 ms, but the 51.676828 ms observation is retained. In that
run the gate interval was 43.217720 ms and successful client requests resumed
about 9 ms after release. There is no per-event GC trace proving the reason;
the extra delay must not be subtracted from client downtime. These few trials
do not establish a latency bound or a stable sub-50 ms result.

Run 175140 used all four paths: valid PS 376,268, demand 22,417, prefetch 96,703,
BG 1,067,391; 1,562,779 total pages and zero errors. It used 1,562,779 prepared
tokens, kept 193,252 unused, and allocated no fallback tokens. Target ARM was
310.031 ms, catalog setup 110.352 ms, and source final preparation 173.635 ms.
These phases are not a full additive breakdown of the client gap. Its 32
reconnect calls remain visible; connection timeout settings were not changed.

Run 175551 had no reconnects. Its 1,129 remote-fault queue-to-install samples
averaged 16.839 µs, p99 25.707 µs, max 49.704 µs, with zero dropped samples.
All four page paths were exercised and ownership accounting matched.

After the migration trials, three independent physical cold-page probes
measured 1,024 real anonymous-PTE remote faults each, with NUMA node 0 binding
and PS/FT/BG disabled. Application-load-to-resumption means were 9.143, 9.161
and 9.144 µs; p99 was 12.460, 13.974 and 12.450 µs. Clock-pair overhead was
20–21 ns and has not been subtracted. The probes verify zero pre-resident
pages, all reads on the demand lane, every returned byte pattern and actual
use of prepared tokens. This is an isolated remote-fault microbenchmark,
not the latency distribution of container faults under migration load.

A matched follow-up with the original module, same probe binary and NUMA
binding measured means of 9.070, 9.099 and 9.042 µs. The new path's mean is
about 0.08 µs higher across these small sequential samples; this is not a
fault-latency improvement or a randomized confidence interval. The token
pool's measured benefit is in preparation during the stop window.

K still requires substantial changes to PTE activation and preparation of
registration/catalog resources before it can approach 40 ms. This candidate
does not claim that goal or a consistent improvement in TTR across workloads.
