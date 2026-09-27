# SwiftBaton-K downtime: batched exact conntrack deletion

This follow-up changes only the experiment-scoped client cutover. Once the
pre-conntrack OUTPUT gate is closed and the source is isolated, the listener
enumerates live client-to-source TCP sockets through Linux INET_DIAG. A kernel
destination-port filter reduces reply traffic; userspace still checks both
addresses and the port. It sends exact original-tuple deletes in one netfilter
netlink batch, waits for every ACK, and treats all errors except ENOENT as
fatal. The gate stays closed until destination NAT is installed and restored
tasks have resumed. No wildcard conntrack flush is used.

The earlier 8-thread libnetfilter prototype was rejected: on a 16-client
smoke migration it took 15.13 ms to enumerate/delete 18 tuples, slower than
historical 11–14 ms runs. Under a live YCSB workload, INET_DIAG and the old
`/proc/net/tcp*` parser returned identical 18- and 34-socket maps. A final
smoke migration (`sb_ae_20260928_052146`) passed all AE checks and rollback;
its exact-tuple operation took 8.08 ms for 18 tuples, including 7.09 ms for
enumeration.

The full 500,000 × 10 KiB Redis / 32 YCSB client run
`sb_ae_20260928_052344` used 64 MiB PS budget, 25 Gbps hardware QoS,
NUMA node 0, and 16 K export/catalog/validation workers. It passed 500,000
record presence and length checks, the 8 MiB bytewise canary, destination
endpoint, source retirement, zero K transfer errors, four-path page movement,
and source/target module and CRIU rollback. The all-record check is not a
bytewise checksum of every value. The client success interval was **49.315 ms**
and the gate hold was **47.805 ms**. All 33 exact conntrack tuples were deleted
in **6.258 ms**, of which 5.086 ms was socket enumeration; the prior
same-workload sequential runs were typically 11–12 ms. Packet and netlink
queue drops were both zero. K transferred PF 30,660, FT 132,838, BG
1,383,800 pages and retained 15,434 valid PS pages.

This is a measured sub-50 ms downtime release, not a statistical guarantee.
The full run's TTR analyzer marked its retrospective target throughput
reference unstable (`ttr_valid=false`), so it does not provide a valid TTR
comparison. The previously sealed tag `swiftbaton-k-ae-under50-20260928`
remains available; this release changes only the network cutover helper.

The next work item is the RDMA baseline suite under `baseline/`.
