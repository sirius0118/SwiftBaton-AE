# K downtime: combined early socket and optional hot hint

This branch combines the audited sparse prearm planner, early target
`lazy-pages.socket`, and omission of an unprepared hot-order hint. All tests
used 500,000 Redis records × 10 KiB, 32 YCSB clients, 25 Gbps hardware QoS,
NUMA node 0, and 16 export/catalog/validation workers. Every listed run
passed migration, endpoint and all-record presence/length checks, 8 MiB
bytewise canary, recovery analysis, zero K module errors, and post-trial
host rollback checks. The all-record check is not a bytewise value checksum.

| PS budget | Run | Client success gap | Source final prepare | TTR90 | Valid PS pages | Unprepared hot ranges |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 256 MiB | `sb_ae_20260928_035913` | 54.89 ms | 11.55 ms | 2.24 s | 54,738 | 0 |
| 64 MiB | `sb_ae_20260928_040248` | **51.84 ms** | 10.14 ms | 2.30 s | 15,517 | 3 |
| 32 MiB | `sb_ae_20260928_040545` | 51.44 ms | 10.40 ms | 2.40 s | 7,629 | 2 |

The 64 MiB run exercised the changed fallback: three ranges had no prepared
hot order, and frozen-stage hot handling took 0.08 ms. In previous runs with
such ranges it took about 2.8 ms. The 32 MiB budget did not further reduce
source final prepare and increased TTR90 by 0.10 s relative to 64 MiB, so
64 MiB is the current practical tradeoff. Individual success gaps vary with
host scheduling; none of these runs met the 50 ms target.
