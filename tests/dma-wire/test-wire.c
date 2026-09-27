/* Exercise the production wire helper; ioctl supplies/validates synthetic DMA
 * vectors, actual socket streams are fragmented and closed on failures. */
#define _GNU_SOURCE
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include <sys/socket.h>
#include <unistd.h>
#include "sb-kernel-dma-wire.h"
static _Thread_local unsigned calls;
static _Thread_local int ioctl_error, wire_error;
static uint64_t address(const struct sbk_rdma_region *r, size_t i)
{ return (0x100000000ULL + (i * 17 + r->id * 131) * 4096); }
int __wrap_ioctl(int fd, unsigned long request, ...)
{
    assert(fd == 42); calls++;
    if (ioctl_error) { errno = ioctl_error; return -1; }
    va_list ap; va_start(ap,request); struct sbk_dma_map *m=va_arg(ap,struct sbk_dma_map*);va_end(ap);
    uint64_t *a=(void*)(uintptr_t)m->addresses;
    assert(request==SBK_IOC_DMA_EXPORT_MAP || request==SBK_IOC_DMA_IMPORT_MAP);
    for(size_t i=0;i<m->region.pages;i++) {
        if(request==SBK_IOC_DMA_EXPORT_MAP) a[i]=address(&m->region,i);
        else assert(a[i]==address(&m->region,i));
    }
    return 0;
}
static int transfer(int fd,void *data,size_t n,int sending)
{
    if(wire_error) return -1;
    for(size_t off=0;off<n;) {
        size_t part=n-off<251?n-off:251;
        ssize_t done=sending?send(fd,(char*)data+off,part,MSG_NOSIGNAL):recv(fd,(char*)data+off,part,0);
        if(done<0 && errno==EINTR)continue;
        if(done<=0)return -1;
        off+=done;
    }
    return 0;
}
struct pair {int socket,sending;};
static void *roundtrip(void *arg)
{
    struct pair *p=arg; calls=0;
    const unsigned sizes[]={1,31,16384,1<<20};
    for(unsigned i=0;i<4;i++) {
        struct sbk_rdma_region region={.address=0x70000000,.pages=sizes[i],.id=i+1,.rkey=0x13579};
        assert(!sbk_dma_wire_transfer(p->socket,42,&region,p->sending,true,transfer));
        /* Record/index framing after the vector must remain intact. */
        unsigned char marker=0xab;
        assert(!transfer(p->socket,&marker,1,p->sending) && marker==0xab);
    }
    assert(calls==4); close(p->socket);return NULL;
}
int main(void)
{
    struct sbk_rdma_endpoint peer={0};
    assert(sbk_dma_wire_mode_matches(false,&peer) && !sbk_dma_wire_mode_matches(true,&peer));
    peer.reserved[0]=1;assert(sbk_dma_wire_mode_matches(true,&peer) && !sbk_dma_wire_mode_matches(false,&peer));
    peer.reserved[1]=1;assert(!sbk_dma_wire_mode_matches(true,&peer));
    peer.reserved[0]=2;peer.reserved[1]=0;assert(!sbk_dma_wire_mode_matches(true,&peer));
    for(unsigned v=0;v<8;v++) {
        assert(sbk_dma_ps_version_valid(true,v)==(v==5));
        assert(sbk_dma_ps_version_valid(false,v)==(v>=2 && v<=4));
    }
    assert(sbk_dma_ps_version(true)==5 && sbk_dma_final_version(true)==3);
    assert(sbk_dma_ps_version(false)==4 && sbk_dma_final_version(false)==2);
    assert(!sbk_dma_wire_transfer(-1,-1,NULL,true,false,NULL));
    struct sbk_rdma_region r={.address=4096,.pages=4,.id=1,.rkey=7},bad;
    for(int i=0;i<6;i++) {
        bad=r;
        if(i==0)bad.pages=0;
        if(i==1)bad.pages=(1<<20)+1;
        if(i==2)bad.id=0;
        if(i==3)bad.id=SBK_MAX_REGIONS+1;
        if(i==4)bad.address++;
        if(i==5)bad.address=UINT64_MAX-4095;
        assert(sbk_dma_wire_transfer(-1,42,&bad,true,true,transfer)==-EPROTO && !calls);
    }
    ioctl_error=EACCES;
    assert(sbk_dma_wire_transfer(-1,42,&r,true,true,transfer)==-EACCES && calls==1);
    ioctl_error=0; wire_error=1;calls=0;
    assert(sbk_dma_wire_transfer(-1,42,&r,false,true,transfer)==-EIO && !calls);
    assert(sbk_dma_wire_transfer(-1,42,&r,true,true,transfer)==-EIO && calls==1);
    wire_error=0;calls=0;
    int sockets[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    uint64_t a=address(&r,0);assert(send(sockets[0],&a,sizeof(a),0)==sizeof(a));close(sockets[0]);
    assert(sbk_dma_wire_transfer(sockets[1],42,&r,false,true,transfer)==-EIO && !calls);close(sockets[1]);
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    uint64_t all[4];for(size_t i=0;i<4;i++)all[i]=address(&r,i);
    assert(send(sockets[0],all,sizeof(all),0)==sizeof(all));close(sockets[0]);ioctl_error=EEXIST;
    assert(sbk_dma_wire_transfer(sockets[1],42,&r,false,true,transfer)==-EEXIST && calls==1);close(sockets[1]);ioctl_error=0;
    assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sockets));
    struct pair p[2]={{sockets[0],1},{sockets[1],0}};pthread_t t[2];
    for(int i=0;i<2;i++)assert(!pthread_create(&t[i],NULL,roundtrip,&p[i]));
    for(int i=0;i<2;i++)assert(!pthread_join(t[i],NULL));
    puts("PASS DMA wire negotiation, version bounds, fragmented vectors, framing, ioctl errors and no publication on truncated receive");
}
