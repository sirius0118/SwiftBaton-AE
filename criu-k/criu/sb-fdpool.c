#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <compel/plugins/std/syscall-codes.h>
#include <unistd.h>
#include "log.h"
#include "servicefd.h"
#include "sb-fdpool.h"

#define HINT_MAGIC UINT64_C(0x53424644504f4f31)
#define HINT_FILE "sb-fdpool.img"
#define MAX_OBJECTS 8192U
/* v1 ends immediately before consumers. Hints are advisory; final images
 * determine object state and ownership. v2 adds the number of restore actors
 * so one actor cannot drain objects needed by another into a private cache. */
struct hint {
    uint64_t magic;
    uint32_t version, counts[SB_FDPOOL_KINDS];
    uint32_t consumers, reserved;
};
struct context {
    uint64_t user_dev, user_ino, net_dev, net_ino;
    unsigned uid[4], gid[4];
    gid_t groups[64]; int nr_groups;
    unsigned long long capabilities;
    char label[256], cgroup[4096];
};
struct shared_pool {
    struct hint hint;
    struct context creator;
    unsigned prepared[SB_FDPOOL_KINDS];
    unsigned ready;
};
static struct shared_pool *pool;
static int writers[SB_FDPOOL_KINDS];
static int cached[SB_FDPOOL_KINDS][SB_FDPOOL_BATCH];
static unsigned cached_count[SB_FDPOOL_KINDS];
static unsigned reused[SB_FDPOOL_KINDS], misses[SB_FDPOOL_KINDS];
static int min_temporary;
static bool active, context_ok, net_known, net_ok;

