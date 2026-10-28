#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#define EXPECTED_SERVER_NAME "Server of Dmitriy Rybakov"

enum {
	EXPECTED_SERVER_NUMBER = 50,
	MAX_MESSAGE = 256,
	MAX_EVENTS = 256,
	GUARD_TIMEOUT_SEC = 30,
};

enum conn_state { ST_IDLE, ST_CONNECTING, ST_READING, ST_SILENT, ST_DONE };

struct conn {
	int fd;
	enum conn_state state;
	bool silent;
	int number;
	int64_t started_ns;
	size_t len;
	char buf[MAX_MESSAGE];
};

struct results {
	unsigned ok, failed, bad_reply;
	unsigned silent_closed, silent_failed;
	double silent_close_sum_ms;
	double *latencies_ms;
	unsigned latencies_count;
	int64_t last_ok_ns;
};

static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void conn_finish(struct conn *c, int epoll_fd)
{
	epoll_ctl(epoll_fd, EPOLL_CTL_DEL, c->fd, NULL);
	close(c->fd);
	c->fd = -1;
	c->state = ST_DONE;
}

static int conn_start(struct conn *c, int epoll_fd, const struct sockaddr_in *addr)
{
	c->fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	if (c->fd < 0)
		return -1;
	c->started_ns = now_ns();
	c->len = 0;

	if (connect(c->fd, (const struct sockaddr *)addr, sizeof *addr) < 0 && errno != EINPROGRESS) {
		close(c->fd);
		c->fd = -1;
		return -1;
	}
	c->state = c->silent ? ST_SILENT : ST_CONNECTING;
	struct epoll_event ev = {
		.events = c->silent ? (uint32_t)EPOLLIN : (uint32_t)EPOLLOUT,
		.data.ptr = c,
	};
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, c->fd, &ev) < 0) {
		close(c->fd);
		c->fd = -1;
		return -1;
	}
	return 0;
}

static bool verify_reply(struct conn *c)
{
	char *sep = strrchr(c->buf, '|');
	if (!sep)
		return false;
	*sep = '\0';
	long server_number = strtol(sep + 1, NULL, 10);
	long sum = c->number + server_number;
	return strcmp(c->buf, EXPECTED_SERVER_NAME) == 0 &&
	       server_number == EXPECTED_SERVER_NUMBER &&
	       sum == c->number + EXPECTED_SERVER_NUMBER;
}

static bool conn_on_event(struct conn *c, int epoll_fd, struct results *r)
{
	if (c->state == ST_SILENT) {
		char tmp[64];
		ssize_t n = recv(c->fd, tmp, sizeof tmp, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return false;
		double ms = (double)(now_ns() - c->started_ns) / 1e6;
		if (n == 0) {
			r->silent_closed++;
			r->silent_close_sum_ms += ms;
		} else {
			r->silent_failed++;
		}
		conn_finish(c, epoll_fd);
		return true;
	}

	if (c->state == ST_CONNECTING) {
		int err = 0;
		socklen_t len = sizeof err;
		getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &len);
		if (err != 0)
			goto fail;

		char req[MAX_MESSAGE];
		int req_len = snprintf(req, sizeof req, "Load client #%d|%d\n", c->number, c->number);
		if (send(c->fd, req, (size_t)req_len, MSG_NOSIGNAL) != req_len)
			goto fail;

		c->state = ST_READING;
		struct epoll_event ev = { .events = EPOLLIN, .data.ptr = c };
		epoll_ctl(epoll_fd, EPOLL_CTL_MOD, c->fd, &ev);
		return false;
	}

	for (;;) {
		ssize_t n = recv(c->fd, c->buf + c->len, sizeof c->buf - 1 - c->len, 0);
		if (n < 0 && (errno == EAGAIN || errno == EINTR))
			return false;
		if (n <= 0)
			goto fail;
		char *nl = memchr(c->buf + c->len, '\n', (size_t)n);
		c->len += (size_t)n;
		if (nl) {
			*nl = '\0';
			break;
		}
		if (c->len == sizeof c->buf - 1)
			goto fail;
	}
	if (verify_reply(c)) {
		r->ok++;
		r->last_ok_ns = now_ns();
		r->latencies_ms[r->latencies_count++] = (double)(r->last_ok_ns - c->started_ns) / 1e6;
	} else {
		r->bad_reply++;
	}
	conn_finish(c, epoll_fd);
	return true;

fail:
	r->failed++;
	conn_finish(c, epoll_fd);
	return true;
}

static int cmp_double(const void *a, const void *b)
{
	double x = *(const double *)a, y = *(const double *)b;
	return (x > y) - (x < y);
}

static void usage(const char *prog)
{
	fprintf(stderr,
		"Usage: %s [-s host] [-p port] [-n total] [-c concurrent] [-S silent]\n"
		"  defaults: -s 127.0.0.1 -p 5555 -n 1000 -c 1000 -S 0\n",
		prog);
}

