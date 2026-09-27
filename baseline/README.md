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

Current verification:

| Baseline | Implementation | Verified |
| --- | --- | --- |
| Native CRIU | Unmodified upstream v3.18 and rsocket image transfer | Standalone 64 MiB process and Redis container, Node2 to Node3 |
| PCLive | Pending | No |
| Optimized post-copy | Pending | No |
| Hybrid-copy | Pending | No |
| Remote-fork | Pending | No |

The small Redis container smoke is only a correctness check; no YCSB or
comparable performance result has been recorded for the baselines yet. Do not
use its transfer time as a SwiftBaton comparison.
