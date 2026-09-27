# Final catalog and effective K worker settings

**Status: neither mode is demonstrated to stay below 50 ms; the joint 40 ms objective remains unmet.** The frozen 56 ms release is unchanged. This checkpoint includes unsuccessful/slow experiments.

All full trials used 500,000 records × 10,240 bytes, 32 clients and the existing 25 Gbps cap. U uses an 8 GiB pretransfer budget, 4 MiB parent-stage budget and NUMA node 1; K uses 2 GiB pretransfer and NUMA node 0. Mode comparisons must retain these different PS budgets.

| Run | Mode / profile | Effective scan / catalog workers | Client downtime (ms) | Valid TTR90 start / confirmation (s) | Status |
|---|---|---|---:|---|---|
| 192405 | K / smoke | 1 / 1 | 57.716707 | 1.110 / 2.110 | PASS |
| 192534 | K / redis | 1 / 1 | 568.712640 | invalid reference | PASS |
| 192907 | K / smoke | 16 / 16 | 157.282968 | 1.100 / 2.100 | PASS |
| 193046 | K / redis | 16 / 16 | excluded | not accepted | FAIL: 48-page deficit |
| 193719 | K / smoke | 16 / 1 | 58.054604 | 0.110 / 1.110 | PASS |
| 193846 | K / redis | 16 / 1 | 515.463705 | 9.115 / 10.118 | PASS |
| 194123 | K / redis | 16 / 1 | 476.338153 | 1.530 / 2.530 | PASS |
| 194435 | U / redis | 32 / — | 53.930912 | 1.970 / 2.970 | PASS |
| 194816 | U / redis | 16 / — | 56.359501 | 1.440 / 2.440 | PASS |

The smaller smoke workload is only a correctness/diagnostic check. Downtime retains the intersection of successful-operation gaps across all clients in the same JVM monotonic clock. Gate ACK timing and zero-throughput samples are not substitutes. TTR90 uses the valid target-final-steady reference, 100 ms rolling windows and a 1 second hold; start and confirmation are both shown. K full successful trials had 32 reconnect calls each; both U trials had 0.

## What changed

The K adapter previously accepted `--validation-workers 16` in the experiment command but omitted it from the actual K CRIU config and page-client arguments. The recorded request was 16, while execution used the default of 1. Commit `ff399036` propagates it through the shared K settings. A regression test covers config/CLI propagation and a requested-versus-executed mismatch. Target final-catalog runtime settings are now checked by the experiment driver.

The catalog implementation prepares independent final CONFIG contexts with bounded worker concurrency, joins all admitted helpers before publishing descriptors, and frees unpublished contexts on failure. If thread creation fails, the caller and admitted helpers finish the remaining jobs. Existing PS-slice import/seal order remains serial. Source pagemap reading and sparse planning have separate timers.

Physical testing showed that parallel catalog allocation was counterproductive: the 16-worker smoke spent 102.700 ms preparing contexts, versus 2.732 ms in the serial control; the full 16-worker run spent 117.467 ms in preparation. Therefore commit `4a8752df` separates `--kernel-catalog-workers` from source `--validation-workers`; catalog defaults to 1. Allocation launch/sum/max/peak counters support further investigation. Do not describe this parallel allocator as a measured improvement.

The retained source change is effective 16-worker pagemap reading. The full serial control read pagemap in 107.974 ms; retained full trials used 9.438 and 9.160 ms. Sparse planning remained 9.174 and 8.138 ms. Source final preparation was 148.707 and 123.628 ms, including MR export of 100.948 and 77.272 ms and hot-order work of 24.349 and 24.203 ms. Catalog control remained 114.148 and 108.425 ms, and PTE ARM remained 206.524 and 203.337 ms. These stage measurements explain the remaining large gap to 40 ms; phase overlaps must not be summed indiscriminately.

U executable code was unchanged. Its 32-worker run measured 53.930912 ms with 10.065 ms final validation. The subsequent 16-worker control measured 56.359501 ms with 13.556 ms final validation. These two noisy runs do not establish either a worker-count benefit or a regression caused by 32 workers. The prior 40.371978 ms result remains a historical observation, alongside earlier 41.574479 / 43.059336 / 51.676828 ms runs; it is not a stable bound. Retain the prior 16-worker configuration pending source scheduling and client-side pause attribution.

