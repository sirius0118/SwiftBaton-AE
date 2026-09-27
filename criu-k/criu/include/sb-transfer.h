#ifndef __SB_TRANSFER_H__
#define __SB_TRANSFER_H__
int sb_parallel_negotiate(int socket);
void sb_parallel_prepare_traces(int source, unsigned processes);
/* Final source geometry/validity only, after final images are published. */
void sb_parallel_prepare_catalog(void);
int sb_parallel_server(int socket);
int sb_parallel_client(int socket);
#endif
