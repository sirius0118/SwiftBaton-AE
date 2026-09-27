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

The transport round trip was checked across Node2/Node3 using 1 MiB in each
direction, with byte-exact SHA-256 verification. An idle RDMA listener now
uses nonblocking accept and sleeps between empty polls; it does not busy-spin.
