# Experiments after the 56 ms freeze (2026-09-27)

The immutable baseline is tag `swiftbaton-u-56ms-20260927`, commit
`1e4fe1758e0b4918c74a23d31878b953a70ae842`. Development continues on
`codex/downtime-after56-20260927`. This is an experimental candidate, not a
claim that both U and K meet 40 ms.

## Changes

* Prepare the U source transfer catalog after listening and before accepting
  the restore connection. Final image/validity publication still precedes this
  operation; page workers and ownership handoff still wait for the normal
  protocol. Fresh anonymous scheduler storage skips a redundant full memset.
* Bound inherited-stage invalidation to the last actually selected page.
  Final dirty/PFN checks and discarding invalid cached pages remain mandatory.
* K sparse planning and PS validation share one pagemap snapshot taken while
  all application threads are stopped, before writable MR registration. Up to
  32 readers use disjoint pread jobs. A 256 MiB bound retains the serial fallback
  for larger maps; every worker is joined before cleanup. This changes CRIU C
  code only, with no kernel/module ABI change.
* Expose existing MR export chunk/worker controls in the common run wrapper.
* Fix sampled migration-window selection so terminal workload idle cannot
  replace the migration interval. Preserve all raw intervals. Check image
  integrity before analysis, and collect every analysis result before failing.

## Physical workload and measurement

knode1 runs 32 YCSB threads; Redis migrates knode2 to knode3. Each run uses
500,000 records with a 10,240-byte field, 50% reads / 50% updates, an 8 MiB
canary, and the existing 25 Gbps hardware QoS configuration. Source NUMA
binding is U node 1 and K node 0, as in their existing profiles.

Downtime is the intersection of all 32 client workers' intervals between
consecutive successful requests, using timestamps from one JVM monotonic clock.
The journal records gaps at least 5 ms; wall anchors locate the cutover only.
It is not the sampled 10 ms zero-throughput duration or a CRIU timer.

TTR90 here uses destination steady throughput, a 100 ms rolling window and a
1 s sustained criterion, measured from the client-observed service recovery.
Both recovery start and confirmation are retained. Invalid steady references
must remain invalid. This operational definition and its sampled time origin
must be distinguished from a paper-specific TTR definition.

## Reproduction

Use the matching binaries on both hosts and build/stage the supplied YCSB.
The wrapper previews commands by default; `--execute` runs the experiment and
restores the previous CRIU selection afterwards. Run U and K sequentially.

```sh
python3 scripts/run.py U --profile redis --network-lock nftables --vma-cache --buffered-cutover --stage-max-mb 4 --validation-workers 16 --execute
python3 scripts/run.py K --profile redis --network-lock nftables --vma-cache --buffered-cutover --validation-workers 16 --kernel-export-workers 16 --kernel-export-chunk-mb 64 --execute
```

U retains the 8 GiB PS budget. Reducing it to 4 GiB gave 49.151326 ms downtime
and 7.90 s TTR90 in run `sb_ae_20260927_170101`, versus 46.591907 ms and 1.18 s
with 8 GiB in `sb_ae_20260927_165303`. The smaller budget is not adopted.
Increasing validation workers from 16 to 32 also failed to improve overall
downtime in the measured run (50.894427 ms).

## Remaining K work

The K run `sb_ae_20260927_165625` measured 769.838410 ms client downtime.
Its source final preparation took 203.724 ms (scan 90.914, validation 5.576,
MR export 82.651, hot ordering 24.229 ms). Target catalog setup took 118.239 ms
and PTE/token activation 399.251 ms. These are measured subphases, not a full
additive decomposition of client downtime.

Reaching approximately 40 ms in K requires additional structural work: prepare
MR resources and token objects in PS, reconcile final mappings/PFNs safely,
and make final PTE activation substantially cheaper. The current change does
not implement that work. Pinning mutable source pages early must handle
unmap, COW and epoch changes; token/PTE preparation must preserve fork, teardown
and reference lifetimes. Such kernel work needs isolated fault-injection and
sanitizer validation before host deployment.

## Validation

`tests/downtime/run.sh` packages the production-body regression fixtures.
Normal, ASan/UBSan and TSan runs passed before physical migration tests; the
packaged normal runner was checked again after relocation. Full U and K builds
passed. Physical success requires image checksums, record presence/length and
count, an 8 MiB bytewise SHA-256 canary, all pages drained, and owned-resource
cleanup. The workload's entire 5.12 GB payload is not bytewise verified.

Kernel fault callback aggregate timing includes cache hits and excludes x86
fault entry/exit. It must not be presented as remote-only fault latency or
directly equated to the U request-queue-to-page-install measurement.

## Recorded trials

| Run suffix | Configuration | Client gap (ms) | TTR90 start / confirmation (s) | Reconnect calls |
|---|---|---:|---|---:|
| 162841 | catalog overlap: stage64/PS8192/v16 | 48.716982 | 1.56 / 2.56 | 0 |
| 163224 | catalog overlap repeat | 49.485646 | 1.18 / 2.18 | 0 |
| 163608 | K baseline export4/no-split | 943.411541 | 3.15 / 4.15 | 32 |
| 164005 | validation32, rejected | 50.894427 | 1.25 / 2.25 | 0 |
| 164932 | K shared pagemap/v16/export4 | 903.836205 | 7.05 / 8.05 | 33 |
| 165303 | stage4/PS8192/v16 | 46.591907 | 1.18 / 2.18 | 0 |
| 165625 | K shared pagemap/v16/export16/chunk64 | 769.838410 | invalid steady reference | 32 |
| 170101 | stage4/PS4096/v16, rejected | 49.151326 | 7.90 / 8.90 | 1 |
| 170513 | stage4/PS8192/v16 repeat | 46.954073 | 3.34 / 4.34 | 0 |
| 170833 | K shared pagemap/v16/export16/chunk64 repeat | 752.175618 | 12.48 / 13.48 | 32 |

These are individual trials, not a distribution or a causal ablation study. Both final candidate runs per mode passed migration/data/image checks and cleanup. TTR validity is a separate performance check. All raw run and driver records are preserved outside the source tree. The initial standalone U fixture compile failure in tests-stage14.log was fixed by placing the feature-test macro before includes; tests-stage14-U.log records the successful rerun.
