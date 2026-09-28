#ifndef SB_RSOCKET_AS_H
#define SB_RSOCKET_AS_H

#include "sb-rdma-write.h"

/* Three independent rsocket mappings preserve the existing PF, FT and BG
 * receive rings. Data and the publication field are one ordered operation. */
int sb_rsocket_as_start(int control_socket, int source, int port);
int sb_rsocket_as_putv(int lane, const struct write_part *parts, unsigned count);
void sb_rsocket_as_report(const char *side);

enum { SB_RSOCKET_PF, SB_RSOCKET_FT, SB_RSOCKET_BG };

#endif
