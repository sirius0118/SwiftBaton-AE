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
| Native CRIU | Unmodified upstream v3.18 and rsocket image transfer | Standalone process, Redis container, 100k and 500k Redis/YCSB with full key/canary verification |
| PCLive prototype | Two PS snapshots: source re-reads all pages locally but RDMA-reads only changed payloads into the same target memfd, refreshes anonymous resident staging, then validates and adopts with `mremap` | 100k and 500k Redis/YCSB with full key-length/canary validation |
| Optimized post-copy prototype | PS payload disabled; independent demand and address-order BG lanes | 100k smoke and 500k Redis/YCSB, PF=73,581, BG=1,663,096, FT=PS=0 in full run |
| Hybrid-copy prototype | PS staging plus independent demand/BG; address-order BG | 100k smoke and 500k Redis/YCSB, PS=756,179, PF=32,758, BG=947,740 in full run |
| Remote-fork prototype | Kernel demand-only during service; target exit retires unused markers before source MR revocation | 100k smoke and 500k Redis/YCSB, PF=1,545,059, FT=BG=PS=0, 17,422 retired unused in full run |

`baseline/run.py` selects a profile, holds the same migration lock as the main
driver, verifies CRIU binary hashes on both hosts, and restores both installed
CRIU symlinks after each run. A preview is the default; `--check` performs
read-only cluster preflight and `--execute` runs a profile. `--profile smoke`
uses 100k x 1 KiB, while `--profile redis` uses 500k x 10 KiB.
`--duration`, `--warmup` and `--threads` override either profile for matched
large-load trials; for example, `--profile redis --threads 16 --duration 300
--warmup 20 --execute`.

The PCLive second round re-reads all candidates locally to avoid the
soft-dirty epoch handoff race, but transfers only changed payloads over RDMA.
Its 500k x 10 KiB trial (`sb_ae_20260928_074945`) refreshed 267,631
resident pages and read 1.170 GB over RDMA including metadata. Every indexed
key length and the 8 MiB canary passed; the client success gap was 328.775 ms,
stable target reference 44.8k ops/s, and TTR90 started 3.520 s after service
resumed. More than two rounds and source-side dirty-only reads are not yet
implemented. The hybrid profile
uses a page index and bounded queues, not a pipe per missing run. Its
checkerboard stress test committed 1,048,576 pages with 524,288 isolated
pre-copy fragments and four file descriptors before and after. Native CRIU's
matched 100k x 1 KiB trial
(`sb_native_20260928_061945`) observed an 8314 ms common successful-operation
gap with 177.7 MB transferred over rsocket. That gap includes the stock
stop-and-copy checkpoint, complete image transfer, restore and client endpoint
switch. The target's last-ten-second YCSB reference was stable at 80.3k
ops/s; all keys and the canary passed. Run
`SB_STOCK_CRIU=/path/to/upstream/criu/criu python3 baseline/native-criu/run_redis.py`
to repeat it; the driver restores both installed CRIU symlinks in cleanup.
The larger 500k x 10 KiB trial (`sb_native_20260928_063745`, 16 YCSB
threads, 300-second run) also passed: 6.40 GB of images streamed in 27.33 s,
and the client-wide successful-operation gap was 212.507 s. Checkpoint took
about 113 s; target restore and endpoint switch took about 68 s after image
arrival. The last-ten-second target reference was stable at 42.1k ops/s.
All 500k indexed keys were present with the expected value length, and the
8 MiB canary passed byte-for-byte SHA-256 verification. The indexed values
were not individually checksummed. A subsequent parallel rsocket trial with
the same workload (`sb_native_20260928_082012`) sent 6.40 GB in 5.61 s and
measured a 183.958 s client success gap, 42.5k stable target ops/s, and
TTR90 start 0.480 s after service resumed. All indexed key lengths and the
canary passed, both CRIU symlinks were restored, and the owned containers were
removed. These are single runs, so the end-to-end gap difference also includes
checkpoint/restore variance. `baseline/report.py` builds a matched-workload table
from validated result/state files and rejects missing cleanup or unstable
target throughput. U profiles use native ibverbs, K uses kernel RDMA, and
native CRIU uses rsocket; results are not yet transport-normalized speedups.
The full matched-workload measurements, raw artifact paths, metric definitions
and implementation limits are recorded in `baseline/results-20260928.md`.
