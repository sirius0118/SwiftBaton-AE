#ifndef __CR_MUL_UFFD_H__
#define __CR_MUL_UFFD_H__

#include "linux/userfaultfd.h"
#include "pstree.h"

#define REGION_SIZE (unsigned long)(1UL * 1024 * 1024 * 1024 * 50)
#define MAX_VMA_NUM 2000
#define MAX_UFFD_NUM 20

#ifdef MUL_UFFD
// #define ioctl_mul(&uffdio_copy, pid) ioctl_mul(&uffdio_copy, pid)
#endif
struct vma
{
    unsigned long start;
    unsigned long end;
};

/* 一个uffd可能映射了多个vma，组成一个区域。*/
struct uffd_region
{
    int uffd;
    // start和end是uffd的映射区域最小地址和最大地址，用于快速查找地址是否包含在本region
    unsigned long start;
    unsigned long end;
    struct vma vma[MAX_VMA_NUM];
    int nr_vma;
};

struct pid_uffd_region_set
{
    unsigned long pid;
    struct uffd_region uffd_region[MAX_UFFD_NUM];
    int nr_uffd_region;
};

extern volatile struct pid_uffd_region_set *PidUffdSet;
extern int item_num;
extern uint64_t pidset[MAX_PROCESS];


extern int InitPidUffdSet(void);
int sb_uffd_lifecycle_start(void);
int sb_uffd_lifecycle_next(int pid, uint64_t *address);
int sb_uffd_lifecycle_next_batch(int pid, uint64_t *addresses, unsigned capacity);
int sb_uffd_lifecycle_finish(void);
extern void sb_uffd_send_lock(void);
extern void sb_uffd_send_unlock(void);

extern int PidUffdSet_taskargs(void *args, int pid);

extern int PidUffdSet_sendfd(int sockfd, int pid);
extern int PidUffdSet_recvfd(int sockfd, int *pid, int **uffdset, int *nr_uffd);

extern int PidUffdSet_send_region(int sockfd, int pid);
extern int PidUffdSet_recv_region(int sockfd, int pid, int *uffdset);

extern int PidUffdSet_fullfill(int pid, int sockfd);

extern int ioctl_mul(volatile struct uffdio_copy *data, int pid);
extern int ioctl_mul_tagged(volatile struct uffdio_copy *data, int pid, unsigned lane);
struct sb_install_profile;
extern int ioctl_mul_profiled(volatile struct uffdio_copy *, int, unsigned, struct sb_install_profile *);
extern int sb_uffd_set_ready_fd(int pid, int fd);
extern int sb_uffd_wait_ready(int pid);

extern void * shmmap(uint64_t size);

int update_PidUffdSet(int pid, int nr_uffd, int *uffdset);

#endif
