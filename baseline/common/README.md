# Shared RDMA baseline transport

Build on each Linux host with `make`; requires rdma-core development headers
and libraries. The source relay accepts only RDMA on the supplied RDMA NIC IP,
connects each accepted session to a loopback TCP server, and spawns an isolated
bridge. The target relay accepts only loopback TCP and opens one rsocket RDMA
connection per local client. Never bind the TCP side to a host-facing IP.

On Node3, for an image receiver at `127.0.0.1:19510`:

```sh
./rsocket_relay listen 10.0.0.63 19511 127.0.0.1 19510
```

On Node2:

```sh
./rsocket_relay connect 127.0.0.1 19512 10.0.0.63 19511
```

`tree_stream.py send IMAGE_DIR --port 19512` on Node2 connects to that local
port; `tree_stream.py receive IMAGE_DIR --port 19510` on Node3 waits locally.
Both ends emit the same stream digest. The receiver publishes the destination
directory only after validating the digest. The relay records per-session byte
counts, elapsed time and transfer errors.

The tree stream separates control metadata from 128 MiB data segments. Large
container images use up to eight independent data relay pairs concurrently,
configured by `--data-ports` on both sender and receiver. Each lane sends its
assigned segments at fixed offsets, using a fresh rsocket connection for each
128 MiB segment. Long-lived per-lane connections stalled partway through a
real 6.4 GB Redis image, so the per-segment connection bound is required.
The tree SHA-256 commits to each segment's SHA-256 in offset order,
so both sides can hash concurrently without a second read of the image. The
receiver publishes the image tree only after the digest matches. The relay
configures 8 MiB rsocket buffers and 512 send/receive queue entries before
connection setup. This avoids the credit stall seen with default rsocket
settings in a single multi-GiB stream.

`large_transfer_test.py --gib 4.5` passed on Node2 to Node3: 4,831,838,208
bytes over 36 data sessions with at most eight concurrent in 3.520 seconds,
or about 11.0
Gbit/s of application data. The test wrote distinct markers at every segment
boundary and verified the complete source and target file SHA-256 after the
stream finished. The older one-session-at-a-time implementation took 19.6
seconds on the same size. The synthetic file is mostly sparse zeros; the
measured throughput is a transfer test, not an end-to-end Redis result. The
relay still does not saturate the 25 Gbit/s QoS cap. The standalone test cleans
its owned `/tmp` directories.

The transport round trip was checked across Node2/Node3 using 1 MiB in each
direction, with byte-exact SHA-256 verification. An idle RDMA listener now
uses nonblocking accept and sleeps between empty polls; it does not busy-spin.
