#ifndef __SB_PROC_H__
#define __SB_PROC_H__
#include <sys/types.h>
/* Private procfs directory descriptors for parallel dump workers. pid=0 is
 * self, pid=-1 is the proc root; positive values name a host process. */
void sb_proc_thread_begin(void);
void sb_proc_thread_end(void);
int sb_proc_thread_active(void);
int sb_proc_thread_open(pid_t pid);
void sb_proc_thread_clear(int root);
int sb_proc_thread_set_root(int fd);
int sb_proc_thread_set_self(int fd);
#endif
