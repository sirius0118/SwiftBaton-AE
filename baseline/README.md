# RDMA migration baselines

The SwiftBaton-K release remains sealed at
`swiftbaton-k-ae-ct-batch-20260928`. These five baseline prototypes run in
separate builds and profiles. Their matched Redis/YCSB trials used Node1 as
client, Node2 as source, Node3 as destination, 500,000 × 10 KiB records,
16 client threads, a 300-second workload with 20-second warmup, and the same
25 Gbps `ens4f1` hardware QoS cap in both directions. The sealed K release's
49.315 ms trial used 32 clients and is therefore a separate observation.

| Baseline | Algorithm exercised | Transport and validation |
| --- | --- | --- |
| Native CRIU v3.18 | Stock stop-and-copy checkpoint and restore | Eight parallel rsocket relay lanes stream complete images; matching SHA-256 tree digests |
| PCLive prototype | First resident PS snapshot, source re-read and changed-page second round, final validation and resident adoption | Rsocket images, both PS rounds and separate AS fault/prefetch/background lanes; changed-page and byte/ACK checks |
| Post-copy prototype | No PS page payload; independent demand faults and address-order background transfer | Rsocket images, PS control snapshot and separate AS fault/background lanes |
| Hybrid-copy prototype | Resident PS plus demand and bounded page-index background queues | Rsocket images, PS snapshot and separate AS fault/background lanes; checkerboard fragmentation check |
| Remote-fork prototype | Kernel demand-only pages, with no PS/FT/BG installation during service | Kernel RDMA and marker-retirement checks |

The U prototypes reuse SwiftBaton CRIU orchestration, and the remote-fork
prototype reuses its kernel module. They are functional algorithm profiles,
not independent reimplementations of the cited systems. The main image and
memory payloads of the U baselines use the same librdmacm rsocket library;
some existing setup/control messages still use the original CRIU channels.
The kernel fault handler cannot call a userspace rsocket API, so remote-fork
retains kernel RDMA under the same NIC cap. Do not interpret the one-run
end-to-end differences as transport-normalized speedups.

The stock CRIU path uses `common/rsocket_relay` because normal `send`/`recv`
cannot take rsocket pseudo-descriptors. `tree_stream.py` copies the image tree
incrementally and verifies its digest. The U page paths use rsocket
`riomap`/`riowrite`; see `as-rsocket/README.md`. PCLive re-reads all source
candidates locally in its second round to avoid a soft-dirty epoch race,
while sending only changed pages. It does not yet implement further rounds
or source-side dirty-only reads. The hybrid scheduler uses bounded queues
and a page index instead of one pipe per missing run; its 4 GiB alternating
fragmentation test kept the file-descriptor count at four.

Run this command on Node2 with `NAME` equal to `pclive`, `postcopy`,
`hybrid` or `remote-fork`:

```bash
python3 baseline/run.py NAME --profile redis --threads 16 --duration 300 --warmup 20 --execute
```

The default is a non-mutating preview;
`--check` performs read-only cluster preflight and `--profile smoke` uses
100k × 1 KiB. The runner locks the cluster, checks the selected CRIU hash on
both hosts, validates images/keys/canary and page accounting, cleans owned
containers, and restores both installed CRIU symlinks. Use
`baseline/native-criu/run_redis.py` for the stock CRIU row.

`baseline/report.py` prints a matched-workload table from validated run
artifacts. It rejects failed cleanup or unstable final throughput. The
measurements, result paths, metric definitions and remaining limits are in
`results-20260928.md`.
