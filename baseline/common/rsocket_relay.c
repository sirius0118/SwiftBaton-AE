// SPDX-License-Identifier: GPL-2.0
/* A byte-transparent local TCP <-> RDMA rsocket bridge for CRIU baselines.
 * Only loopback TCP is used; all traffic between hosts crosses rsocket.
 * A separate session is created for every local connection. */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <poll.h>
#include <rdma/rsocket.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static atomic_uint_fast64_t next_session = 1;
static volatile sig_atomic_t stopping;

struct bridge {
	int tcp, rdma;
	uint64_t id;
	uint64_t tcp_to_rdma, rdma_to_tcp;
	atomic_int error;
};

struct direction {
	struct bridge *bridge;
	bool to_rdma;
};

static uint64_t monotonic_ns(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (uint64_t)t.tv_sec * 1000000000ULL + t.tv_nsec;
}

static void interrupted(int signal_number)
{
	(void)signal_number;
	stopping = 1;
}

static int endpoint(const char *host, const char *port, struct sockaddr_in *address)
{
	char *end;
	long n = strtol(port, &end, 10);
	if (!*port || *end || n < 1 || n > 65535 ||
	    inet_pton(AF_INET, host, &address->sin_addr) != 1)
		return -1;
	address->sin_family = AF_INET;
	address->sin_port = htons((uint16_t)n);
	return 0;
}

static int local_listener(const struct sockaddr_in *address)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0), yes = 1;
	if (fd < 0)
		return -1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) ||
	    bind(fd, (const struct sockaddr *)address, sizeof(*address)) ||
	    listen(fd, 128)) {
		close(fd);
		return -1;
	}
	return fd;
}

static int local_connect(const struct sockaddr_in *address)
{
	uint64_t deadline = monotonic_ns() + 5000000000ULL;
	for (;;) {
		int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
		if (fd < 0)
			return -1;
		if (!connect(fd, (const struct sockaddr *)address, sizeof(*address)))
			return fd;
		int error = errno;
		close(fd);
		if (error != ECONNREFUSED || monotonic_ns() >= deadline) {
			errno = error;
			return -1;
		}
		struct timespec delay = {.tv_nsec = 5000000};
		nanosleep(&delay, NULL);
	}
}

static int configure_rdma_socket(int fd)
{
	int buffer_bytes = 8 * 1024 * 1024;
	int queue_entries = 512;
	if (rsetsockopt(fd, SOL_SOCKET, SO_RCVBUF, &buffer_bytes, sizeof(buffer_bytes)) ||
	    rsetsockopt(fd, SOL_SOCKET, SO_SNDBUF, &buffer_bytes, sizeof(buffer_bytes)) ||
	    rsetsockopt(fd, SOL_RDMA, RDMA_RQSIZE, &queue_entries, sizeof(queue_entries)) ||
	    rsetsockopt(fd, SOL_RDMA, RDMA_SQSIZE, &queue_entries, sizeof(queue_entries))) {
		perror("configure rsocket buffers/queues");
		return -1;
	}
	return 0;
}

static int relay_rdma_connect(const struct sockaddr_in *address)
{
	int fd = rsocket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0)
		return -1;
	if (configure_rdma_socket(fd) ||
	    rconnect(fd, (const struct sockaddr *)address, sizeof(*address))) {
		rclose(fd);
		return -1;
	}
	return fd;
}

static void *copy_direction(void *arg)
{
	struct direction *direction = arg;
	struct bridge *bridge = direction->bridge;
	char buffer[65536];
	for (;;) {
		ssize_t n = direction->to_rdma ? recv(bridge->tcp, buffer, sizeof(buffer), 0) :
					      rrecv(bridge->rdma, buffer, sizeof(buffer), 0);
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0) {
			atomic_store(&bridge->error, 1);
			break;
		}
		if (!n)
			break;
		for (ssize_t offset = 0; offset < n;) {
			ssize_t sent = direction->to_rdma ?
				rsend(bridge->rdma, buffer + offset, n - offset, 0) :
				send(bridge->tcp, buffer + offset, n - offset, MSG_NOSIGNAL);
			if (sent < 0 && errno == EINTR)
				continue;
			if (sent <= 0) {
				atomic_store(&bridge->error, 1);
				goto done;
			}
			offset += sent;
		}
		if (direction->to_rdma)
			bridge->tcp_to_rdma += n;
		else
			bridge->rdma_to_tcp += n;
	}
done:
	if (direction->to_rdma)
		rshutdown(bridge->rdma, atomic_load(&bridge->error) ? SHUT_RDWR : SHUT_WR);
	else
		shutdown(bridge->tcp, atomic_load(&bridge->error) ? SHUT_RDWR : SHUT_WR);
	return NULL;
}

