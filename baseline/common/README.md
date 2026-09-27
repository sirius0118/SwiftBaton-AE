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
container images use eight independent data relay pairs, selected round-robin
with `--data-ports` on both sender and receiver. Each segment uses a fresh
rsocket connection and an acknowledgement before the next segment; the whole
tree still has one SHA-256 digest. The relay configures 8 MiB rsocket buffers
and 512 send/receive queue entries before connection setup. This avoids the
credit stall seen with default rsocket settings in a single multi-GiB stream.
`large_transfer_test.py --gib 4.5` passed on Node2 to Node3: 4,831,838,208
bytes over 36 data sessions in 19.6 seconds, with digest verification. That is
about 1.97 Gbit/s of application data; this userspace relay has not saturated
the 25 Gbit/s QoS cap. The standalone test cleans its owned `/tmp` directories.

The transport round trip was checked across Node2/Node3 using 1 MiB in each
direction, with byte-exact SHA-256 verification. An idle RDMA listener now
uses nonblocking accept and sleeps between empty polls; it does not busy-spin.