static int read_text(const char *path, char *buf, size_t size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    ssize_t n;
    do { n = read(fd, buf, size - 1); } while (n < 0 && errno == EINTR);
    int error=errno;close(fd);
    if (n<0){errno=error;return -1;}
    if ((size_t)n==size-1){errno=EOVERFLOW;return -1;}
    buf[n] = 0; return 0;
}
static int read_context(struct context *c)
{
    char status[8192], *line;
    struct stat user, net;
    memset(c, 0, sizeof(*c));
    if (stat("/proc/self/ns/user", &user) || stat("/proc/self/ns/net", &net) ||
        read_text("/proc/self/status", status, sizeof(status)) ||
        read_text("/proc/self/cgroup", c->cgroup, sizeof(c->cgroup))) return -1;
    c->nr_groups=getgroups(64,c->groups);if(c->nr_groups<0)return -1;
    c->user_dev=user.st_dev; c->user_ino=user.st_ino; c->net_dev=net.st_dev; c->net_ino=net.st_ino;
    line=strstr(status,"Uid:");
    if (!line || sscanf(line,"Uid: %u %u %u %u",&c->uid[0],&c->uid[1],&c->uid[2],&c->uid[3])!=4) return -1;
    line=strstr(status,"Gid:");
    if (!line || sscanf(line,"Gid: %u %u %u %u",&c->gid[0],&c->gid[1],&c->gid[2],&c->gid[3])!=4) return -1;
    line=strstr(status,"CapEff:");
    if (!line || sscanf(line,"CapEff: %llx",&c->capabilities)!=1) return -1;
    if (read_text("/proc/self/attr/current", c->label, sizeof(c->label))) {
        /* EINVAL is the no active LSM case. All other errors fail closed. */
        if (errno!=EINVAL && errno!=ENOENT) return -1;
        c->label[0]=0;
    }
    return 0;
}
int sb_fdpool_socket_kind(int domain, int type, int protocol)
{
    if (domain!=AF_INET && domain!=AF_INET6) return -1;
    if (type==SOCK_STREAM && (!protocol || protocol==IPPROTO_TCP)) return domain==AF_INET?SB_FDP_TCP4:SB_FDP_TCP6;
    if (type==SOCK_DGRAM && (!protocol || protocol==IPPROTO_UDP)) return domain==AF_INET?SB_FDP_UDP4:SB_FDP_UDP6;
    return -1;
}
static int create_object(unsigned kind)
{
    if (kind==SB_FDP_EVENT || kind==SB_FDP_SEMAPHORE) return eventfd(0,kind==SB_FDP_SEMAPHORE?EFD_SEMAPHORE:0);
    if (kind==SB_FDP_EPOLL) return epoll_create1(0);
    int domain=kind>=SB_FDP_TCP6?AF_INET6:AF_INET;
    return socket(domain,(kind==SB_FDP_TCP4||kind==SB_FDP_TCP6)?SOCK_STREAM:SOCK_DGRAM,0);
}
int sb_fdpool_write_hint(int image_dir, const pid_t *pids, size_t count)
{
    struct hint h={.magic=HINT_MAGIC,.version=2,.consumers=count};
    unsigned total=0;
    if ((!pids && count) || count>4096) { errno=EINVAL; return -1; }
    for (size_t i=0; i<count && total<MAX_OBJECTS; i++) {
        char path[128]; struct dirent *entry;
        snprintf(path,sizeof(path),"/proc/%d/fd",pids[i]);
        DIR *dir=opendir(path); if(!dir) continue;
        int pidfd=syscall(SYS_pidfd_open,pids[i],0);
        while ((entry=readdir(dir)) && total<MAX_OBJECTS) {
            char *end,link[128]; long n=strtol(entry->d_name,&end,10); int kind=-1;
            if (*end || n<0 || n>INT_MAX) continue;
            ssize_t size=readlinkat(dirfd(dir),entry->d_name,link,sizeof(link)-1);
            if (size<0 || size==sizeof(link)-1) continue;
            link[size]=0;
            if (!strcmp(link,"anon_inode:[eventpoll]")) kind=SB_FDP_EPOLL;
            else if (!strcmp(link,"anon_inode:[eventfd]")) {
                char info[1024]; unsigned sem=0;
                snprintf(path,sizeof(path),"/proc/%d/fdinfo/%ld",pids[i],n);
                if (!read_text(path,info,sizeof(info))) {
                    char *p=strstr(info,"eventfd-semaphore:");
                    if (p && (sscanf(p,"eventfd-semaphore: %u",&sem)!=1 || sem>1)) continue;
                    kind=sem?SB_FDP_SEMAPHORE:SB_FDP_EVENT;
                }
            } else if (!strncmp(link,"socket:[",8) && pidfd>=0) {
                int fd=syscall(SYS_pidfd_getfd,pidfd,(int)n,0);
                if(fd>=0) {
                    int domain,type,protocol; socklen_t len=sizeof(int);
                    if (!getsockopt(fd,SOL_SOCKET,SO_DOMAIN,&domain,&len) &&
                        !getsockopt(fd,SOL_SOCKET,SO_TYPE,&type,&len) &&
                        !getsockopt(fd,SOL_SOCKET,SO_PROTOCOL,&protocol,&len))
                        kind=sb_fdpool_socket_kind(domain,type,protocol);
                    close(fd);
                }
            }
            if(kind>=0) {h.counts[kind]++;total++;}
        }
        if(pidfd>=0) close(pidfd);
        closedir(dir);
    }
    int fd=openat(image_dir,HINT_FILE,O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);
    if(fd<0) return -1;
    ssize_t n=write(fd,&h,sizeof(h)); int saved=errno;close(fd);errno=saved;
    if(n!=sizeof(h)) return -1;
    pr_info("SB_FDPOOL hint consumers=%u total=%u event=%u semaphore=%u epoll=%u tcp4=%u udp4=%u tcp6=%u udp6=%u\n",
        h.consumers,total,h.counts[0],h.counts[1],h.counts[2],h.counts[3],h.counts[4],h.counts[5],h.counts[6]);
    return 0;
}
static int send_batch(int socket_fd, int *fds, unsigned count)
{
    char data=(char)count;
    struct iovec iov={&data,1};
    union {struct cmsghdr alignment; char bytes[CMSG_SPACE(SB_FDPOOL_BATCH*sizeof(int))];}control;
    memset(&control,0,sizeof(control));
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=&control,.msg_controllen=CMSG_SPACE(count*sizeof(int))};
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c->cmsg_level=SOL_SOCKET;c->cmsg_type=SCM_RIGHTS;c->cmsg_len=CMSG_LEN(count*sizeof(int));
    memcpy(CMSG_DATA(c),fds,count*sizeof(int));
    return sendmsg(socket_fd,&msg,MSG_DONTWAIT|MSG_NOSIGNAL)==1?0:-1;
}
int sb_fdpool_prepare(int image_dir)
{
    struct hint h={0}; struct stat st; unsigned total=0;
    int fd=openat(image_dir,HINT_FILE,O_RDONLY|O_CLOEXEC);
    if(fd<0) return errno==ENOENT?0:-1;
    int ok=!fstat(fd,&st) &&
        (st.st_size==offsetof(struct hint,consumers) || st.st_size==sizeof(h)) &&
        read(fd,&h,st.st_size)==st.st_size;
    close(fd);
    if(!ok || h.magic!=HINT_MAGIC) return -1;
    if(h.version==1 && st.st_size==offsetof(struct hint,consumers)) {
        /* Older hints do not identify consumers: use conservative delivery. */
        h.consumers=2;
    } else if(h.version!=2 || st.st_size!=sizeof(h) ||
              h.consumers>4096 || h.reserved) return -1;
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++) {
        if(h.counts[k]>MAX_OBJECTS-total) return -1;
        total+=h.counts[k]; writers[k]=-1;
    }
    if(total && !h.consumers) return -1;
    pool=mmap(NULL,sizeof(*pool),PROT_READ|PROT_WRITE,MAP_SHARED|MAP_ANONYMOUS,-1,0);
    if(pool==MAP_FAILED){pool=NULL;return -1;}
    pool->hint=h;
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++) {
        if(!h.counts[k]) continue;
        int pair[2], size=8*1024*1024;
        if(socketpair(AF_UNIX,SOCK_DGRAM|SOCK_CLOEXEC|SOCK_NONBLOCK,0,pair)) goto fail;
        /* Queue limits may reduce PS coverage but never block migration. */
        setsockopt(pair[0],SOL_SOCKET,SO_SNDBUFFORCE,&size,sizeof(size));
        setsockopt(pair[1],SOL_SOCKET,SO_RCVBUFFORCE,&size,sizeof(size));
        if(install_service_fd(SB_FDPOOL_FIRST+k,pair[1])<0){close(pair[0]);goto fail;}
        writers[k]=pair[0];
    }
    return 0;
