# Rsocket baseline transport

The U baseline profiles use a separate CRIU build and opt in with
`--rsocket-as`; the sealed SwiftBaton-U and SwiftBaton-K builds are unchanged.
The option requires `--parallel-transfer --sync-fault-transport`. CRIU images
are sent over a persistent rsocket direct-write connection, and the PS
snapshot uses four persistent rsocket read lanes. The PS snapshot also serves
the PCLive second-round changed runs. During AS, demand faults, adjacent
prefetch and background transfer have distinct rsocket direct-write lanes.
The page lanes use `riomap` and `riowrite`; a small stream message publishes
each completed write and the receiver acknowledges it before the sender
reuses the source buffer. Each lane has its own write lock and receiver thread,
so a background queue lock cannot hold a demand-fault transport lock.

The ports are the migration control port plus 8 for PS, plus 9 through 11 for
AS, and plus 12 for images. Both hosts must have the same baseline binary
hash. `baseline/as-rsocket/build.sh --stage-target` builds/stages the binary
for post-copy and hybrid; `baseline/pclive/build.sh --stage-target` builds the
PCLive binary. `baseline/run.py` checks the hashes, selects those binaries
only for its run, verifies all image phases, PS byte counts and AS lane
write/receive/acknowledgement counts, then restores `/usr/bin/criu` and cleans
the owned containers. The PCLive run additionally verifies the changed-page
delta and resident refresh.

The demand-only kernel remote-fork profile retains its kernel RDMA transport:
a kernel fault handler cannot call the userspace librdmacm rsocket API. All
profiles run under the same 25 Gbps hardware QoS cap, but the kernel row is
not an identical userspace transport comparison.
