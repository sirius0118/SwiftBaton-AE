/* In-memory CRIU image snapshots, published at explicit PS and IS barriers.
 * The K rsocket-proxy baseline uses a dedicated mapped rsocket for image
 * payloads; other K profiles retain the reliable-connected QP. */
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <linux/magic.h>
#include <rdma/rsocket.h>
#include <unistd.h>
#include "cr_options.h"
#include "cr-sync.h"
#include "image.h"
#include "xmalloc.h"
#include "RDMA.h"
#include "sb-images.h"
#include "sb-trace.h"

#define SB_IMAGE_MAGIC 0x5342494dU
#define SB_IMAGE_CHUNK (1024U * 1024U)
#define SB_IMAGE_NAME 256
#define SB_IMAGE_CONTROL 64

struct sb_image_manifest {
    uint32_t magic, version, phase, count;
    uint64_t bytes;
};
struct sb_image_entry {
    char name[SB_IMAGE_NAME];
    uint64_t bytes;
};
struct sb_image_rsocket_map { uint32_t magic, version; uint64_t offset, length; };
struct sb_image_rsocket_phase { uint32_t magic, phase; uint64_t bytes; };

static struct resources image_res;
static size_t image_capacity;
static int image_active;
static int image_rsocket_fd = -1;
static off_t image_remote_offset;

static int image_rsocket_send(const void *data, size_t length)
{
    const char *at = data;
    while (length) {
        ssize_t n = rsend(image_rsocket_fd, at, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        at += n; length -= n;
    }
    return 0;
}
static int image_rsocket_recv(void *data, size_t length)
{
    char *at = data;
    while (length) {
        ssize_t n = rrecv(image_rsocket_fd, at, length, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        at += n; length -= n;
    }
    return 0;
}
static int image_rsocket_configure(int fd)
{
    int mappings = 1, queue = 512, buffer = 8 * 1024 * 1024;
    return rsetsockopt(fd, SOL_RDMA, RDMA_IOMAPSIZE, &mappings, sizeof(mappings)) ||
           rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue, sizeof(queue)) ||
           rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue, sizeof(queue)) ||
           rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer, sizeof(buffer)) ||
           rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer, sizeof(buffer));
}
static int image_rsocket_connect(int control_socket, int source)
{
    struct sockaddr_in address = {0};
    struct sb_image_rsocket_map own, peer;
    socklen_t address_length = sizeof(address);
    off_t offset;
    int fd, listener;
    if (image_rsocket_fd >= 0) return 0;
    if (opts.port < 1 || opts.port + 12 > 65535 ||
        (source ? getpeername(control_socket, (struct sockaddr *)&address, &address_length) :
                  getsockname(control_socket, (struct sockaddr *)&address, &address_length)) ||
        address.sin_family != AF_INET) return -1;
    address.sin_port = htons(opts.port + 12);
    fd = rsocket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0 || image_rsocket_configure(fd)) return -1;
    if (source) {
        if (rconnect(fd, (struct sockaddr *)&address, sizeof(address))) return -1;
    } else {
        listener = fd;
        if (rbind(listener, (struct sockaddr *)&address, sizeof(address)) ||
            rlisten(listener, 1)) return -1;
        fd = raccept(listener, NULL, NULL);
        if (fd < 0) return -1;
        /* Keep the listener until process exit; rclose can block on a live
         * rsocket connection in the lab's librdmacm build. */
    }
    image_rsocket_fd = fd;
    offset = riomap(fd, image_res.buf, image_capacity, PROT_WRITE, 0, -1);
    if (offset == -1) return -1;
    own = (struct sb_image_rsocket_map){SB_IMAGE_MAGIC, 1, offset, image_capacity};
    if (source) {
        if (image_rsocket_send(&own, sizeof(own)) || image_rsocket_recv(&peer, sizeof(peer))) return -1;
    } else {
        if (image_rsocket_recv(&peer, sizeof(peer)) || image_rsocket_send(&own, sizeof(own))) return -1;
    }
    if (peer.magic != SB_IMAGE_MAGIC || peer.version != 1 ||
        peer.offset > INT64_MAX || peer.length != image_capacity ||
        peer.length > INT64_MAX - peer.offset) return -1;
    image_remote_offset = (off_t)peer.offset;
    pr_info("SB_IMAGE rsocket_connected role=%s bytes=%zu\n",
            source ? "source" : "target", image_capacity);
    return 0;
}

