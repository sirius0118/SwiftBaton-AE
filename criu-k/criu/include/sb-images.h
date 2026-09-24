#ifndef __CR_SB_IMAGES_H__
#define __CR_SB_IMAGES_H__
#include <sys/types.h>
int sb_images_source_prepare(pid_t pid);
int sb_images_init(int socket, int source);
int sb_images_publish(int socket, unsigned int phase);
int sb_images_receive(int socket, unsigned int phase);
#endif
