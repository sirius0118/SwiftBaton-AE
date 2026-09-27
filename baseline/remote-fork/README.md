# Demand-only kernel remote-fork profile

`baseline/run.py remote-fork` uses a separate `build/criu-K-baseline` binary.
The destination catalog sets `SBK_DEMAND_ONLY=1`: no PS payload, pre-transfer,
prefetch or background batch sweep runs while Redis serves. The kernel
intercepts remote page faults and installs the fetched page. Source memory
remains exported until the measured workload and full record validation end;
the driver then stops the target and retires all unaccessed markers before
revoking the source MR. This is a demand-only experimental profile built on
SwiftBaton-K's control plane, not an independent remote-fork implementation.

The control session must outlive the service period. A fixed 300-second CRIU
socket/serve timeout caused the first 300-second trial to terminate when
full-key validation began. The baseline K binary uses a bounded 3,600-second
timeout; the sealed SwiftBaton-K binary is unchanged. The 100,000 × 1 KiB
regression trial `sb_ae_20260928_072847` passed all indexed keys and the
8 MiB canary: 42,149 pages were demand-fetched, zero pages used PS/FT/BG,
and 825 unaccessed markers were retired after target exit.

Run a matched full trial with
`python3 baseline/run.py remote-fork --profile redis --threads 16 --duration 300 --warmup 20 --execute`.
The 500,000 × 10 KiB run `sb_ae_20260928_073050` passed complete key-length
and canary validation. It fetched 1,545,059 pages through demand faults,
used zero PS/FT/BG pages, recorded zero transfer errors and retired 17,422
untouched markers after the target stopped. The client-wide success gap was
1,023.327 ms; stable target throughput was 43.5k operations per second, and
TTR90 began 89.780 seconds after service resumed. These are a deliberately
minimal demand-only baseline's performance, not SwiftBaton-K's performance.
Node3 also logged hung-task warnings for inactive FT/BG dispatcher threads
during the long demand-only run. The migration completed, the module reported
zero transfer errors and its reference count returned to zero; the warning
behavior remains to be cleaned up separately.
