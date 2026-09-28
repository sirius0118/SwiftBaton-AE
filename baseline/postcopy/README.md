# Post-copy algorithm profile

`baseline/run.py postcopy` selects the separate U baseline profile. It does
not preload pages in PS. After the destination resumes, a fault lane handles
immediate demand reads while independent background workers walk the missing
page index in address order. The profile disables pre-transfer, prefetch and
hot-first hints to isolate demand plus batch transfer. It reuses the existing
SwiftBaton migration orchestration. The baseline CRIU build sends images,
the PS control snapshot and three AS lanes through librdmacm rsockets.
Demand fault and background pages use separate direct-write connections.

Run a matched Redis/YCSB trial with
`python3 baseline/run.py postcopy --profile redis --threads 16 --duration 300 --warmup 20 --execute`.
The verified 500,000 × 10 KiB rsocket trial `sb_ae_20260928_095818`
passed all indexed key-length and 8 MiB bytewise-canary checks. The client-wide
success gap was 397.287 ms, the stable target rate was 42,950 operations per
second, and TTR90 began 33.020 seconds after service resumed. Source and
target image phases, PS byte count, and AS lane write/receive/acknowledgement
counts agreed with zero transport errors. This one-run result includes the
current rsocket per-write acknowledgement cost; it is not a controlled estimate
of the post-copy algorithm alone. The older native-ibverbs trial
`sb_ae_20260928_070539` measured a 261.295 ms gap and 3.640 s TTR90 start.
