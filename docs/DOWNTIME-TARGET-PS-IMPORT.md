# K target PS import before final freeze

The destination previously moved cached PS pages from PS regions into final
regions only after receiving the frozen source's final records. That import was
inside the client interruption window. This branch moves the cached pages into
the prepared destination layout immediately after all PS receive workers join,
before the PS ACK and while the source still serves requests. Final dirty/PFN
validation and `SBK_IOC_SEAL_REGION` remain mandatory before any destination
PTE is exposed.

The feature is enabled only with `--kernel-ps-mr` (which requires PS ARM). If a
final range differs from its prepared layout, it does not take a cached page
from an unbound prepared context: its missing pages are fetched from the frozen
source after resume. This conservative fallback preserves correctness at the
possible cost of some PS benefit. A catalog without prepared regions retains
the original final import behavior.

Physical Redis profile: 500,000 records × 10 KiB, 32 YCSB clients, 25 Gbps
hardware QoS, 16 source export/target catalog workers, 16 validation workers,
NUMA node 0. The client gap is the intersection between successful DB returns
across workers. All runs checked the endpoint, all 500,000 keys for presence and
value length, the 8 MiB bytewise canary, and K transport counters; no page
errors were reported. The key check is not a full value checksum.

| Build | Run | Client gap | Source final preparation | Target catalog seal | Early PS import | Prepared layouts reused |
| --- | --- | ---: | ---: | ---: | ---: | ---: |
| Stage 26 baseline | `sb_ae_20260928_015020` | 122.00 ms | 57.05 ms | 13.50 ms | — | — |
| Stage 26 baseline | `sb_ae_20260928_015335` | 118.17 ms | 55.60 ms | 12.25 ms | — | — |
| Target preimport | `sb_ae_20260928_024239` | 110.92 ms | 54.24 ms | 5.72 ms | 27.98 ms outside downtime | 414/416 |
| Target preimport | `sb_ae_20260928_024546` | 126.67 ms | 58.59 ms | 9.26 ms | 28.05 ms outside downtime | 408/415 |

The target seal reduction repeats, but the two client gaps straddle the
baseline. No statistically supported end-to-end improvement or sub-50 ms
claim follows from these runs. TTR90 start against the stable final 10-second
reference was 1.99 s and 2.05 s; the two baseline values were 2.09 s and
1.77 s. Both experiments with preimport exercised discarded prepared plans
(2 and 7 respectively), and full migration validation passed.

The trial harness restores the original source and destination module build
IDs, CRIU symlinks, kernel boot IDs, QoS, and AE-owned resources after each
run. `health-arm-plan.py` passed after each trial. No new physical kernel was
loaded or booted for this branch; only the stage-26 candidate source module,
known target module, and this CRIU were used temporarily.
