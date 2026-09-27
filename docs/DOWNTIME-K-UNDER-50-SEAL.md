# SwiftBaton-K AE downtime milestone: one valid run below 50 ms

This version combines PS source MR prearm with notifier-guarded sparse final
planning, target PS import, cached hot order with optional fallback omission,
early target lazy-page socket binding, and parallel per-record final catalog
validation. Cross-record overlap/identity validation remains serial before
publishing any final entry. Catalog validation uses one thread for fewer than
64 records to avoid launch overhead.

The fixed workload is 500,000 Redis records × 10 KiB, 32 YCSB clients,
25 Gbps hardware QoS, NUMA node 0, 16 export/catalog/validation workers,
and a 64 MiB PS budget. Both listed runs passed endpoint, all-record
presence/length, 8 MiB bytewise canary, source retirement, recovery analysis,
K error counter (zero), and post-trial host health/rollback checks. All four
K page-transfer paths carried pages. The all-record check is not a bytewise
value checksum.

| Run | Client success gap | Source final prepare | Target catalog validation | TTR90 | PF / FT / BG / valid PS pages |
| --- | ---: | ---: | ---: | ---: | --- |
| `sb_ae_20260928_041615` | 51.37 ms | 14.08 ms | 0.81 ms | 2.39 s | 31,031 / 134,264 / 1,381,941 / 15,486 |
| `sb_ae_20260928_041919` | **46.71 ms** | 9.29 ms | 0.80 ms | 2.35 s | 31,259 / 134,468 / 1,381,489 / 15,502 |

The 46.71 ms result meets the requested one-run milestone. The preceding
51.37 ms run with the same binary did not meet it; source MR export was
9.21 ms versus 4.18 ms in the under-50 run. Do not present this version as
reliably sub-50 ms without additional repeated trials. The catalog validation
fell from roughly 3.1 ms in prior serial runs to ~0.8 ms in both runs.

The normal, ASan, and TSan catalog ownership fixtures passed. Their small
catalog fixtures exercise the serial fallback; the full 400+ region host
runs exercised parallel validation. After each host trial, original CRIU
binaries and K modules were restored without reboot, and host health passed.