static int image_file_name(const char *name)
{
    size_t n = strnlen(name, SB_IMAGE_NAME);
    return n > 4 && n < SB_IMAGE_NAME && !strchr(name, '/') &&
           !strcmp(name + n - 4, ".img");
}

int sb_images_source_prepare(pid_t pid)
{
    char path[PATH_MAX];
    if (!opts.sb_image_rdma)
        return 0;
    snprintf(path, sizeof(path), "/dev/shm/swiftbaton-images-%d", pid);
    /* Never reuse a checkpoint left over from a previous PID generation. */
    if (mkdir(path, 0700)) {
        pr_perror("Creating RAM image directory %s", path);
        return -1;
    }
    SET_CHAR_OPTS(imgs_dir, path);
    if (open_image_dir(opts.imgs_dir, -1))
        return -1;
    pr_info("SB_IMAGE directory=%s storage=tmpfs\n", opts.imgs_dir);
    return 0;
}

int sb_images_init(int socket, int source)
{
    struct statfs fs;
    if (!opts.sb_image_rdma)
        return 0;
    if (fstatfs(get_service_fd(IMG_FD_OFF), &fs) || fs.f_type != TMPFS_MAGIC) {
        pr_err("RDMA image mode requires a tmpfs image directory\n");
        return -1;
    }
    image_capacity = 64UL * 1024 * 1024;
    if (!opts.sb_kernel_rsocket_proxy) {
        resources_init(&image_res);
        image_res.config.dev_name = opts.sb_kernel_transfer && opts.sb_kernel_device ?
                                    opts.sb_kernel_device : "mlx5_1";
        if (opts.sb_kernel_transfer) image_res.config.gid_idx = opts.sb_kernel_gid;
        image_res.config.ib_port = 1;
        image_res.config.server_name = source ? NULL : (opts.sync_addr ? opts.sync_addr : opts.addr);
        if (resources_create(&image_res))
            return -1;
    }
    image_res.buf = mmap(NULL, image_capacity, PROT_READ | PROT_WRITE,
                         MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
    if (image_res.buf == MAP_FAILED)
        return -1;
    /* This transport buffer belongs to the coordinator, never the restored
     * address space. Keep fork/COW from changing the registered backing. */
    if (madvise(image_res.buf, image_capacity, MADV_DONTFORK))
        return -1;
    if (!opts.sb_kernel_rsocket_proxy) {
        image_res.mr_buf = ibv_reg_mr(image_res.pd, image_res.buf, image_capacity,
                                     IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
        if (!image_res.mr_buf) {
            pr_perror("Registering RAM image buffer");
            return -1;
        }
        if (connect_qp(&image_res, socket, source))
            return -1;
    }
    image_active = 1;
    sb_trace(opts.sb_kernel_rsocket_proxy ? "images.rsocket_buffer_ready" : "images.rdma_ready");
    return 0;
}

/* Snapshot only after the corresponding CRIU producers have closed their
 * images. A bounded overflow is fatal, never a truncated checkpoint. */
static int pack_images(unsigned int phase)
{
    int dfd = get_service_fd(IMG_FD_OFF), scanfd;
    DIR *dir;
    struct dirent *de;
    struct sb_image_manifest *manifest = (void *)image_res.buf;
    size_t offset = sizeof(*manifest);
    uint32_t count = 0;
    scanfd = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (scanfd < 0)
        return -1;
    dir = fdopendir(scanfd);
    if (!dir) {
        close(scanfd);
        return -1;
    }
    while ((de = readdir(dir))) {
        struct sb_image_entry entry = {0};
        struct stat st;
        size_t done = 0;
        int fd;
        if (!image_file_name(de->d_name))
            continue;
        fd = openat(dfd, de->d_name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (fd < 0)
            goto fail;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 ||
            offset > image_capacity - SB_IMAGE_CONTROL - sizeof(entry) ||
            (uint64_t)st.st_size > image_capacity - SB_IMAGE_CONTROL - offset - sizeof(entry)) {
            close(fd);
            pr_err("RAM image snapshot exceeds capacity or has invalid file: %s\n", de->d_name);
            goto fail;
        }
        strcpy(entry.name, de->d_name);
        entry.bytes = st.st_size;
        memcpy(image_res.buf + offset, &entry, sizeof(entry));
        offset += sizeof(entry);
        while (done < entry.bytes) {
            ssize_t n = read(fd, image_res.buf + offset + done, entry.bytes - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                close(fd);
                goto fail;
            }
            done += n;
        }
        close(fd);
        offset = (offset + entry.bytes + 7) & ~7UL;
        if (offset > image_capacity - SB_IMAGE_CONTROL)
            goto fail;
        count++;
    }
    closedir(dir);
    *manifest = (struct sb_image_manifest){SB_IMAGE_MAGIC, 1, phase, count, offset};
    return 0;
fail:
    closedir(dir);
    return -1;
}

static int image_write(size_t offset, size_t size)
{
    if (opts.sb_kernel_rsocket_proxy) {
        size_t done = 0;
        if (image_rsocket_fd < 0 || offset > image_capacity || size > image_capacity - offset)
            return -1;
        while (done < size) {
            size_t chunk = size - done;
            if (chunk > 256 * 1024) chunk = 256 * 1024;
            size_t n = riowrite(image_rsocket_fd, image_res.buf + offset + done,
                                chunk, image_remote_offset + offset + done, 0);
            if (n == (size_t)-1 || !n) return -1;
            done += n;
        }
        return 0;
    }
    struct ibv_send_wr wr = {0}, *bad;
    struct ibv_sge sge = {0};
    sge.addr = (uintptr_t)image_res.buf + offset;
    sge.length = size;
    sge.lkey = image_res.mr_buf->lkey;
    wr.wr_id = offset;
    wr.sg_list = &sge;
    wr.num_sge = 1;
    wr.opcode = IBV_WR_RDMA_WRITE;
    wr.send_flags = IBV_SEND_SIGNALED;
    wr.wr.rdma.remote_addr = image_res.remote_props.addr + offset;
    wr.wr.rdma.rkey = image_res.remote_props.rkey;
    return ibv_post_send(image_res.qp, &wr, &bad) || poll_completion(&image_res) ? -1 : 0;
}

static int image_wait(size_t offset, uint32_t phase)
{
    struct timespec start, now;
    unsigned int spins = 0;
    uint32_t *flag = (void *)(image_res.buf + offset);
    clock_gettime(CLOCK_MONOTONIC, &start);
    while (__atomic_load_n(flag, __ATOMIC_ACQUIRE) != phase) {
#if defined(__x86_64__) || defined(__i386__)
        __asm__ __volatile__("pause" ::: "memory");
#endif
        if (!(++spins & 65535)) {
            clock_gettime(CLOCK_MONOTONIC, &now);
            if (now.tv_sec - start.tv_sec > 90) {
                pr_err("RAM image RDMA barrier timed out: phase=%u offset=%zu\n", phase, offset);
                return -1;
            }
        }
    }
    __sync_synchronize();
    return 0;
}

int sb_images_publish(int socket, unsigned int phase)
{
    struct sb_image_manifest *manifest = (void *)image_res.buf;
    size_t offset, control = image_capacity - SB_IMAGE_CONTROL;
    struct sb_image_rsocket_phase message, ack;
    if (!opts.sb_image_rdma)
        return 0;
    if (!image_active || pack_images(phase) ||
        (opts.sb_kernel_rsocket_proxy && image_rsocket_connect(socket, 1)))
        return -1;
    sb_trace("images.publish_begin");
    for (offset = 0; offset < manifest->bytes; offset += SB_IMAGE_CHUNK) {
        size_t size = manifest->bytes - offset;
        if (size > SB_IMAGE_CHUNK)
            size = SB_IMAGE_CHUNK;
        if (image_write(offset, size))
            return -1;
    }
    /* The commit word follows payload completion on the same RC QP. ACK is
     * written back by the external pageclient after materializing the images. */
    if (opts.sb_kernel_rsocket_proxy) {
        message = (struct sb_image_rsocket_phase){SB_IMAGE_MAGIC, phase, manifest->bytes};
        if (image_rsocket_send(&message, sizeof(message)) ||
            image_rsocket_recv(&ack, sizeof(ack)) ||
            memcmp(&message, &ack, sizeof(message))) return -1;
    } else {
        *(uint32_t *)(image_res.buf + control) = phase;
        if (image_write(control, sizeof(uint32_t)) ||
            image_wait(control + sizeof(uint32_t), phase)) return -1;
    }
    pr_info("SB_IMAGE sent phase=%u files=%u bytes=%llu transport=%s\n", phase,
            manifest->count, (unsigned long long)manifest->bytes,
            opts.sb_kernel_rsocket_proxy ? "rsocket" : "RDMA");
    sb_trace("images.publish_done");
    return 0;
}

int sb_images_receive(int socket, unsigned int phase)
{
    struct sb_image_manifest header;
    struct sb_image_rsocket_phase message;
    size_t offset;
    uint32_t i;
    int dfd = get_service_fd(IMG_FD_OFF);
    if (!opts.sb_image_rdma)
        return 0;
    if (!image_active) return -1;
    if (opts.sb_kernel_rsocket_proxy) {
        if (image_rsocket_connect(socket, 0) ||
            image_rsocket_recv(&message, sizeof(message)) ||
            message.magic != SB_IMAGE_MAGIC || message.phase != phase ||
            message.bytes > image_capacity - SB_IMAGE_CONTROL) return -1;
    } else if (image_wait(image_capacity - SB_IMAGE_CONTROL, phase)) return -1;
    __sync_synchronize();
    memcpy(&header, image_res.buf, sizeof(header));
    if (header.magic != SB_IMAGE_MAGIC || header.version != 1 || header.phase != phase ||
        header.bytes < sizeof(header) || header.bytes > image_capacity - SB_IMAGE_CONTROL ||
        (opts.sb_kernel_rsocket_proxy && header.bytes != message.bytes) ||
        memcmp(&header, image_res.buf, sizeof(header)))
        return -1;
    sb_trace("images.receive_begin");
    /* Validate the entire frame before changing any destination image. */
    offset = sizeof(header);
    for (i = 0; i < header.count; i++) {
        struct sb_image_entry entry;
        if (offset > header.bytes || header.bytes - offset < sizeof(entry))
            return -1;
        memcpy(&entry, image_res.buf + offset, sizeof(entry));
        offset += sizeof(entry);
        if (!image_file_name(entry.name) || entry.bytes > header.bytes - offset)
            return -1;
        offset = (offset + entry.bytes + 7) & ~7UL;
    }
    if (offset != header.bytes)
        return -1;
    {
        int scanfd = openat(dfd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
        DIR *dir;
        struct dirent *de;
        if (scanfd < 0)
            return -1;
        dir = fdopendir(scanfd);
        if (!dir) {
            close(scanfd);
            return -1;
        }
        while ((de = readdir(dir))) {
            int found = 0;
            if (!image_file_name(de->d_name))
                continue;
            offset = sizeof(header);
            for (i = 0; i < header.count; i++) {
                struct sb_image_entry entry;
                memcpy(&entry, image_res.buf + offset, sizeof(entry));
                offset = (offset + sizeof(entry) + entry.bytes + 7) & ~7UL;
                if (!strcmp(entry.name, de->d_name)) {
                    found = 1;
                    break;
                }
            }
            if (!found && unlinkat(dfd, de->d_name, 0)) {
                closedir(dir);
                return -1;
            }
        }
        closedir(dir);
    }
    offset = sizeof(header);
    for (i = 0; i < header.count; i++) {
        struct sb_image_entry entry;
        size_t done = 0;
        int fd;
        memcpy(&entry, image_res.buf + offset, sizeof(entry));
        offset += sizeof(entry);
        fd = openat(dfd, entry.name, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0)
            return -1;
        while (done < entry.bytes) {
            ssize_t n = write(fd, image_res.buf + offset + done, entry.bytes - done);
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                close(fd);
                return -1;
            }
            done += n;
        }
        close(fd);
        offset = (offset + entry.bytes + 7) & ~7UL;
    }
    pr_info("SB_IMAGE received phase=%u files=%u bytes=%llu storage=tmpfs\n", phase,
            header.count, (unsigned long long)header.bytes);
    sb_trace("images.receive_done");
    *(uint32_t *)(image_res.buf + image_capacity - SB_IMAGE_CONTROL + sizeof(uint32_t)) = phase;
    return opts.sb_kernel_rsocket_proxy ? image_rsocket_send(&message, sizeof(message)) :
           image_write(image_capacity - SB_IMAGE_CONTROL + sizeof(uint32_t), sizeof(uint32_t));
}
