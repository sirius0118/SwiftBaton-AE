# K anonymous marker activation: batched MM bridge

Status: physically tested on Node3 after an authorized one-shot kernel boot.
Both modes below 50 ms remains unmet. The 56 ms U release and previous candidate
are preserved. Separate VM setup measurements from client-observable downtime.

Apply `kernel/patches/linux-5.15.167-sbk-arm-batch.patch` after the existing K1
PTE patch. It retains the original ABI and fault handler and changes setup only:

1. Acquire token references in bounded batches of 64 under the XArray lock.
   No lock is held over the entire region; rescheduling remains possible.
2. Walk the upper tables once per PMD and inspect a PTE page under one PTE lock.
   Reject existing PTEs and huge/special mappings before inserting markers.
3. Transfer the acquired lookup reference to each installed marker, instead of
   incrementing and then decrementing a second reference for every page.
4. Add the shared anonymous-anchor references in one operation while holding
   `mmap_write_lock`, before any rollback or unlock can permit retirement.

Caller-owned creator references remain unchanged. Missing lookups release every
reference already acquired. Partial insertion rolls back only the installed
prefix. The duplicate-token test explicitly exercises this path. Existing PTEs
are never overwritten. No application page validity check is omitted.

`arm_timing=1` on the development module prints allocation, token binding,
bridge and creator-reference-release durations; it is off by default. Serial
console output adds overhead to the outer ioctl timer. Do not use a timed
logging run to claim the production wall time.

`kernel/tests/arm-bench.c` measures CONFIG, reservation and ARM separately,
checks sampled page contents and exact retirement, and labels its result as a
setup microbenchmark. It does not test RDMA or container/client availability.
`kernel/vm/arm-validation.py` builds the real production and bridge fixture
modules, boots only an isolated kernel, checks the original full anonymous
suite and the new ownership/rollback fixture, and rejects kernel warnings,
unexpected taint or failed module unload. Without `--ofed`, it also runs RXE.
See `kernel/tests/bridge/README.md` for exact coverage and the initial fixture
command-number correction.

Example (all paths supplied explicitly):

```sh
python3 kernel/vm/arm-validation.py --kernel /path/to/patched/kernel \
  --output /path/to/new/validation-directory
```

Use a KASAN/lock-debug kernel for safety tests. For a matching performance kernel
and OFED build, add `--ofed /path/to/ofed --bench`. Do not run heavy builds at the
same time as the before/after timing trials.

## Isolated before/after results

The performance and KASAN/RXE kernels passed the full suite plus the rollback
fixture. Performance trials ran in A-B-B-A kernel order with timing logs off;
each boot measured three trials per size. No heavy build ran concurrently.

| Range | K1 ARM median (6 trials) | Batched ARM median (6 trials) | Reduction |
|---|---:|---:|---:|
| 64 MiB / 16,384 pages | 2.226666 ms | 1.394369 ms | 37.38% |
| 1 GiB / 262,144 pages | 82.145421 ms | 42.494114 ms | 48.27% |

These remain VM setup microbenchmarks, not measured client downtime. The entire
kernel configuration and all 11,981 kernel-export CRCs match K1. Candidate
bzImage SHA-256 is `4268bfc5f5eda3bd13ffbb9cfb7969062c8acfbbfbf3d12c9cd858880c12504e`;
its release string remains `5.15.167-swiftbaton-k1` for the unchanged module ABI,
so physical validation must check the boot image and ELF notes, not just uname.
The candidate uses a separate one-shot boot entry; neither the baseline image
nor the original default entry is overwritten.

## Physical migration results, 2026-09-27

The runtime kernel ELF notes match the candidate; Node3 boot ID is
`3220ec35-2ba6-4c80-93ee-ed1eba296d97`. The source kernel stayed unchanged. A
single reboot completed in about 9 minutes 22 seconds. The preexisting paused VM
was saved and restored **paused**; the existing memcached container and managed
Ceph exporter are running. The candidate runs now; the old boot images and
explicit original default remain available. The release string alone does not
distinguish these kernels: use ELF notes and the separate boot image path.

The physical module was unchanged from the token-pool trial (`fab88287…`). The
new optional module phase logger is not part of that binary and was not needed
for end-to-end timing. CRIU binaries remain U `77d7ba27…` and K `41649fc5…`.

| Full trial | Client gap | TTR90 start / confirmation | Reconnect calls |
|---|---:|---:|---:|
| K 185632 | 579.986813 ms | 5.11 / 6.11 s | 32 |
| K 185907 | 578.289817 ms | 1.49 / 2.49 s | 32 |
| U 190237 | 40.371978 ms | 1.24 / 2.24 s | 0 |

All used 500,000 records × 10,240 bytes, 32 clients and 25 Gbps. All TTR references
passed validity checks; TTR90 uses the prior target-final-steady reference,
100 ms rolling window and 1 second confirmation. A few runs do not establish a
stable latency ceiling or a TTR improvement. Client gaps retain the same
intersection-of-success-gaps measurement, not the network gate ACK duration.

K ARM fell from 308.855–310.031 ms to **206.570 / 205.146 ms**. Catalog setup was
97.660406 / 104.693691 ms. Source final preparation was 235.531 / 226.627 ms,
including scan 131.224 / 111.842, validation 6.632 / 5.074, MR registration
69.961 / 82.095, and hot-order preparation 27.339 / 27.274 ms. The source was
slower than the preceding 174–184 ms trials; do not present the client delta as
a clean isolated ARM-only improvement, or add overlapping stage times blindly.

The first small smoke (185500) passed at 87.285183 ms, but used postboot PCP
trust/vendor TSA, with all priorities still in capped TC1. The coordinator was
paused while that child completed and cleaned up. DSCP trust and strict TC1
scheduling were restored before **both** full trials, with no QoS change during
an active migration. This smoke is retained as correctness evidence rather
than a matched performance comparison.

All successful migrations verified records/counts/lengths, the 8 MiB canary hash,
images, four transfer paths, source retirement and cleanup. These checks do not
constitute a bytewise hash of the entire changing 5.12 GB dataset. K reported
zero page errors. Original runtime modules and CRIU symlinks were restored after
the experiments; the new candidate kernel remains running. Final health checks
verified exact loaded module build IDs, zero module references, no owned
containers/gates/queues, the paused VM, CPU settings and 25 Gbps/DSCP/strict TC1.
The boot log includes the preexisting configuration warning about unprivileged
BPF with eIBRS; no new BUG/Oops/panic or runtime lockup was observed.

Three real ConnectX cold-page tests on the candidate, with PS/FT/BG off and
1,024 unique 4 KiB loads per trial, measured application-load-to-resumption means
**9.198 / 8.919 / 9.613 µs**, p99 **15.209 / 10.077 / 15.524 µs**. The probe binary
is identical to the previous run. These are isolated hardware microbenchmarks,
not the fault distribution during migration; no fault-latency improvement is
claimed from this small comparison. U's separate queue-to-install scope must
not be directly compared with the K application timer.

Next large K costs remain source preparation, directory construction and marker
activation. A draft for parallel final-context allocation exists locally but
is not part of this commit, binary or these results. It requires concurrency,
failure-join and FD-ownership tests before deployment.


## Follow-up correction: effective K validation workers

The later catalog investigation found that the K adapter discarded the requested
`--validation-workers` setting when generating K configuration. The K runs in
this checkpoint therefore used the default of 1 despite commands requesting 16.
The measured numbers are unchanged. Commit `ff399036` fixes the propagation;
see [the follow-up experiments](DOWNTIME-CATALOG.md) for corrected runtime
settings, unsuccessful trials and the current limits.