int main(int argc, char **argv)
{
	const char *host = "127.0.0.1";
	int port = 5555;
	unsigned total = 1000, concurrency = 1000, silent = 0;

	int opt;
	while ((opt = getopt(argc, argv, "s:p:n:c:S:h")) != -1) {
		switch (opt) {
		case 's': host = optarg; break;
		case 'p': port = atoi(optarg); break;
		case 'n': total = (unsigned)strtoul(optarg, NULL, 10); break;
		case 'c': concurrency = (unsigned)strtoul(optarg, NULL, 10); break;
		case 'S': silent = (unsigned)strtoul(optarg, NULL, 10); break;
		default: usage(argv[0]); return EXIT_FAILURE;
		}
	}
	if (total == 0 || concurrency == 0 || port <= 0 || port > 65535) {
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	struct sockaddr_in addr = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port) };
	if (inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
		fprintf(stderr, "invalid IPv4 address: %s\n", host);
		return EXIT_FAILURE;
	}

	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}

	const unsigned nconns = total + silent;
	struct conn *conns = calloc(nconns, sizeof *conns);
	struct results r = { .latencies_ms = calloc(total, sizeof(double)) };
	int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (!conns || !r.latencies_ms || epoll_fd < 0) {
		perror("initialization");
		free(conns);
		free(r.latencies_ms);
		return EXIT_FAILURE;
	}
	for (unsigned i = 0; i < nconns; i++) {
		conns[i].fd = -1;
		conns[i].silent = i < silent;
		conns[i].number = (int)(i % 100) + 1;
	}

	printf("load test: %u requests (up to %u concurrent) + %u silent connections -> %s:%d\n",
	       total, concurrency, silent, host, port);

	const int64_t t0 = now_ns();
	unsigned next = 0, inflight = 0, done = 0, silent_inflight = 0;

	for (; next < silent; next++) {
		if (conn_start(&conns[next], epoll_fd, &addr) < 0) {
			r.silent_failed++;
			done++;
		} else {
			silent_inflight++;
		}
	}

	struct epoll_event events[MAX_EVENTS];
	while (done < nconns) {
		while (next < nconns && inflight < concurrency) {
			if (conn_start(&conns[next], epoll_fd, &addr) < 0) {
				r.failed++;
				done++;
			} else {
				inflight++;
			}
			next++;
		}

		int n = epoll_wait(epoll_fd, events, MAX_EVENTS, 1000);
		if (n < 0 && errno != EINTR) {
			perror("epoll_wait");
			break;
		}
		for (int i = 0; i < n; i++) {
			struct conn *c = events[i].data.ptr;
			bool was_silent = c->silent;
			if (conn_on_event(c, epoll_fd, &r)) {
				done++;
				if (was_silent)
					silent_inflight--;
				else
					inflight--;
			}
		}

		const int64_t now = now_ns();
		for (unsigned i = 0; i < next; i++) {
			struct conn *c = &conns[i];
			if (c->state != ST_DONE && c->fd >= 0 &&
			    now - c->started_ns > (int64_t)GUARD_TIMEOUT_SEC * 1000000000) {
				conn_finish(c, epoll_fd);
				done++;
				if (c->silent) {
					r.silent_failed++;
					silent_inflight--;
				} else {
					r.failed++;
					inflight--;
				}
			}
		}
	}
	const double elapsed_s = (double)(now_ns() - t0) / 1e9;

	qsort(r.latencies_ms, r.latencies_count, sizeof(double), cmp_double);
	double sum = 0;
	for (unsigned i = 0; i < r.latencies_count; i++)
		sum += r.latencies_ms[i];

	printf("successful: %u, connection errors: %u, bad replies: %u\n", r.ok, r.failed, r.bad_reply);
	if (r.latencies_count > 0) {
		unsigned p99 = (unsigned)((double)(r.latencies_count - 1) * 0.99);
		printf("latency, ms: min=%.2f avg=%.2f p99=%.2f max=%.2f\n",
		       r.latencies_ms[0], sum / r.latencies_count,
		       r.latencies_ms[p99], r.latencies_ms[r.latencies_count - 1]);
	}
	if (silent > 0)
		printf("silent: closed by server %u (avg after %.2f s), otherwise %u\n",
		       r.silent_closed,
		       r.silent_closed ? r.silent_close_sum_ms / r.silent_closed / 1000 : 0.0,
		       r.silent_failed);
	const double busy_s = r.ok ? (double)(r.last_ok_ns - t0) / 1e9 : elapsed_s;
	printf("time: %.3f s (responses received in %.3f s), throughput: %.0f requests/s\n",
	       elapsed_s, busy_s, (double)r.ok / busy_s);

	close(epoll_fd);
	free(conns);
	free(r.latencies_ms);
	return (r.ok == total && r.failed == 0 && r.bad_reply == 0 && r.silent_failed == 0)
		       ? EXIT_SUCCESS : EXIT_FAILURE;
}