static void *serve_bridge(void *arg)
{
	struct bridge *bridge = arg;
	struct direction outbound = {bridge, true}, inbound = {bridge, false};
	pthread_t a, b;
	uint64_t begin = monotonic_ns();
	int first = pthread_create(&a, NULL, copy_direction, &outbound);
	int second = first ? first : pthread_create(&b, NULL, copy_direction, &inbound);
	if (second) {
		atomic_store(&bridge->error, 1);
		shutdown(bridge->tcp, SHUT_RDWR);
		rshutdown(bridge->rdma, SHUT_RDWR);
	}
	if (!first)
		pthread_join(a, NULL);
	if (!second)
		pthread_join(b, NULL);
	fprintf(stderr,
		"relay session=%" PRIu64 " tcp_to_rdma=%" PRIu64 " rdma_to_tcp=%" PRIu64
		" elapsed_ms=%.3f error=%d\n", bridge->id, bridge->tcp_to_rdma,
		bridge->rdma_to_tcp, (monotonic_ns() - begin) / 1e6, atomic_load(&bridge->error));
	close(bridge->tcp);
	rclose(bridge->rdma);
	free(bridge);
	return NULL;
}

static int start_bridge(int tcp, int rdma)
{
	struct bridge *bridge = calloc(1, sizeof(*bridge));
	if (!bridge) {
		close(tcp);
		rclose(rdma);
		return -1;
	}
	bridge->tcp = tcp;
	bridge->rdma = rdma;
	bridge->id = atomic_fetch_add(&next_session, 1);
	atomic_init(&bridge->error, 0);
	pthread_t thread;
	int ret = pthread_create(&thread, NULL, serve_bridge, bridge);
	if (ret) {
		close(tcp);
		rclose(rdma);
		free(bridge);
		errno = ret;
		return -1;
	}
	pthread_detach(thread);
	return 0;
}

int main(int argc, char **argv)
{
	struct sockaddr_in local = {0}, remote = {0};
	if (argc != 6 || (strcmp(argv[1], "listen") && strcmp(argv[1], "connect")) ||
	    endpoint(argv[2], argv[3], &local) || endpoint(argv[4], argv[5], &remote)) {
		fprintf(stderr,
			"usage: %s listen RDMA_BIND_IP RDMA_PORT LOCAL_IP LOCAL_PORT\n"
			"       %s connect LOCAL_BIND_IP LOCAL_PORT RDMA_PEER_IP RDMA_PORT\n",
			argv[0], argv[0]);
		return 2;
	}
	if (signal(SIGINT, interrupted) == SIG_ERR || signal(SIGTERM, interrupted) == SIG_ERR)
		return 1;
	signal(SIGPIPE, SIG_IGN);
	bool server = !strcmp(argv[1], "listen");
	int listener;
	if (server) {
		listener = rsocket(AF_INET, SOCK_STREAM, 0);
		if (listener < 0 || configure_rdma_socket(listener) ||
		    rbind(listener, (struct sockaddr *)&local, sizeof(local)) ||
		    rlisten(listener, 128) || rfcntl(listener, F_SETFL, O_NONBLOCK)) {
			perror("RDMA listen");
			return 1;
		}
	} else {
		listener = local_listener(&local);
		if (listener < 0) {
			perror("local TCP listen");
			return 1;
		}
	}
	while (!stopping) {
		if (!server) {
			struct pollfd ready = {.fd = listener, .events = POLLIN};
			int polled = poll(&ready, 1, 100);
			if (polled < 0 && errno == EINTR)
				continue;
			if (polled < 0 || (polled && (ready.revents & (POLLERR | POLLNVAL)))) {
				perror("relay poll");
				return 1;
			}
			if (!polled)
				continue;
		}
		int accepted = server ? raccept(listener, NULL, NULL) :
				       accept4(listener, NULL, NULL, SOCK_CLOEXEC);
		if (accepted < 0) {
			if (server && (errno == EAGAIN || errno == EWOULDBLOCK)) {
				struct timespec delay = {.tv_nsec = 5000000};
				nanosleep(&delay, NULL);
				continue;
			}
			if (errno == EINTR && stopping)
				break;
			if (errno == EINTR)
				continue;
			perror("relay accept");
			return 1;
		}
		int peer = server ? local_connect(&remote) : relay_rdma_connect(&remote);
		if (peer < 0) {
			perror("relay peer connect");
			if (server)
				rclose(accepted);
			else
				close(accepted);
			continue;
		}
		if (start_bridge(server ? peer : accepted, server ? accepted : peer)) {
			perror("start relay session");
			return 1;
		}
	}
	if (server)
		rclose(listener);
	else
		close(listener);
	return 0;
}
