# Anonymous bridge ownership tests

This module is an isolated VM fixture, not part of the production module. Run it
through `kernel/vm/arm-validation.py`; do not install it on migration hosts.

It calls the actual exported `sbk_pte_arm()` and creates real tokens. Each of 12
iterations checks a 513-page range crossing a PTE-table boundary:

- successful marker installation followed by unmap;
- a duplicate token, forcing failure *after* one marker has been installed;
- a missing token after at least one 64-token lookup batch;
- a preexisting final PTE, which must reject the operation without changing it
  or exposing markers in the earlier empty pages.

Every case verifies that exactly all created tokens retire and that the provider
is never unexpectedly called. Failed ranges are read to verify ordinary zero
pages and preservation of the preexisting byte. Module unload proves no token
still retains the fixture module. The ordinary SwiftBaton suite separately tests
actual data fetching, fork/COW, mremap, protection, background installation,
pretransfer, drain and cancellation.

The fixture uses typed `_IOW/_IOR` ioctl commands so they do not collide
with filesystem commands dispatched before the driver's ioctl.
