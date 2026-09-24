#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <errno.h>
#include <stdint.h>
#include <pthread.h>
#include <netinet/tcp.h>
#include <sys/stat.h>

#include "cr-sync.h"
#include "log.h"

/* Phase names are events, not an ordered counter. Page and process dump
 * complete independently, and distinct sockets must never share progress. */
struct sync_channel {
    dev_t device;
    ino_t inode;
    uint32_t received;
    pthread_mutex_t tx_lock;
    struct sync_channel *next;
};
static struct sync_channel *channels;
static pthread_mutex_t channels_lock = PTHREAD_MUTEX_INITIALIZER;

static struct sync_channel *sync_channel(int fd)
{
    struct sync_channel *channel;
    struct stat st;
    if (fstat(fd, &st)) {
        pr_perror("Identifying sync socket");
        exit(EXIT_FAILURE);
    }
    pthread_mutex_lock(&channels_lock);
    for (channel = channels; channel; channel = channel->next)
        if (channel->device == st.st_dev && channel->inode == st.st_ino)
            break;
    if (!channel) {
        channel = calloc(1, sizeof(*channel));
        if (!channel) {
            pr_perror("Allocating sync channel");
            exit(EXIT_FAILURE);
        }
        channel->device = st.st_dev;
        channel->inode = st.st_ino;
        pthread_mutex_init(&channel->tx_lock, NULL);
        channel->next = channels;
        channels = channel;
    }
    pthread_mutex_unlock(&channels_lock);
    return channel;
}

int sync_transfer(int fd, void *data, size_t size, int sending)
{
    size_t done = 0;
    while (done < size) {
        ssize_t n = sending ? send(fd, (char *)data + done, size - done, MSG_NOSIGNAL)
                            : recv(fd, (char *)data + done, size - done, 0);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0) {
            if (!n)
                errno = ECONNRESET;
            pr_perror("Incomplete sync message (%zu/%zu)", done, size);
            return -1;
        }
        done += n;
    }
    return 0;
}

static void sync_tcp_nodelay(int fd)
{
    int enabled = 1;
    if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enabled, sizeof(enabled))) {
        pr_perror("sync socket TCP_NODELAY");
        exit(EXIT_FAILURE);
    }
}

int syncServerInit(char *ip, int port){
    int server_fd, new_socket;
    struct sockaddr_in address = {0};
    int addrlen = sizeof(address);
    char buffer[1024] = {0};
    char *ack = "ACK from Server";

    // create socket fd
    if((server_fd = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    int reuse = 1;
    if (setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0) {
        pr_perror("sync socket SO_REUSEADDR");
        exit(EXIT_FAILURE);
    }
    // bind socket to port
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);

    if(bind(server_fd, (struct sockaddr *)&address, sizeof(address))<0){
        pr_perror("sync socket bind failed");
        exit(EXIT_FAILURE);
    }

    // listen for connections
    if(listen(server_fd, 3) < 0){
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // accept connection
    if((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen))<0){
        perror("accept");
        exit(EXIT_FAILURE);
    }

    close(server_fd);
    sync_tcp_nodelay(new_socket);
    return new_socket;
}

int syncClientInit(char *ip, int port){
    int sock = 0;
    struct sockaddr_in serv_addr = {0};
    char buffer[1024] = {0};

    // create socket fd
    if((sock = socket(AF_INET, SOCK_STREAM, 0)) < 0){
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port = htons(port);

    // convert IPv4 and IPv6 addresses from text to binary form
    if(inet_pton(AF_INET, ip, &serv_addr.sin_addr)<=0){
        perror("inet_pton");
        exit(EXIT_FAILURE);
    }

    // connect to server
    if(connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0){
        perror("connect");
        exit(EXIT_FAILURE);
    }
    sync_tcp_nodelay(sock);
    return sock;
}

int sync_wait(int sockfd){
    char buffer[4];
    if (sync_transfer(sockfd, "wait", 4, 1) || sync_transfer(sockfd, buffer, 4, 0))
        return -1;
    return memcmp(buffer, "wait", 4) ? -1 : 0;
}


int wait_state(int sockfd, enum STATE state){
    struct sync_channel *channel = sync_channel(sockfd);
    if (state <= READY || state >= STATE_NR)
        exit(EXIT_FAILURE);
    /* Each protocol stream has one receiver. Cache out-of-order events for
     * subsequent waits; sending our own event does not satisfy a peer wait. */
    while (!(channel->received & (1U << state))) {
        enum STATE arrived;
        if (sync_transfer(sockfd, &arrived, sizeof(arrived), 0) ||
            arrived <= READY || arrived >= STATE_NR) {
            pr_err("Invalid or closed sync stream while waiting for phase %d\n", state);
            exit(EXIT_FAILURE);
        }
        channel->received |= 1U << arrived;
    }
    return 0;
}

int notify_peer(int sockfd, enum STATE state){
    struct sync_channel *channel = sync_channel(sockfd);
    int ret;
    pthread_mutex_lock(&channel->tx_lock);
    ret = sync_transfer(sockfd, &state, sizeof(state), 1);
    pthread_mutex_unlock(&channel->tx_lock);
    if (ret)
        exit(EXIT_FAILURE);
    return sizeof(state);
}

int update_state(int sockfd, enum STATE state){
    return notify_peer(sockfd, state);
}

// 服务器函数
int syncServerInit_unix(char *socket_path) {
    int server_fd, new_socket;
    struct sockaddr_un address = {0};
    int addrlen = sizeof(address);

    // 创建 socket fd
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 设置 socket 地址
    address.sun_family = AF_UNIX;
    strncpy(address.sun_path, socket_path, sizeof(address.sun_path) - 1);

    // 绑定 socket 到地址
    unlink(socket_path); // 删除旧的 socket 文件，确保不会冲突
    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        pr_perror("sync socket bind failed");
        exit(EXIT_FAILURE);
    }

    // 监听连接
    if (listen(server_fd, 3) < 0) {
        perror("listen");
        exit(EXIT_FAILURE);
    }

    // 接受连接
    if ((new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen)) < 0) {
        perror("accept");
        exit(EXIT_FAILURE);
    }

    close(server_fd);
    return new_socket;
}

// 客户端函数
int syncClientInit_unix(char *socket_path) {
    int sock = 0;
    struct sockaddr_un serv_addr = {0};

    // 创建 socket fd
    if ((sock = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket failed");
        exit(EXIT_FAILURE);
    }

    // 设置服务器地址
    serv_addr.sun_family = AF_UNIX;
    strncpy(serv_addr.sun_path, socket_path, sizeof(serv_addr.sun_path) - 1);

    // 连接到服务器
    if (connect(sock, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) < 0) {
        perror("connect");
        exit(EXIT_FAILURE);
    }

    return sock;
}





