# Post-copy algorithm profile

`baseline/run.py postcopy` selects the separate U baseline profile. It does
not preload pages in PS. After the destination resumes, a fault lane handles
immediate demand reads while independent background workers walk the missing
page index in address order. The profile disables pre-transfer, prefetch and
hot-first hints to isolate demand plus batch transfer. It reuses the existing
SwiftBaton migration orchestration and native ibverbs page transport.

Run a matched Redis/YCSB trial with
`python3 baseline/run.py postcopy --profile redis --threads 16 --duration 300 --warmup 20 --execute`.
The verified 500,000 × 10 KiB trial `sb_ae_20260928_070539` transferred
73,581 pages through demand fault and 1,663,096 through background workers;
pre-copy and prefetch transferred zero. All indexed key lengths and the 8 MiB
bytewise canary passed. The client-wide success gap was 261.295 ms and TTR90
began 3.640 seconds after client service resumed. The stable target reference
was 41.7k operations per second.
