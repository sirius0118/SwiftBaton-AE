#ifndef __SB_RSOCKET_SNAPSHOT_H__
#define __SB_RSOCKET_SNAPSHOT_H__

#include <stddef.h>
#include <stdint.h>

struct sb_rsocket_range {
    uint64_t offset;
    uint64_t length;
};

/* The source snapshot remains owned by CRIU. The detached listener exits with
 * the page server process; do not unmap the source while it is serving. */
int sb_rsocket_snapshot_serve(const void *source, uint64_t length,
                             const char *bind_ip, int port);

/* Ranges must be disjoint. Each request is at most 16 MiB. Workers own
 * separate connections and reuse them across sequential PS snapshot reads. */
int sb_rsocket_snapshot_read(void *destination, uint64_t length,
                            const char *source_ip, int port,
                            const struct sb_rsocket_range *ranges, size_t count,
                            unsigned workers, uint64_t *payload_bytes);

#endif
