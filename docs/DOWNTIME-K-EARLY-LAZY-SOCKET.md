# K downtime: bind the target lazy-pages socket during PS

Previously the target page-client bound `lazy-pages.socket` only after final
images arrived and its final K catalog was sealed. Restore reached
`prepare_lazy_pages_socket()` first and busy-waited for the socket. A full
Redis run spent 11.50 ms there. This change binds/listens after PS reception,
while the source application is still running. The page-client still waits
for final images and catalog validation before accepting or serving the
connection. Unix socket backlog queues the early connection; no K page is
served from an unsealed catalog.

Smoke `sb_ae_20260928_035309` (100k × 1 KiB, 16 clients) passed full
migration, Redis/canary validation, recovery analysis, and host rollback.
Restore socket wait was 0.029 ms and client success gap 38.95 ms.

Full `sb_ae_20260928_035454` (500k × 10 KiB, 32 clients, 25 Gbps QoS,
NUMA node 0, PS budget 256 MiB) passed all-record presence/length,
8 MiB bytewise canary, endpoint, source retirement, recovery analysis, K
error counter, and host rollback. Socket wait was 0.031 ms, client success
gap 54.13 ms, and TTR90 2.24 s. The source final prepare was 13.96 ms,
including 2.76 ms building an unmatched hot-order hint. The 50 ms target is
not yet met. The all-record check is not a bytewise value checksum.
