# K downtime: PS budget, MR retry, and early hot order

This stage builds on target-side PS import before final freeze. Three changes
reduce source work while Redis is frozen:

1. The trial harness can set the PS page budget. A smaller budget keeps the
   genuine PS path active but lets more source ranges register their RDMA MRs
   during PS. The benchmarked candidate uses 256 MiB, versus the previous
   2 GiB profile. This is a downtime/TTR tradeoff, not a universal default.
2. If a running-source MR is invalidated during its initial writable GUP, the
   controller registers it once more after that COW settles. It captures fresh
   PFNs and notifier status for the new descriptor. The old MR stays owned by
   the session until revoke. Final frozen PFN and notifier checks still decide
   whether the new MR may be reused; any failure falls back to final export.
3. The heat order is built against the PS layout while Redis is still serving.
   Exact final ranges adopt the prepared order. A final subset derives its
   order from the containing PS range; other changed shapes use the original
   frozen-stage builder. Heat order is only a scheduling hint.

All measurements below use 500,000 Redis records × 10 KiB, 32 YCSB clients,
25 Gbps hardware QoS, NUMA node 0, 16 export/catalog/validation workers, and
the same client-success-gap and TTR90 definitions. These are individual runs,
so variation in COW activity, remaining MR regions, and host scheduling is
visible. Every run passed endpoint, all-record presence/length, 8 MiB bytewise
canary, module error counters, and post-trial host health checks. All four K
page paths were active. The all-record check is not a bytewise value checksum.

| Build | PS budget | Run | Client gap | Source final prepare | TTR90 start | Final MR reuse | Final hot reuse |
| --- | ---: | --- | ---: | ---: | ---: | ---: | ---: |
| Stage 26 | 2 GiB | `sb_ae_20260928_015020` | 122.00 ms | 57.05 ms | 2.09 s | 298/419 | — |
| Stage 26 | 2 GiB | `sb_ae_20260928_015335` | 118.17 ms | 55.60 ms | 1.77 s | 242/413 | — |
| Target preimport only | 256 MiB | `sb_ae_20260928_025427` | 85.61 ms | 37.28 ms | 2.38 s | 321/411 | — |
| Preimport + MR retry | 256 MiB | `sb_ae_20260928_030342` | 82.63 ms | 34.69 ms | 2.33 s | 386/421 | — |
| Preimport + MR retry | 512 MiB | `sb_ae_20260928_030648` | 98.15 ms | 47.66 ms | 2.27 s | 343/409 | — |
| Combined | 256 MiB | `sb_ae_20260928_031318` | **73.78 ms** | 23.82 ms | 2.42 s | 398/415 | 409 exact, 6 derived |
| Combined | 256 MiB | `sb_ae_20260928_031627` | **91.28 ms** | 40.14 ms | 2.24 s | 401/420 | 419 exact, 1 fallback |
| Combined | 64 MiB | `sb_ae_20260928_032013` | 74.59 ms | 25.64 ms | 2.38 s | 406/416 | 413 exact, 3 fallback |

In the two combined 256 MiB runs, initial notifier invalidation affected 78
and 51 registrations; one retry recovered every one. Final source MR export
was 8.25 ms and 20.32 ms, respectively, explaining much of the client-gap
spread. Reducing PS to 64 MiB offered no clear downtime gain and kept far
fewer valid PS pages; 512 MiB gave a small TTR gain with a larger gap. The
combined implementation remains above the 50 ms client-gap target.

One 100k-record smoke run after introducing retry had zero final MR reuse
despite valid initial captures; migration and bytewise canary checks passed.
The immediately repeated smoke reused all 176 eligible MRs, as did later
large runs for their valid exact ranges. The cause of the one-run fallback is
not established. It is a safe fallback but a performance variability risk.

The trial script restores original modules and CRIU symlinks after each run.
`health-arm-plan.py` passed after every trial; there was no new host kernel
boot. The checked-in branch is opt-in through `--kernel-ps-mr`; its PS budget
is selected by the run command, not hard-coded into CRIU.
