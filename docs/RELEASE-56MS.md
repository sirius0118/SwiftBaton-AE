# SwiftBaton-U 56 ms baseline — 2026-09-27

This is the exact stage12 implementation measured in `sb_ae_20260927_161122`, frozen before subsequent optimization. It is a single physical-cluster observation, not a repeated-run guarantee or a SwiftBaton-K latency result.

- Client no-success interval: **56.350716 ms**, all 32 worker journals intersected on the same JVM monotonic clock. Wall anchors only locate the cutover; their spread was 0.921768 ms.
- Redis: 500,000 records × 10,240-byte field0 (5,120,000,000 payload bytes), workload A, 32 clients, RDMA hardware limit 25 Gbps, source knode2 → target knode3, client knode1.
- TCP: 0 logged reconnects; 115 queued packets released; NFQUEUE drops and netlink drops both 0.
- 500,000 indexed records checked for presence and expected field length; 500,003 total keys and sentinel verified. This is not a checksum of all workload values. A separate 8 MiB canary passed SHA-256 verification.
- All 1,736,677 source pages accounted for: pre-transfer 1,323,698; demand 637; prefetch 1,297; background 411,045. Source/target accounting matches.
- Remote demand queue-to-install latency, 637 samples: mean 23.427 us, p50 16.200 us, p95 25.413 us, p99 31.730 us, max 3829.900 us. No trace drops. These are measured U transport events, not kernel trap-to-resume times.
- U executable SHA-256: `d2b6d09b852893199596fe20fe726611e2ccc419f6840da0f78c57eea2057c31`.

## Exact command

Run from the frozen source after deploying its preserved binaries and YCSB to the prepared cluster:

```sh
python3 scripts/run.py U --profile redis --network-lock nftables --vma-cache --stage-max-mb 64 --buffered-cutover --validation-workers 16 --execute
```

Other profile settings are preserved in `configs/profiles.json`, the original driver `result.json`, and migration `state.json`. Source NUMA 1; 1 demand worker, 1 prefetch worker, 4 background/copy workers, prefetch window 4, batch 16 pages, 8 GiB pre-copy capacity. Both NICs map all priorities to TC1 capped at 25 Gbps. No host kernel/module changes were made in this optimization series.

## Known report limitation preserved in this baseline

The migration driver completed its data and canary checks successfully, and wrapper cleanup and runtime symlink restoration succeeded. The wrapper's overall result is nevertheless **success=false**: `analyze_recovery.py` raised StopIteration because `analyze_run.py` selected the terminal workload idle samples as the longest zero run. Consequently this run has no valid TTR report, and the later automatic CRIU-image checksum step did not run before image cleanup. Do not relabel that wrapper result as PASS. Independent success-gap, transport and fault analyses completed afterward against retained raw logs. The 56.350716 ms observation is from the independent success journal and does not use the broken zero-run selector.

This reporting issue will be repaired on the next development branch; the frozen code and original logs retain it. The archived K executable compiled successfully, but this U measurement does not validate the current K performance. The preceding clean U run `sb_ae_20260927_160439` measured 59.745459 ms with the full wrapper checks passing.

## What is frozen

Git tag, source archive and Git bundle; U and K executables; the exact built YCSB tree and runtime dependencies; configuration and build/deploy logs; complete raw results, worker success journals, transport/PF traces, driver result and cleanup logs; independent analysis outputs; file hashes. Restore from the archived source and binaries to a separate path, deploy while idle, and invoke its wrapper. Each wrapper restores the previously installed CRIU symlinks after running.
