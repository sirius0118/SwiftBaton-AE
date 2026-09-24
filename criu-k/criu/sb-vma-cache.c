#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include "include/sb-vma-cache.h"
#include "include/sb-vma-syscalls.h"
#include "include/sb-vma-blob.h"
#include "log.h"
#define LIMIT (16U << 20)
struct cached_smaps { pid_t pid; uint64_t identity, generation; int fd; char *maps; size_t length; };
static struct cached_smaps *saved;
static size_t saved_count;
static struct bpf_object *object;
static struct bpf_link *enter_link, *exit_link;
static int epoch_fd = -1;

static void detach(void)
{
    if (enter_link) bpf_link__destroy(enter_link);
    if (exit_link) bpf_link__destroy(exit_link);
    enter_link = exit_link = NULL;
    if (object) bpf_object__close(object);
    object = NULL; epoch_fd = -1;
}
void sb_vma_cache_close(void)
{
    detach();
    for (size_t i=0;i<saved_count;i++) { if(saved[i].fd>=0)close(saved[i].fd); free(saved[i].maps); }
    free(saved); saved=NULL; saved_count=0;
}
static int epoch_read(struct sb_vma_epoch *value)
{
    unsigned key=0;
    return epoch_fd<0 || bpf_map_lookup_elem(epoch_fd,&key,value) || value->active || value->poisoned ? -1 : 0;
}
static uint64_t identity(pid_t pid)
{
    char path[64],line[4096],*last,*save,*token;uint64_t result=0;
    snprintf(path,sizeof(path),"/proc/%d/stat",pid);FILE *f=fopen(path,"re");
    if(!f)return 0;
    if(fgets(line,sizeof(line),f)&&(last=strrchr(line,')'))) {
        token=strtok_r(last+1," ",&save);
        for(int field=3;token&&field<=22;field++,token=strtok_r(NULL," ",&save))
            if(field==22)result=strtoull(token,NULL,10);
    }
    fclose(f);return result;
}
static char *read_file(pid_t pid,const char *file,size_t *length)
{
    char path[96],*buffer=malloc(LIMIT+1);size_t used=0;int fd;
    if(!buffer)return NULL;
    snprintf(path,sizeof(path),"/proc/%d/%s",pid,file);fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd<0){free(buffer);return NULL;}
    while(used<LIMIT) {
        ssize_t n=read(fd,buffer+used,LIMIT-used);
        if(n<0&&errno==EINTR)continue;
        if(n<0){used=LIMIT;break;}
        if(!n)break;
        used+=n;
    }
    close(fd);
    if(used==LIMIT){free(buffer);return NULL;}
    buffer[used]=0;*length=used;return buffer;
}
static int native_elf(pid_t pid)
{
    unsigned char header[20]; char path[64];
    snprintf(path,sizeof(path),"/proc/%d/exe",pid);
    int fd=open(path,O_RDONLY|O_CLOEXEC); if(fd<0)return 0;
    ssize_t n=read(fd,header,sizeof(header)); close(fd);
    return n==sizeof(header) && !memcmp(header,"\177ELF",4) &&
        header[4]==2 && header[5]==1 && header[18]==62 && header[19]==0;
}
static int known_pid(pid_t pid)
{
    for(size_t i=0;i<saved_count;i++)if(saved[i].pid==pid)return 1;
    return 0;
}
/* Establishing the monitor cannot see syscall entries which predate attach.
 * Reject an in-flight mutation at capture; later entries/exits are counted.
 * Unknown descendants reject the closed-process-set cache as well. */