fail:
    sb_fdpool_parent_close_writers();sb_fdpool_close();return -1;
}
void sb_fdpool_parent_close_writers(void)
{
    if(!pool)return;
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++) if(writers[k]>=0){close(writers[k]);writers[k]=-1;}
}
void sb_fdpool_populate(int permitted)
{
    if(!pool)return;
    if(!permitted || read_context(&pool->creator)) goto done;
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++) {
        unsigned remaining=pool->hint.counts[k];
        unsigned batch=pool->hint.consumers==1?SB_FDPOOL_BATCH:1;
        while(remaining) {
            /* SCM_RIGHTS messages are indivisible. A multi-process restore
             * must consume only the object it needs, otherwise cached spare
             * FDs disappear with one actor while another creates replacements.
             * A single actor still amortizes recvmsg with the original batch. */
            int fds[SB_FDPOOL_BATCH]; unsigned n=0, wanted=remaining<batch?remaining:batch;
            while(n<wanted){int fd=create_object(k);if(fd<0)break;fds[n++]=fd;}
            int ret=n?send_batch(writers[k],fds,n):-1;
            for(unsigned j=0;j<n;j++)close(fds[j]);
            if(ret)break;
            pool->prepared[k]+=n; remaining-=n;
            if(n<wanted)break;
        }
        pr_info("SB_FDPOOL prepared kind=%u requested=%u created=%u delivery_batch=%u consumers=%u\n",k,pool->hint.counts[k],pool->prepared[k],batch,pool->hint.consumers);
    }
    __atomic_store_n(&pool->ready,1,__ATOMIC_RELEASE);