## Failed trial and correctness limits

Full run 193046 failed strict page ownership accounting: PS 515678 − invalid 60499 + PF 20383 + FT 84910 + BG 1002316 = 1562788, while total pages were 1562836 (48 short). Page errors were zero and all migration commands terminated, but this is not sufficient to accept the run. It is excluded from successful downtime/TTR results. Its exact cause is unresolved. A possible discard of unfetched markers requires evidence; it is not treated as established fact.

Post-drain diagnostics now enumerate page states only for mismatched regions, after confirming that every marker retired and every admitted worker joined. The validator retains the original exact ownership equality. Subsequent serial-catalog full runs had zero deficit; that does not retroactively validate the failed parallel run or prove its cause.

Successful runs passed record count/presence/length checks, the 8 MiB bytewise canary hash, image checks, four nonzero memory paths, source retirement and cleanup. These checks are not a bytewise hash of the entire changing 5.12 GB dataset. The mock fixtures passed normal, ASan/UBSan and TSan, including CONFIG/WATCH failures, partial/zero helper-creation fallback, joins, PS slice imports, descriptor cleanup and post-drain audit. Mock tests do not establish hardware performance.

## Fault measurements

The unchanged physical ConnectX cold-page probe used 1,024 unique 4 KiB faults per repeat, NUMA 0, no PS/FT/BG, initially nonresident destinations and content checks. Application load-to-resumption means (µs): 9.054, 9.048, 9.104; p99: 12.559, 12.092, 12.284. Clock-pair overhead was 21 ns, not subtracted. Probe SHA-256 is `cb2b0ec3df083572311534a28226281e3f1ee691921c171b8a31aabf9de818b2`. These are isolated microbenchmarks, not container faults under migration load.

U 194435 queue-to-install: n=4024, mean 18.632 µs, p99 29.403 µs, max 3646.914 µs; dropped samples={'source': 0, 'target': 0}. This scope differs from the K application timer.
U 194816 queue-to-install: n=7275, mean 18.038 µs, p99 35.428 µs, max 153.974 µs; dropped samples={'source': 0, 'target': 0}. This scope differs from the K application timer.

## Reproduce the retained configuration

Use the same verified ARM-batch kernel and the token-pool module from the previous checkpoint. K module loading/rollback is separately guarded by the archived coordinator; `scripts/run.py` does not load that candidate automatically.

```sh
python3 scripts/run.py K --profile redis --network-lock nftables --vma-cache \
  --buffered-cutover --validation-workers 16 --kernel-catalog-workers 1 \
  --kernel-export-workers 16 --kernel-export-chunk-mb 64 --execute

python3 scripts/run.py U --profile redis --network-lock nftables --vma-cache \
  --buffered-cutover --validation-workers 16 --stage-max-mb 4 \
  --precopy-limit-mb 8192 --execute
```

Current K binary SHA-256 `59b479d9e6bdeb1cab030f6344d630b64c7d62974fb9506a298d92d8cef2a01b`; initial parallel/control binary `17d4f35f0a7277e8834387f425357766abe992a6519dca2a44bc2c6aec45e253`. Current implementation commit is `4a8752df56206a17142770524cf34c33384c94c9`. The source bundle preserves both adapter versions and the frozen tag.

The physical kernel and module were unchanged this round: boot `3220ec35-2ba6-4c80-93ee-ed1eba296d97`, ARM-batch kernel hash `4268bfc5…`, module `fab88287…`. Original source/target runtime modules (actual loaded ELF build IDs) and CRIU symlinks were restored after testing. Final checks passed for module references, owned experiment cleanup, the preexisting paused VM, CPU settings and original 25 Gbps QoS. The already-running ARM-batch kernel remains in place with its original fallback images/default preserved.

Next work: resolve the 48-page deficit with stable per-region evidence; move K context/PTE preparation and final MR work earlier with explicit ownership/dirty-page checks; attribute U source-validation and post-release client pauses without changing the downtime definition. This checkpoint is progress, not goal completion.
