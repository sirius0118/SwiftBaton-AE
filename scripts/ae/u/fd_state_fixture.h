/* Real kernel FD state retained across the migration; no synthetic results. */
#include <fcntl.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/timerfd.h>
#include <time.h>

struct fd_group {
    int normal, semaphore, epoll, pipe[2], unix_pair[2], udp[2], tcp[2], listener, file[2], timer;
    uint64_t counter;
    off_t offset;
    unsigned char file_byte;
    struct sockaddr_in udp_addr[2], tcp_addr[2];
    time_t deadline;
};
static struct fd_group *fd_groups;
static unsigned fd_group_count;
static bool fd_adversarial;
static pthread_mutex_t fd_churn_lock = PTHREAD_MUTEX_INITIALIZER;
static int fd_churn_event = -1, fd_churn_socket = -1;
static uint64_t fd_churn_epoch, fd_verified_epoch;
static int fd_churn_type;
static uint64_t fd_state_errors;
static void fd_payload(char *buf, size_t size, unsigned group, unsigned kind)
{
    for (size_t i = 0; i < size; i++) buf[i] = (char)(19 + group * 37 + kind * 11 + i * 13);
}
static void fd_enqueue(int fd, unsigned group, unsigned kind, bool socket_fd)
{
    char buf[61]; fd_payload(buf, sizeof(buf), group, kind);
    ssize_t n = socket_fd ? send(fd, buf, sizeof(buf), MSG_NOSIGNAL) : write(fd, buf, sizeof(buf));
    if (n != sizeof(buf)) fail("FD queue populate");
}
static unsigned fd_verify_queue(int input, int output, unsigned group, unsigned kind, bool socket_fd, bool retain)
{
    char buf[61], expected[61]; fd_payload(expected, sizeof(expected), group, kind);
    ssize_t n = socket_fd ? recv(input, buf, sizeof(buf), MSG_DONTWAIT) : read(input, buf, sizeof(buf));
    unsigned bad = n != sizeof(buf) || (n == sizeof(buf) && memcmp(buf, expected, sizeof(buf)));
    /* Restore precisely the bytes consumed so source and target checks can
     * both validate the same nonempty queues. Never manufacture a pass. */
    if (n > 0 && retain) {
        ssize_t written = socket_fd ? send(output, buf, n, MSG_NOSIGNAL) : write(output, buf, n);
        bad += written != n;
    }
    return bad;
}
static unsigned fd_address(int fd, const struct sockaddr_in *expected, bool peer)
{
    struct sockaddr_in actual = {0}; socklen_t len = sizeof(actual);
    int ret = peer ? getpeername(fd, (void *)&actual, &len) : getsockname(fd, (void *)&actual, &len);
    return ret || len != sizeof(actual) || actual.sin_family != expected->sin_family ||
        actual.sin_port != expected->sin_port || actual.sin_addr.s_addr != expected->sin_addr.s_addr;
}
static void *fd_churn(void *unused)
{
    (void)unused;
    for (;;) {
        pthread_mutex_lock(&fd_churn_lock);
        close(fd_churn_event); close(fd_churn_socket);
        fd_churn_epoch++;
        uint64_t count = (UINT64_C(1) << 42) + fd_churn_epoch;
        fd_churn_type = fd_churn_epoch & 1 ? SOCK_DGRAM : SOCK_STREAM;
        fd_churn_event = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
        fd_churn_socket = socket(AF_INET, fd_churn_type | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        if (fd_churn_event < 0 || fd_churn_socket < 0 || write(fd_churn_event, &count, 8) != 8) fail("FD churn");
        pthread_mutex_unlock(&fd_churn_lock);
        usleep(1000);
    }
    return NULL;
}
static void fd_state_init(void)
{
    const char *requested = getenv("SB_AE_FD_GROUPS");
    if (!requested) return;
    fd_group_count = strtoul(requested, NULL, 10);
    fd_adversarial = getenv("SB_AE_FD_ADVERSARIAL") != NULL;
    if (!fd_group_count || fd_group_count > 512) fail("FD group count");
    fd_groups = calloc(fd_group_count, sizeof(*fd_groups));
    if (!fd_groups) fail("FD groups allocate");
    for (unsigned i = 0; i < fd_group_count; i++) {
        struct fd_group *g = &fd_groups[i];
        uint64_t three = 3;
        g->counter = (UINT64_C(1) << 40) + i + 1;
        g->normal = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        g->semaphore = eventfd(0, (fd_adversarial ? EFD_SEMAPHORE : 0) | EFD_NONBLOCK | EFD_CLOEXEC);
        g->epoll = epoll_create1(EPOLL_CLOEXEC);
        if (g->normal < 0 || g->semaphore < 0 || g->epoll < 0 ||
            write(g->normal, &g->counter, 8) != 8 || write(g->semaphore, &three, 8) != 8) fail("FD events");
        struct epoll_event event = {.events = EPOLLIN, .data.u64 = UINT64_C(0xa100000000) + 2*i};
        if (epoll_ctl(g->epoll, EPOLL_CTL_ADD, g->normal, &event)) fail("FD normal watch");
        event.data.u64++;
        if (epoll_ctl(g->epoll, EPOLL_CTL_ADD, g->semaphore, &event)) fail("FD semaphore watch");
        if (pipe2(g->pipe, O_NONBLOCK | O_CLOEXEC) ||
            socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, g->unix_pair)) fail("FD pipe/pair");
        fd_enqueue(g->pipe[1], i, 0, false);
        fd_enqueue(g->unix_pair[0], i, 1, true); fd_enqueue(g->unix_pair[1], i, 2, true);
        for (unsigned j = 0; j < 2; j++) {
            g->udp[j] = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            g->udp_addr[j] = (struct sockaddr_in){.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
            socklen_t len = sizeof(g->udp_addr[j]);
            if (g->udp[j] < 0 || bind(g->udp[j], (void *)&g->udp_addr[j], len) ||
                getsockname(g->udp[j], (void *)&g->udp_addr[j], &len)) fail("FD UDP bind");
        }
        for (unsigned j = 0; j < 2; j++) {
            if (connect(g->udp[j], (void *)&g->udp_addr[1-j], sizeof(g->udp_addr[0]))) fail("FD UDP connect");
            if (fd_adversarial) fd_enqueue(g->udp[j], i, 3+j, true);
        }
        struct sockaddr_in address = {.sin_family=AF_INET, .sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
        socklen_t len = sizeof(address);
        g->listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        g->tcp[0] = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (g->listener < 0 || g->tcp[0] < 0 || bind(g->listener, (void *)&address, len) ||
            listen(g->listener, 4) || getsockname(g->listener, (void *)&address, &len) ||
            connect(g->tcp[0], (void *)&address, len)) fail("FD TCP connect");
        g->tcp[1] = accept4(g->listener, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (g->tcp[1] < 0 || fcntl(g->tcp[0], F_SETFL, O_NONBLOCK)) fail("FD TCP accept");
        for (unsigned j = 0; j < 2; j++) {
            len = sizeof(g->tcp_addr[j]);
            if (getsockname(g->tcp[j], (void *)&g->tcp_addr[j], &len)) fail("FD TCP address");
            fd_enqueue(g->tcp[j], i, 5+j, true);
        }
        const char *fixture_path = getenv("SB_AE_FIXTURE_PATH");
        g->file[0] = open(fixture_path ? fixture_path : "/ae-memory-fixture", O_RDONLY | O_CLOEXEC);
        g->file[1] = fcntl(g->file[0], F_DUPFD_CLOEXEC, 0);
        g->offset = 4096 + 64*i;
        if (g->file[0] < 0 || g->file[1] < 0 || pread(g->file[0], &g->file_byte, 1, g->offset) != 1 ||
            lseek(g->file[0], g->offset, SEEK_SET) != g->offset) fail("FD shared file offset");
        g->timer = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK | TFD_CLOEXEC);
        g->deadline = time(NULL) + 3600;
        struct itimerspec spec = {.it_value.tv_sec = g->deadline};
        if (g->timer < 0 || timerfd_settime(g->timer, TFD_TIMER_ABSTIME, &spec, NULL)) fail("FD timer");
    }
    pthread_t changing;
    if (pthread_create(&changing, NULL, fd_churn, NULL)) fail("FD churn thread");
}
static uint64_t fd_state_verify(void)
{
    uint64_t bad = 0;
    for (unsigned i = 0; i < fd_group_count; i++) {
        struct fd_group *g = &fd_groups[i];
        struct epoll_event ready[3]; unsigned seen = 0;
        int n = epoll_wait(g->epoll, ready, 3, 0);
        bad += n != 2;
        for (int j = 0; j < n; j++) {
            uint64_t tag = ready[j].data.u64 - (UINT64_C(0xa100000000) + 2*i);
            if (tag > 1 || ready[j].events != EPOLLIN) bad++;
            else seen |= 1U << tag;
        }
        bad += seen != 3;
        uint64_t value = 0;
        n = read(g->normal, &value, 8); bad += n != 8 || value != g->counter;
        if (n == 8) bad += write(g->normal, &value, 8) != 8;
        value = 0; n = read(g->semaphore, &value, 8); bad += n != 8 || value != (fd_adversarial ? 1 : 3);
        if (n == 8) bad += write(g->semaphore, &value, 8) != 8;
        bad += fd_verify_queue(g->pipe[0], g->pipe[1], i, 0, false, true);
        if (!fd_adversarial) for (unsigned j=0; j<2; j++) fd_enqueue(g->udp[j], i, 3+j, true);
        for (unsigned j = 0; j < 2; j++) {
            bad += fd_verify_queue(g->unix_pair[1-j], g->unix_pair[j], i, 1+j, true, true);
            bad += fd_verify_queue(g->udp[1-j], g->udp[j], i, 3+j, true, fd_adversarial);
            bad += fd_verify_queue(g->tcp[1-j], g->tcp[j], i, 5+j, true, true);
            bad += fd_address(g->udp[j], &g->udp_addr[j], false) + fd_address(g->udp[j], &g->udp_addr[1-j], true);
            bad += fd_address(g->tcp[j], &g->tcp_addr[j], false) + fd_address(g->tcp[j], &g->tcp_addr[1-j], true);
        }
        unsigned char byte = 0;
        bad += lseek(g->file[0], 0, SEEK_CUR) != g->offset || lseek(g->file[1], 0, SEEK_CUR) != g->offset;
        bad += read(g->file[0], &byte, 1) != 1 || byte != g->file_byte;
        bad += lseek(g->file[1], 0, SEEK_CUR) != g->offset + 1;
        bad += lseek(g->file[1], g->offset, SEEK_SET) != g->offset;
        struct itimerspec spec;
        if (timerfd_gettime(g->timer, &spec)) bad++;
        else bad += spec.it_interval.tv_sec || spec.it_interval.tv_nsec ||
            llabs((long long)time(NULL) + spec.it_value.tv_sec - g->deadline) > 2;
        const int fds[] = {g->normal,g->semaphore,g->epoll,g->pipe[0],g->pipe[1],g->unix_pair[0],g->unix_pair[1],
            g->udp[0],g->udp[1],g->tcp[0],g->tcp[1],g->listener,g->file[0],g->file[1],g->timer};
        for (unsigned j=0;j<sizeof(fds)/sizeof(fds[0]);j++) {
            bad += !(fcntl(fds[j], F_GETFD) & FD_CLOEXEC);
            if (j != 2 && j != 12 && j != 13) bad += !(fcntl(fds[j], F_GETFL) & O_NONBLOCK);
        }
    }
    if (fd_group_count) {
        pthread_mutex_lock(&fd_churn_lock);
        uint64_t value = 0, expected = (UINT64_C(1)<<42) + fd_churn_epoch;
        ssize_t n = read(fd_churn_event, &value, 8);
        bad += !fd_churn_epoch || n != 8 || value != expected;
        if (n == 8) bad += write(fd_churn_event, &value, 8) != 8;
        int type = 0; socklen_t len = sizeof(type);
        bad += getsockopt(fd_churn_socket, SOL_SOCKET, SO_TYPE, &type, &len) || type != fd_churn_type;
        fd_verified_epoch = fd_churn_epoch;
        pthread_mutex_unlock(&fd_churn_lock);
    }
    fd_state_errors += bad;
    return fd_state_errors;
}