done:
    sb_fdpool_parent_close_writers();
    /* The long-lived netns holder must not retain any pool queue. */
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++)close_service_fd(SB_FDPOOL_FIRST+k);
}
void sb_fdpool_enter(int floor)
{
    struct context now;
    active=false;context_ok=false;net_known=false;net_ok=false;min_temporary=floor;
    memset(reused,0,sizeof(reused));memset(misses,0,sizeof(misses));
    if(!pool || !__atomic_load_n(&pool->ready,__ATOMIC_ACQUIRE) || floor<=0) return;
    if(read_context(&now))return;
    const struct context *before=&pool->creator;
    bool user=now.user_dev==before->user_dev && now.user_ino==before->user_ino;
    bool cred=!memcmp(now.uid,before->uid,sizeof(now.uid))&&!memcmp(now.gid,before->gid,sizeof(now.gid))&&now.capabilities==before->capabilities&&now.nr_groups==before->nr_groups&&!memcmp(now.groups,before->groups,now.nr_groups*sizeof(gid_t));
    bool label=!strcmp(now.label,before->label), cgroup=!strcmp(now.cgroup,before->cgroup);
    context_ok=user&&cred&&label&&cgroup;active=true;
    pr_info("SB_FDPOOL enter pid=%d context=%u user=%u credentials=%u label=%u cgroup=%u floor=%d\n",getpid(),context_ok,user,cred,label,cgroup,floor);
}
void sb_fdpool_netns_changed(void){net_known=false;net_ok=false;}
static int receive_batch(unsigned kind)
{
    int queue=get_service_fd(SB_FDPOOL_FIRST+kind);if(queue<0)return -1;
    unsigned char count=0;struct iovec iov={&count,1};
    union {struct cmsghdr alignment; char bytes[CMSG_SPACE(SB_FDPOOL_BATCH*sizeof(int))];}control;
    memset(&control,0,sizeof(control));
    struct msghdr msg={.msg_iov=&iov,.msg_iovlen=1,.msg_control=&control,.msg_controllen=sizeof(control)};
    ssize_t n=recvmsg(queue,&msg,MSG_DONTWAIT|MSG_CMSG_CLOEXEC);
    if(n<0)return -1;
    struct cmsghdr *c=CMSG_FIRSTHDR(&msg);
    unsigned received=0;int raw[SB_FDPOOL_BATCH];
    if(c && c->cmsg_level==SOL_SOCKET && c->cmsg_type==SCM_RIGHTS && c->cmsg_len>=CMSG_LEN(0)) {
        received=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
        if(received>SB_FDPOOL_BATCH)received=SB_FDPOOL_BATCH;
        memcpy(raw,CMSG_DATA(c),received*sizeof(int));
    }
    bool valid=n==1 && !(msg.msg_flags&(MSG_CTRUNC|MSG_TRUNC)) && received==count && count && count<=SB_FDPOOL_BATCH;
    for(unsigned j=0;j<received;j++) {
        int fd=valid?fcntl(raw[j],F_DUPFD,min_temporary):-1;
        close(raw[j]);
        if(fd>=0)cached[kind][cached_count[kind]++]=fd;
    }
    return cached_count[kind]?0:-1;
}
int sb_fdpool_take(enum sb_fdpool_kind kind)
{
    if(kind<0 || kind>=SB_FDPOOL_KINDS || !active)return -1;
    if(!context_ok)goto miss;
    if(kind>=SB_FDP_TCP4) {
        if(!net_known){struct stat st;net_ok=!stat("/proc/self/ns/net",&st)&&st.st_dev==pool->creator.net_dev&&st.st_ino==pool->creator.net_ino;net_known=true;}
        if(!net_ok)goto miss;
    }
    if(!cached_count[kind] && receive_batch(kind))goto miss;
    reused[kind]++;return cached[kind][--cached_count[kind]];
miss:
    misses[kind]++;return -1;
}
void sb_fdpool_leave(void)
{
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++) {
        while(cached_count[k])close(cached[k][--cached_count[k]]);
        if(active && (reused[k]||misses[k]))pr_info("SB_FDPOOL consumed pid=%d kind=%u reused=%u fallback=%u\n",getpid(),k,reused[k],misses[k]);
    }
    active=false;
}
void sb_fdpool_close(void)
{
    sb_fdpool_leave();
    for(unsigned k=0;k<SB_FDPOOL_KINDS;k++)close_service_fd(SB_FDPOOL_FIRST+k);
}
