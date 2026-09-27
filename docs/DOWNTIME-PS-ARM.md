# SwiftBaton-K: PS prepared plans and parallel final catalog

This experimental branch preserves the earlier 56 ms release and changes only
the isolated K development version. Its full Redis migration now has a
**169.56–172.83 ms client-observed cutover gap** across two validated runs,
down from 578–580 ms on the earlier ARM-batch version. The requested <50 ms
full-workload target has **not** been reached. A small 100,000-record smoke
workload reached 44.00 ms; it is not a substitute for the 500,000-record run.

The principal change prepares target kernel contexts, token ownership and
detached PTE plans during PS using advisory source layout. At final, only
exactly matching source/address/page-count plans are reused; changed shapes
use the ordinary safe allocation path. The final catalog remains authoritative
for page presence, PS validity and remote MR identity. The source PS layout
collector skips sparse giant VMAs that would otherwise exhaust region slots
before resident Redis ranges; it never limits final frozen-source coverage.

The final catalog now imports disjoint PS page slices into target contexts in
bounded parallel workers, joins every worker, then seals each independent
context in parallel. The kernel's IMPORT_PS path orders both context locks by
address and checks ownership/state before moving pages. A source-range overlap
falls back to the original serial ordering. Failure leaves the catalog
unpublished, joins all workers and closes its contexts. Tests exercise
concurrent imports and seals, failed CONFIG/WATCH/IMPORT/SEAL, partial thread
creation and descriptor cleanup under normal, ASan/UBSan and TSan builds.

The kernel page accounting accepts an unfetched page only when the post-drain
per-page audit proves its last remote marker has retired, no fetch is in flight
and no error exists. A full run exercised this case for five pages that Redis
unmapped before retrieval; all records and the canary still verified. Every
other missing page remains a hard failure.

## Physical measurements

All full runs used 500,000 records × 10 KiB, 32 YCSB clients, Redis on Node2,
target Node3, and 25 Gbps all-priority TC1 QoS. The command included
`--kernel-ps-arm --vma-cache --buffered-cutover --network-lock nftables`,
16 validation/catalog workers, and 64 MiB source export chunks. TTR uses the
recorded target-final 10 s throughput reference, 0.5 s smoothing, a sustained
1 s 90% threshold, and starts at the first qualifying service sample. All
listed TTR results were marked valid by the recovery metric checker.

| Candidate | Full client gap (ms) | Source final export (ms) | Target final catalog seal (ms) | TTR90 start (s) |
|---|---:|---:|---:|---:|
| Earlier ARM-batch K | 579.99, 578.29 | — | — | 5.11, 1.49 |
| PS prepared plan, serial seal | 221.08, 211.25 | 83.15, 68.57 | 56.90, 59.77 | 2.03, 1.91 |
| Parallel seal | 182.19, 193.67 | 68.03, 69.36 | 37.28, 42.26 | 1.71, 1.96 |
| Parallel import and seal | **169.56, 172.83** | 75.03, 76.17 | **13.28, 13.55** | 1.95, 1.87 |
| Same code, 32 export workers | 170.95 | 74.48 | 14.50 | 1.87 |

The first prepared-plan pair was measured with post-boot PCP trust, but all
priorities still mapped to the same 25 Gbps TC1. The later pairs were measured
after restoring DSCP trust and strict TC1 scheduling. The source and workload
also vary between runs; these are real observed results, not isolated causal
estimates. The 32-worker comparison did not improve registration and remains
an ablation, not the default.

On the final 16-worker pair, source final preparation took 104.18 and
105.94 ms: pagemap/planning 14.53/15.02 ms, PS validation 4.46/4.53 ms,
MR export 75.03/76.17 ms, and hot ordering 9.81/9.86 ms. Target PS import
fell from ~35 ms serial to 7.92/7.11 ms parallel; the separate seal ioctl
phase was 5.14/6.23 ms. This leaves source MR registration as the largest
single measured stop-window operation.

Both final full runs completed the Redis key-count, presence and expected
length of all 500,000 indexed records, exact-byte 8 MiB canary, and client
endpoint checks; transfer errors were zero and all four K memory paths had
nonzero page counts. The per-record check does not compare value bytes; the
canary provides a separate byte-for-byte integrity check. The client log showed
32 successful reconnect calls in each run, so connection continuity is not
yet achieved even though requests and data verified. The live kernel callback
aggregate `fault_ns/faults` includes ready-page hits and waits; it must not be
reported as a single cold remote page-fault latency. A separate physical
ConnectX-6 microbenchmark made 1,024 cold anonymous-PTE loads per repetition
on NUMA node 0: means **9.20/9.11/9.12 µs** and p99
**15.04/12.87/15.13 µs** across three repetitions. This excludes concurrent
PS/FT/BG traffic and is not the container's page-fault distribution.

The original Node2 and Node3 runtime CRIU links and kernel modules were
restored after each trial. Node3 remains on the one-shot prepared-plan test
kernel; its protected old default boot entry, paused foreign VM, original
containers, RDMA QoS and NFS mounts were verified. The full structured data,
raw run paths, page counters and exact timing fields are in
`docs/arm-plan-trials.json` in this repository.

## Next bottleneck

The <50 ms goal requires taking most of the 75 ms source MR export out of the
stop window. Increasing export workers did not help. A safe PS preregistration
design may target only final-layout chunks with no PS-cached candidate pages,
so registration's writable GUP cannot invalidate the soft-dirty test for
those cached pages. Reuse must still be rejected if the source page mapping
changes between registration and final freeze. The existing eBPF VMA journal
is useful as a hint but cannot detect COW or other PFN changes by itself;
an MMU invalidation notifier or exact pinned-PFN comparison, with fallback
registration, is required before this can be called correct. No PS source-MR
reuse is included in this checkpoint.
