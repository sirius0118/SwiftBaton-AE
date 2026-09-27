# RDMA baselines

All baseline results must use the same Node1 YCSB client, Redis image and load,
Node2 source, Node3 destination, `ens4f1` QoS cap and definition of client
successful-operation gap. Keep each implementation separate from the sealed
SwiftBaton-U/K release profiles.

`common/rsocket_relay` exposes loopback TCP endpoints to programs that assume
POSIX socket file descriptors, while every inter-host byte crosses an RDMA
rsocket connection. rsocket pseudo-descriptors cannot be passed directly to
CRIU's normal `send`/`recv` calls, so the relay is part of the baseline's
transport, not a transparent CRIU recompilation. `tree_stream.py` copies a
complete image tree incrementally with SHA-256 verification over that link.

The first functional U/K profiles reuse the existing SwiftBaton CRIU
orchestration and its native RDMA verbs transport. This establishes each
algorithm's behavior on the same Redis/YCSB experiment before a transport
refactor. **Do not label those profiles as independent paper implementations or
claim that they already use rsocket.** Native CRIU is the only current baseline
using the common rsocket relay for its inter-host image bytes. Kernel remote
faults cannot call the userspace rsocket library; they use the kernel RDMA
transport already present in K.

Current verification:

| Baseline | Implementation | Verified |
| --- | --- | --- |
| Native CRIU | Unmodified upstream v3.18 and rsocket image transfer | Standalone 64 MiB process and Redis container, Node2 to Node3; YCSB pending |
| PCLive prototype | One PS snapshot into anonymous resident staging, final validity and `mremap` adoption | 100k-key Redis/YCSB smoke, full key/canary validation |
| Optimized post-copy prototype | PS payload disabled; independent demand and address-order BG lanes | 100k-key Redis/YCSB smoke, PF=543, BG=64,966, FT=PS=0 |
| Hybrid-copy prototype | PS staging plus independent demand/BG; address-order BG | 100k-key Redis/YCSB smoke, PS=18,622, PF=99, BG=46,788 |
| Remote-fork prototype | Kernel demand-only during service; target exit retires unused markers before source MR revocation | 100k-key Redis/YCSB smoke, PF=42,163, FT=BG=PS=0, 815 retired unused |

`baseline/run.py` selects a profile, holds the same migration lock as the main
driver, verifies CRIU binary hashes on both hosts, and restores both installed
CRIU symlinks after each run. A preview is the default; `--check` performs
read-only cluster preflight and `--execute` runs a profile. `--profile smoke`
uses 100k x 1 KiB, while `--profile redis` uses 500k x 10 KiB.

The PCLive prototype currently has one payload snapshot with two validity
checks. Updating the resident target memory with successive dirty snapshots
remains to be implemented before it is a full PCLive baseline. The hybrid
profile reuses the sorted page index and validity bitmap, so it does not
construct one pipe per surviving page; fragmentation scaling still needs a
specific stress test. Native CRIU needs a full YCSB run and matched performance
analysis. Cross-baseline throughput and downtime comparisons are not yet
ready; the rows above are correctness smoke tests, not measured speedups.