static int quiescent(pid_t pid)
{
    char path[96];struct dirent *entry;int ok=1;
    snprintf(path,sizeof(path),"/proc/%d/task",pid);DIR *dir=opendir(path);if(!dir)return 0;
    while((entry=readdir(dir))) {
        char *end;long tid=strtol(entry->d_name,&end,10);if(*end||tid<=0)continue;
        char line[512];FILE *f;long nr;
        snprintf(path,sizeof(path),"/proc/%d/task/%ld/syscall",pid,tid);f=fopen(path,"re");
        if(!f){ok=0;break;}
        if(!fgets(line,sizeof(line),f) || (strncmp(line,"running",7) &&
           (sscanf(line,"%ld",&nr)!=1 || sb_vma_changes(nr))))ok=0;
        fclose(f);if(!ok)break;
        snprintf(path,sizeof(path),"/proc/%d/task/%ld/children",pid,tid);f=fopen(path,"re");
        if(!f){ok=0;break;}
        while(fscanf(f,"%ld",&nr)==1)if(!known_pid(nr)){ok=0;break;}
        if(ferror(f))ok=0;
        fclose(f);if(!ok)break;
    }
    closedir(dir);return ok;
}
static char *map_headers(const char *text,size_t length,size_t *output)
{
    char *headers=malloc(length+1);size_t used=0,flags=0,mappings=0;
    const char *p=text,*end=text+length; int vvar=0;
    if(!headers)return NULL;
    while(p<end) {
        const char *next=memchr(p,'\n',end-p);if(!next){free(headers);return NULL;}next++;
        unsigned long long start,finish;char perms[5];
        if(sscanf(p,"%llx-%llx %4s",&start,&finish,perms)==3&&start<finish) {
            memcpy(headers+used,p,next-p);used+=next-p;mappings++;
            vvar=memmem(p,next-p,"[vvar]",6)!=NULL;
        } else if(!strncmp(p,"VmFlags:",8)) {
            flags++;
            /* External UFFD owners can change flags with an ioctl outside the
             * watched process set. Keep those mappings on live collection. */
            for(const char *q=p+8;q+3<next;q++)
                if(!memcmp(q," um ",4)||!memcmp(q," uw ",4)||!memcmp(q," ss ",4)||
                   (!vvar&&(!memcmp(q," io ",4)||!memcmp(q," pf ",4)))){free(headers);return NULL;}
        }
        p=next;
    }
    if(!mappings||flags!=mappings){free(headers);return NULL;}
    headers[used]=0;*output=used;return headers;
}
static int monitor_log(enum libbpf_print_level level, const char *format, va_list args)
{
    char buffer[2048];int n;
    if(level==LIBBPF_DEBUG)return 0;
    n=vsnprintf(buffer,sizeof(buffer),format,args);
    pr_warn("SB_VMA_BPF %s",buffer);return n;
}
int sb_vma_cache_start(const uint64_t *pids,size_t count)
{
    struct bpf_program *program;int watched;const char *step="open";
    libbpf_print_fn_t old_log;
    sb_vma_cache_close();
#if !defined(__x86_64__)
    return -1;
#endif
    if(!count||count>4096)return -1;
    saved=calloc(count,sizeof(*saved));if(!saved)return -1;
    for(size_t i=0;i<count;i++) {
        if(!pids[i] || pids[i]>INT32_MAX || known_pid(pids[i]))continue;
        saved[saved_count].pid=pids[i];saved[saved_count++].fd=-1;
    }
    if(!saved_count){sb_vma_cache_close();return -1;}
    old_log=libbpf_set_print(monitor_log);
    object=bpf_object__open_mem(sb_vma_bpf,sizeof(sb_vma_bpf),NULL);
    if(!object||libbpf_get_error(object)){object=NULL;goto fail;}
    step="load";if(bpf_object__load(object))goto fail;
    step="maps";watched=bpf_object__find_map_fd_by_name(object,"watched");
    epoch_fd=bpf_object__find_map_fd_by_name(object,"epoch");
    if(watched<0||epoch_fd<0)goto fail;
    step="watch";for(size_t i=0;i<saved_count;i++){unsigned key=saved[i].pid,value=1;if(bpf_map_update_elem(watched,&key,&value,BPF_NOEXIST))goto fail;}
    step="attach_enter";program=bpf_object__find_program_by_name(object,"vma_enter");if(!program)goto fail;
    enter_link=bpf_program__attach_raw_tracepoint(program,"sys_enter");
    if(!enter_link||libbpf_get_error(enter_link)){enter_link=NULL;goto fail;}
    step="attach_exit";program=bpf_object__find_program_by_name(object,"vma_exit");if(!program)goto fail;
    exit_link=bpf_program__attach_raw_tracepoint(program,"sys_exit");
    if(!exit_link||libbpf_get_error(exit_link)){exit_link=NULL;goto fail;}
    libbpf_set_print(old_log);
    pr_info("SB_VMA_CACHE monitor_ready processes=%zu input_entries=%zu\n",saved_count,count);return 0;
fail:
    libbpf_set_print(old_log);
    pr_warn("SB_VMA_CACHE monitor_unavailable step=%s errno=%d fallback=live_smaps\n",step,errno);sb_vma_cache_close();return -1;
}
void sb_vma_cache_refresh(void)
{
    if(epoch_fd<0)return;
    for(size_t i=0;i<saved_count;i++) {
        struct cached_smaps *c=&saved[i];struct sb_vma_epoch before,after;
        char *smaps=NULL,*maps=NULL;size_t length,map_length;uint64_t who;int fd=-1;
        if(c->fd>=0)close(c->fd);
        c->fd=-1;free(c->maps);c->maps=NULL;
        if(epoch_read(&before)||!native_elf(c->pid)||!quiescent(c->pid)||!(who=identity(c->pid)))continue;
        smaps=read_file(c->pid,"smaps",&length);if(!smaps)continue;
        maps=map_headers(smaps,length,&map_length);if(!maps)goto skip;
        if(epoch_read(&after)||after.generation!=before.generation||identity(c->pid)!=who)goto skip;
        fd=memfd_create("swiftbaton-smaps",MFD_CLOEXEC|MFD_ALLOW_SEALING);if(fd<0)goto skip;
        size_t offset=0;
        while(offset<length){ssize_t n=write(fd,smaps+offset,length-offset);if(n<0&&errno==EINTR)continue;if(n<=0)goto skip;offset+=n;}
        if(fcntl(fd,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL))goto skip;
        c->fd=fd;c->maps=maps;c->length=map_length;c->identity=who;c->generation=after.generation;
        pr_info("SB_VMA_CACHE captured pid=%d bytes=%zu generation=%llu\n",c->pid,length,after.generation);
        fd=-1;maps=NULL;
skip:
        if(fd>=0)close(fd);
        free(maps);free(smaps);
    }
}
int sb_vma_cache_open(pid_t pid)
{
    struct cached_smaps *c=NULL;struct sb_vma_epoch before,after;char path[64],*maps;size_t length;int fd=-1;
    for(size_t i=0;i<saved_count;i++)if(saved[i].pid==pid){c=&saved[i];break;}
    if(!c||c->fd<0)return -1;
    if(epoch_read(&before)||before.generation!=c->generation||identity(pid)!=c->identity)goto miss;
    /* Distinct TGIDs sharing one mm require a collective stop barrier. Until
     * that path is integrated, retain ordinary collection for those tasks. */
    for(size_t i=0;i<saved_count;i++)if(saved[i].pid!=pid)
        if(syscall(SYS_kcmp,pid,saved[i].pid,1,0,0)<=0)goto miss; /* KCMP_VM */
    maps=read_file(pid,"maps",&length);if(!maps)goto miss;
    int same=length==c->length&&!memcmp(maps,c->maps,length);free(maps);
    if(!same||epoch_read(&after)||after.generation!=before.generation)goto miss;
    snprintf(path,sizeof(path),"/proc/self/fd/%d",c->fd);fd=open(path,O_RDONLY|O_CLOEXEC);
    if(fd>=0){pr_info("SB_VMA_CACHE hit pid=%d generation=%llu\n",pid,after.generation);return fd;}
miss:
    pr_info("SB_VMA_CACHE miss pid=%d fallback=live_smaps\n",pid);return -1;
}
