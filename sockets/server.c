#define _GNU_SOURCE

#include <arpa/inet.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define SERVER_NAME "Server of Dmitriy Rybakov"

enum
{
	DEFAULT_PORT = 5555,
	DEFAULT_PROCESSES = 2,
	DEFAULT_THREADS = 4,
	MAX_PROCESSES = 64,
	DEFAULT_TIMEOUT_SEC = 10,
	DEFAULT_STATS_SEC = 2,
	MIN_UPTIME_FOR_RESPAWN_MS = 1000,
	QUEUE_CAPACITY = 4096,
	MAX_MESSAGE = 256,
	MAX_NAME = 128,
	MAX_EVENTS = 64,
	SERVER_NUMBER = 50,
	NUMBER_MIN = 1,
	NUMBER_MAX = 100,
};

static bool g_verbose = true;
static _Thread_local char g_tag[16] = "master";

static void write_all(int fd, const char* buf, size_t len)
{
	while (len > 0)
	{
		const ssize_t n = write(fd, buf, len);
		if (n < 0 && errno == EINTR)
			continue;
		if (n <= 0)
			return;
		buf += n;
		len -= (size_t)n;
	}
}

__attribute__((format(printf, 1, 2))) static void log_msg(const char* fmt, ...)
{
	char line[1024];
	struct timespec ts;
	struct tm tm;

	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	const int n = snprintf(line, sizeof line, "%02d:%02d:%02d.%03ld [%-6s] ",
		tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000, g_tag);

	va_list ap;
	va_start(ap, fmt);
	const int m = vsnprintf(line + n, sizeof line - (size_t)n, fmt, ap);
	va_end(ap);

	size_t len = (size_t)n + (m > 0 ? (size_t)m : 0);
	if (len > sizeof line - 2)
		len = sizeof line - 2;
	line[len++] = '\n';
	write_all(STDOUT_FILENO, line, len);
}

#define LOG_DEBUG(...)            \
	do                            \
	{                             \
		if (g_verbose)            \
			log_msg(__VA_ARGS__); \
	} while (0)

static const char* errstr(int err)
{
	const char* s = strerrordesc_np(err);
	return s
		? s
		: "unknown error";
}

#define LOG_ERRNO(what) log_msg("ERROR: %s: %s", (what), errstr(errno))


static int64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

_Static_assert(ATOMIC_LONG_LOCK_FREE == 2, "lock-free 64-bit atomics required");

struct proc_stats
{
	_Alignas(64)
		atomic_uint_fast64_t active;
	atomic_uint_fast64_t accepted;
	atomic_uint_fast64_t processed;
	atomic_uint_fast64_t failed;
	atomic_uint_fast64_t timeouts;
	atomic_uint_fast64_t rejected;
	atomic_uint_fast64_t busy_ns;
};

struct shared_state
{
	atomic_uint_fast64_t next_conn_id;
	struct proc_stats procs[MAX_PROCESSES];
};

static struct shared_state* g_shared;
static struct proc_stats* g_my_stats;

struct stats_snapshot
{
	uint64_t active, accepted, processed, failed, timeouts, rejected, busy_ns;
};

static struct stats_snapshot stats_read(const struct proc_stats* p)
{
	return (struct stats_snapshot){
		.active = atomic_load(&p->active),
		.accepted = atomic_load(&p->accepted),
		.processed = atomic_load(&p->processed),
		.failed = atomic_load(&p->failed),
		.timeouts = atomic_load(&p->timeouts),
		.rejected = atomic_load(&p->rejected),
		.busy_ns = atomic_load(&p->busy_ns),
	};
}

static struct stats_snapshot stats_total(unsigned nprocs)
{
	struct stats_snapshot t = { 0 };
	for (unsigned i = 0; i < nprocs; i++)
	{
		struct stats_snapshot s = stats_read(&g_shared->procs[i]);
		t.active += s.active;
		t.accepted += s.accepted;
		t.processed += s.processed;
		t.failed += s.failed;
		t.timeouts += s.timeouts;
		t.rejected += s.rejected;
		t.busy_ns += s.busy_ns;
	}
	return t;
}

static void stats_print(const char* title, const struct stats_snapshot* s)
{
	const double avg_ms = s->processed
		? (double)s->busy_ns / (double)s->processed / 1e6
		: 0.0;
	log_msg("%s: active=%" PRIu64 " accepted=%" PRIu64 " processed=%" PRIu64
			" errors=%" PRIu64 " (timeouts=%" PRIu64 ") rejected=%" PRIu64
			" avg_time=%.3f ms",
		title, s->active, s->accepted, s->processed, s->failed, s->timeouts,
		s->rejected, avg_ms);
}

struct config
{
	uint16_t port;
	unsigned processes;
	unsigned threads;
	int timeout_ms;
	unsigned stats_interval_sec;
};

static void close_fd(int* fd)
{
	if (*fd >= 0)
	{
		close(*fd);
		*fd = -1;
	}
}

static int epoll_watch(int epoll_fd, int fd, uint32_t events)
{
	struct epoll_event ev = { .events = events, .data.fd = fd };
	if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) < 0)
	{
		LOG_ERRNO("epoll_ctl(ADD)");
		return -1;
	}
	return 0;
}


enum io_status
{
	IO_OK,
	IO_TIMEOUT,
	IO_CLOSED,
	IO_TOO_LONG,
	IO_ERROR
};

static int remaining_ms(const int64_t deadline_ns)
{
	const int64_t left = (deadline_ns - now_ns()) / 1000000;
	return left > 0 ? (int)left : 0;
}

static enum io_status wait_fd(int fd, short events, int64_t deadline_ns)
{
	for (;;)
	{
		struct pollfd pfd = { .fd = fd, .events = events };
		const int rc = poll(&pfd, 1, remaining_ms(deadline_ns));
		if (rc > 0)
			return IO_OK;
		if (rc == 0)
			return IO_TIMEOUT;
		if (errno != EINTR)
			return IO_ERROR;
	}
}

static enum io_status recv_line(const int fd, char* buf, size_t cap, size_t* out_len, int64_t deadline_ns)
{
	size_t len = 0;

	for (;;)
	{
		const ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
		if (n > 0)
		{
			char* nl = memchr(buf + len, '\n', (size_t)n);
			len += (size_t)n;
			if (nl)
			{
				*nl = '\0';
				*out_len = (size_t)(nl - buf);
				return IO_OK;
			}
			if (len == cap - 1)
				return IO_TOO_LONG;
			continue;
		}
		if (n == 0)
			return IO_CLOSED;
		if (errno == EINTR)
			continue;
		if (errno != EAGAIN && errno != EWOULDBLOCK)
			return IO_ERROR;

		enum io_status st = wait_fd(fd, POLLIN, deadline_ns);
		if (st != IO_OK)
			return st;
	}
}

static enum io_status send_all(int fd, const char* buf, size_t len, int64_t deadline_ns)
{
	while (len > 0)
	{
		ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
		if (n > 0)
		{
			buf += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			return IO_ERROR;

		enum io_status st = wait_fd(fd, POLLOUT, deadline_ns);
		if (st != IO_OK)
			return st;
	}
	return IO_OK;
}

struct message
{
	char name[MAX_NAME];
	long number;
};

static bool parse_message(char* line, struct message* msg)
{
	size_t len = strlen(line);
	if (len > 0 && line[len - 1] == '\r')
		line[--len] = '\0';

	char* sep = strrchr(line, '|');
	if (!sep)
		return false;
	*sep = '\0';

	size_t name_len = (size_t)(sep - line);
	if (name_len == 0 || name_len >= sizeof msg->name)
		return false;
	for (const char* p = line; *p; p++)
		if (!isprint((unsigned char)*p))
			return false;

	const char* num = sep + 1;
	char* end;
	errno = 0;
	const long value = strtol(num, &end, 10);
	if (errno != 0 || end == num || *end != '\0' || value < INT_MIN || value > INT_MAX)
		return false;

	memcpy(msg->name, line, name_len + 1);
	msg->number = value;
	return true;
}

struct worker_ctx
{
	const struct config* cfg;
	unsigned slot;
	pid_t master_pid;
	int listen_fd;
	int epoll_fd;
	int signal_fd;
	int reserve_fd;
};

struct job
{
	int fd;
	uint64_t id;
	struct sockaddr_in peer;
};

static void handle_client(const struct worker_ctx* ctx, const struct job* job)
{
	const int64_t started = now_ns();
	const int64_t deadline = started + (int64_t)ctx->cfg->timeout_ms * 1000000;
	const uint64_t id = job->id;
	bool ok = false;

	char peer[INET_ADDRSTRLEN];
	inet_ntop(AF_INET, &job->peer.sin_addr, peer, sizeof peer);
	LOG_DEBUG("#%" PRIu64 " picked up for processing (%s:%u, fd=%d)",
		id, peer, ntohs(job->peer.sin_port), job->fd);

	char buf[MAX_MESSAGE];
	size_t len = 0;
	switch (recv_line(job->fd, buf, sizeof buf, &len, deadline))
	{
	case IO_OK:
		break;
	case IO_TIMEOUT:
		atomic_fetch_add(&g_my_stats->timeouts, 1);
		log_msg("#%" PRIu64 " TIMEOUT: request not fully received within %d ms — disconnecting",
			id, ctx->cfg->timeout_ms);
		goto done;
	case IO_CLOSED:
		log_msg("#%" PRIu64 " client disconnected before sending request", id);
		goto done;
	case IO_TOO_LONG:
		log_msg("#%" PRIu64 " request exceeds %d bytes — disconnecting", id, MAX_MESSAGE - 1);
		goto done;
	case IO_ERROR:
		log_msg("#%" PRIu64 " recv error: %s", id, errstr(errno));
		goto done;
	}
	LOG_DEBUG("#%" PRIu64 " recv: message received (%zu bytes): \"%s\"", id, len + 1, buf);

	struct message msg;
	if (!parse_message(buf, &msg))
	{
		log_msg("#%" PRIu64 " invalid message format, expected \"<name>|<number>\"", id);
		goto done;
	}

	const long long sum = (long long)msg.number + SERVER_NUMBER;
	LOG_DEBUG("#%" PRIu64 " Client name   : %s", id, msg.name);
	LOG_DEBUG("#%" PRIu64 " Server name   : %s", id, SERVER_NAME);
	LOG_DEBUG("#%" PRIu64 " Client number : %ld", id, msg.number);
	LOG_DEBUG("#%" PRIu64 " Server number : %d", id, SERVER_NUMBER);
	LOG_DEBUG("#%" PRIu64 " Sum           : %lld", id, sum);

	char reply[MAX_MESSAGE];
	int reply_len = snprintf(reply, sizeof reply, "%s|%d\n", SERVER_NAME, SERVER_NUMBER);
	enum io_status st = send_all(job->fd, reply, (size_t)reply_len, deadline);
	if (st == IO_TIMEOUT)
	{
		atomic_fetch_add(&g_my_stats->timeouts, 1);
		log_msg("#%" PRIu64 " TIMEOUT: client not accepting response — disconnecting", id);
		goto done;
	}
	if (st != IO_OK)
	{
		log_msg("#%" PRIu64 " send error: %s", id, errstr(errno));
		goto done;
	}
	LOG_DEBUG("#%" PRIu64 " send: response sent (%d bytes): \"%s|%d\"",
		id, reply_len, SERVER_NAME, SERVER_NUMBER);
	ok = true;

	if (msg.number < NUMBER_MIN || msg.number > NUMBER_MAX)
	{
		log_msg("#%" PRIu64 " number %ld out of range [%d..%d] — server shutdown command",
			id, msg.number, NUMBER_MIN, NUMBER_MAX);
		if (kill(ctx->master_pid, SIGUSR1) < 0)
			LOG_ERRNO("kill(master, SIGUSR1)");
	}

done:
	close(job->fd);
	LOG_DEBUG("#%" PRIu64 " close: connection closed", id);

	atomic_fetch_sub(&g_my_stats->active, 1);
	if (ok)
	{
		atomic_fetch_add(&g_my_stats->processed, 1);
		atomic_fetch_add(&g_my_stats->busy_ns, (uint64_t)(now_ns() - started));
	}
	else
	{
		atomic_fetch_add(&g_my_stats->failed, 1);
	}
}

struct thread_pool
{
	pthread_t* threads;
	unsigned nthreads;
	atomic_uint next_index;

	struct job* queue;
	size_t capacity, head, count;
	bool stopping;

	pthread_mutex_t lock;
	pthread_cond_t has_work;

	const struct worker_ctx* ctx;
};

static void* pool_thread_main(void* arg)
{
	struct thread_pool* pool = arg;
	unsigned index = atomic_fetch_add(&pool->next_index, 1) + 1;
	snprintf(g_tag, sizeof g_tag, "p%u.w%02u", pool->ctx->slot, index % 100);
	LOG_DEBUG("pool thread started");

	for (;;)
	{
		pthread_mutex_lock(&pool->lock);
		while (pool->count == 0 && !pool->stopping)
			pthread_cond_wait(&pool->has_work, &pool->lock);
		if (pool->count == 0)
		{
			pthread_mutex_unlock(&pool->lock);
			break;
		}
		struct job job = pool->queue[pool->head];
		pool->head = (pool->head + 1) % pool->capacity;
		pool->count--;
		pthread_mutex_unlock(&pool->lock);

		handle_client(pool->ctx, &job);
	}

	LOG_DEBUG("pool thread finished");
	return NULL;
}

static void pool_destroy(struct thread_pool* pool)
{
	pthread_mutex_lock(&pool->lock);
	pool->stopping = true;
	pthread_cond_broadcast(&pool->has_work);
	pthread_mutex_unlock(&pool->lock);

	for (unsigned i = 0; i < pool->nthreads; i++)
		pthread_join(pool->threads[i], NULL);

	pthread_cond_destroy(&pool->has_work);
	pthread_mutex_destroy(&pool->lock);
	free(pool->threads);
	free(pool->queue);
	*pool = (struct thread_pool){ 0 };
}

static int pool_init(
	struct thread_pool* pool,
	unsigned nthreads,
	size_t capacity,
	const struct worker_ctx* ctx)
{
	*pool = (struct thread_pool){ .capacity = capacity, .ctx = ctx };
	pthread_mutex_init(&pool->lock, NULL);
	pthread_cond_init(&pool->has_work, NULL);

	pool->queue = calloc(capacity, sizeof *pool->queue);
	pool->threads = calloc(nthreads, sizeof *pool->threads);
	if (!pool->queue || !pool->threads)
	{
		log_msg("ERROR: not enough memory for thread pool");
		goto fail;
	}

	for (unsigned i = 0; i < nthreads; i++)
	{
		int rc = pthread_create(&pool->threads[i], NULL, pool_thread_main, pool);
		if (rc != 0)
		{
			log_msg("ERROR: pthread_create: %s", errstr(rc));
			goto fail;
		}
		pool->nthreads++;
	}
	return 0;

fail:
	pool_destroy(pool);
	return -1;
}

static bool pool_submit(struct thread_pool* pool, const struct job* job)
{
	bool queued = false;

	pthread_mutex_lock(&pool->lock);
	if (!pool->stopping && pool->count < pool->capacity)
	{
		pool->queue[(pool->head + pool->count) % pool->capacity] = *job;
		pool->count++;
		queued = true;
		pthread_cond_signal(&pool->has_work);
	}
	pthread_mutex_unlock(&pool->lock);
	return queued;
}

static void accept_clients(struct worker_ctx* ctx, struct thread_pool* pool)
{
	for (;;)
	{
		struct job job;
		socklen_t peer_len = sizeof job.peer;
		job.fd = accept4(ctx->listen_fd, (struct sockaddr*)&job.peer, &peer_len,
			SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (job.fd < 0)
		{
			switch (errno)
			{
			case EAGAIN:
#if EAGAIN != EWOULDBLOCK
			case EWOULDBLOCK:
#endif
				return;
			case EINTR:
			case ECONNABORTED:
			case EPROTO:
				continue;
			case EMFILE:
			case ENFILE: {
				if (ctx->reserve_fd < 0)
				{
					LOG_ERRNO("accept4");
					return;
				}
				close_fd(&ctx->reserve_fd);
				int victim = accept(ctx->listen_fd, NULL, NULL);
				if (victim >= 0)
					close(victim);
				ctx->reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
				atomic_fetch_add(&g_my_stats->rejected, 1);
				log_msg("connection rejected: file descriptor limit exhausted");
				continue;
			}
			default:
				LOG_ERRNO("accept4");
				return;
			}
		}

		job.id = atomic_fetch_add(&g_shared->next_conn_id, 1) + 1;
		atomic_fetch_add(&g_my_stats->accepted, 1);
		atomic_fetch_add(&g_my_stats->active, 1);

		char peer[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &job.peer.sin_addr, peer, sizeof peer);
		LOG_DEBUG("#%" PRIu64 " accept: new connection from %s:%u (fd=%d)",
			job.id, peer, ntohs(job.peer.sin_port), job.fd);

		if (!pool_submit(pool, &job))
		{
			close(job.fd);
			atomic_fetch_sub(&g_my_stats->active, 1);
			atomic_fetch_add(&g_my_stats->rejected, 1);
			log_msg("#%" PRIu64 " connection rejected: pool queue full", job.id);
		}
	}
}

static int worker_init(struct worker_ctx* ctx)
{
	if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
	{
		LOG_ERRNO("prctl(PR_SET_PDEATHSIG)");
		return -1;
	}
	if (getppid() != ctx->master_pid)
	{
		log_msg("master terminated before worker started");
		return -1;
	}

	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	ctx->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	ctx->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	ctx->reserve_fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (ctx->signal_fd < 0 || ctx->epoll_fd < 0 || ctx->reserve_fd < 0)
	{
		LOG_ERRNO("creating auxiliary descriptors");
		return -1;
	}

	if (epoll_watch(ctx->epoll_fd, ctx->listen_fd, EPOLLIN | EPOLLEXCLUSIVE) < 0 || epoll_watch(ctx->epoll_fd, ctx->signal_fd, EPOLLIN) < 0)
		return -1;
	return 0;
}

static int worker_process_run(const struct config* cfg, unsigned slot, int listen_fd, pid_t master_pid)
{
	snprintf(g_tag, sizeof g_tag, "p%u", slot);
	g_my_stats = &g_shared->procs[slot - 1];

	struct worker_ctx ctx = {
		.cfg = cfg,
		.slot = slot,
		.master_pid = master_pid,
		.listen_fd = listen_fd,
		.epoll_fd = -1,
		.signal_fd = -1,
		.reserve_fd = -1,
	};
	int status = EXIT_FAILURE;
	if (worker_init(&ctx) < 0)
		goto out;

	struct thread_pool pool;
	if (pool_init(&pool, cfg->threads, QUEUE_CAPACITY, &ctx) < 0)
		goto out;
	log_msg("worker process ready (pid=%d, threads=%u)", (int)getpid(), cfg->threads);

	struct epoll_event events[MAX_EVENTS];
	bool running = true;
	while (running)
	{
		int n = epoll_wait(ctx.epoll_fd, events, MAX_EVENTS, -1);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_ERRNO("epoll_wait");
			break;
		}
		for (int i = 0; i < n; i++)
		{
			if (events[i].data.fd == ctx.signal_fd)
			{
				struct signalfd_siginfo si;
				if (read(ctx.signal_fd, &si, sizeof si) == (ssize_t)sizeof si)
				{
					log_msg("received SIG%s — stopping accept loop",
						sigabbrev_np((int)si.ssi_signo));
					running = false;
				}
			}
			else if (running)
			{
				accept_clients(&ctx, &pool);
			}
		}
	}

	epoll_ctl(ctx.epoll_fd, EPOLL_CTL_DEL, ctx.listen_fd, NULL);
	close_fd(&ctx.listen_fd);
	log_msg(
		"close: listening socket closed, draining active connections: %" PRIu64,
		(uint64_t)atomic_load(&g_my_stats->active));
	pool_destroy(&pool);
	log_msg("all pool threads finished, worker process exiting");
	status = EXIT_SUCCESS;

out:
	close_fd(&ctx.listen_fd);
	close_fd(&ctx.signal_fd);
	close_fd(&ctx.epoll_fd);
	close_fd(&ctx.reserve_fd);
	return status;
}

struct child
{
	pid_t pid;
	int64_t started_ns;
};

struct master_ctx
{
	struct config cfg;
	pid_t pid;
	int listen_fd;
	int epoll_fd;
	int signal_fd;
	int timer_fd;
	struct child children[MAX_PROCESSES];
	unsigned alive;
	bool stopping;
	struct stats_snapshot last_stats;
};

static int listener_open(uint16_t port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
	if (fd < 0)
	{
		LOG_ERRNO("socket");
		return -1;
	}
	log_msg("socket: listening socket created (fd=%d)", fd);

	int one = 1;
	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one) < 0)
	{
		LOG_ERRNO("setsockopt(SO_REUSEADDR)");
		goto fail;
	}

	struct sockaddr_in addr = {
		.sin_family = AF_INET,
		.sin_port = htons(port),
		.sin_addr.s_addr = htonl(INADDR_ANY),
	};
	if (bind(fd, (struct sockaddr*)&addr, sizeof addr) < 0)
	{
		LOG_ERRNO("bind");
		goto fail;
	}
	log_msg("bind: socket bound to 0.0.0.0:%u", port);

	if (listen(fd, SOMAXCONN) < 0)
	{
		LOG_ERRNO("listen");
		goto fail;
	}
	log_msg("listen: waiting for connections (backlog=%d)", SOMAXCONN);
	return fd;

fail:
	close(fd);
	return -1;
}

static void raise_fd_limit(void)
{
	struct rlimit rl;
	if (getrlimit(RLIMIT_NOFILE, &rl) == 0 && rl.rlim_cur < rl.rlim_max)
	{
		rl.rlim_cur = rl.rlim_max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
}

static int master_init(struct master_ctx* m)
{
	sigset_t mask;
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGUSR1);
	sigaddset(&mask, SIGCHLD);
	if (sigprocmask(SIG_BLOCK, &mask, NULL) < 0)
	{
		LOG_ERRNO("sigprocmask");
		return -1;
	}
	struct sigaction ign = { .sa_handler = SIG_IGN };
	sigaction(SIGPIPE, &ign, NULL);

	raise_fd_limit();

	void* shm = mmap(NULL, sizeof *g_shared, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
	if (shm == MAP_FAILED)
	{
		LOG_ERRNO("mmap");
		return -1;
	}
	g_shared = shm;

	m->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
	m->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
	m->epoll_fd = epoll_create1(EPOLL_CLOEXEC);
	if (m->signal_fd < 0 || m->timer_fd < 0 || m->epoll_fd < 0)
	{
		LOG_ERRNO("creating auxiliary descriptors");
		return -1;
	}

	const time_t interval = m->cfg.stats_interval_sec;
	struct itimerspec its = {
		.it_value = { .tv_sec = interval },
		.it_interval = { .tv_sec = interval },
	};
	if (interval > 0 && timerfd_settime(m->timer_fd, 0, &its, NULL) < 0)
	{
		LOG_ERRNO("timerfd_settime");
		return -1;
	}

	m->listen_fd = listener_open(m->cfg.port);
	if (m->listen_fd < 0)
		return -1;

	if (epoll_watch(m->epoll_fd, m->signal_fd, EPOLLIN) < 0 || epoll_watch(m->epoll_fd, m->timer_fd, EPOLLIN) < 0)
		return -1;
	return 0;
}

static void master_close(struct master_ctx* m)
{
	close_fd(&m->listen_fd);
	close_fd(&m->timer_fd);
	close_fd(&m->signal_fd);
	close_fd(&m->epoll_fd);
	if (g_shared)
	{
		munmap(g_shared, sizeof *g_shared);
		g_shared = NULL;
	}
}

static int spawn_worker(struct master_ctx* m, unsigned slot)
{
	pid_t pid = fork();
	if (pid < 0)
	{
		LOG_ERRNO("fork");
		return -1;
	}
	if (pid == 0)
	{
		close_fd(&m->epoll_fd);
		close_fd(&m->signal_fd);
		close_fd(&m->timer_fd);
		int code = worker_process_run(&m->cfg, slot, m->listen_fd, m->pid);
		munmap(g_shared, sizeof *g_shared);
		exit(code);
	}

	m->children[slot - 1] = (struct child){ .pid = pid, .started_ns = now_ns() };
	m->alive++;
	log_msg("fork: worker process p%u started (pid=%d)", slot, (int)pid);
	return 0;
}

static void begin_shutdown(struct master_ctx* m)
{
	if (m->stopping)
		return;
	m->stopping = true;

	close_fd(&m->listen_fd);
	log_msg("graceful shutdown started: SIGTERM to all worker processes (%u)", m->alive);
	for (unsigned i = 0; i < m->cfg.processes; i++)
		if (m->children[i].pid > 0)
			kill(m->children[i].pid, SIGTERM);
}

static void reap_children(struct master_ctx* m)
{
	int status;
	pid_t pid;
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
		unsigned slot = 0;
		for (unsigned i = 0; i < m->cfg.processes; i++)
			if (m->children[i].pid == pid)
				slot = i + 1;
		if (slot == 0)
			continue;

		struct child* c = &m->children[slot - 1];
		const int64_t uptime_ms = (now_ns() - c->started_ns) / 1000000;
		c->pid = 0;
		m->alive--;

		if (WIFSIGNALED(status))
			log_msg("worker process p%u (pid=%d) killed by signal SIG%s",
				slot, (int)pid, sigabbrev_np(WTERMSIG(status)));
		else
			log_msg("worker process p%u (pid=%d) exited with code %d",
				slot, (int)pid, WEXITSTATUS(status));

		if (m->stopping)
			continue;

		atomic_store(&g_shared->procs[slot - 1].active, 0);
		if (uptime_ms < MIN_UPTIME_FOR_RESPAWN_MS)
		{
			log_msg("p%u crashed immediately after start — not respawning to avoid loop", slot);
			if (m->alive == 0)
				begin_shutdown(m);
			continue;
		}
		log_msg("respawning worker process p%u", slot);
		spawn_worker(m, slot);
	}
}

static void on_signals(struct master_ctx* m)
{
	struct signalfd_siginfo si[8];
	ssize_t n;
	while ((n = read(m->signal_fd, si, sizeof si)) > 0)
	{
		for (size_t i = 0; i < (size_t)n / sizeof *si; i++)
		{
			switch (si[i].ssi_signo)
			{
			case SIGINT:
			case SIGTERM:
				if (!m->stopping)
					log_msg("received signal SIG%s", sigabbrev_np((int)si[i].ssi_signo));
				begin_shutdown(m);
				break;
			case SIGUSR1:
				if (!m->stopping)
					log_msg("shutdown command received from client (via process pid=%u)",
						si[i].ssi_pid);
				begin_shutdown(m);
				break;
			case SIGCHLD:
				reap_children(m);
				break;
			}
		}
	}
}

static void on_stats_timer(struct master_ctx* m)
{
	uint64_t expirations;
	if (read(m->timer_fd, &expirations, sizeof expirations) < 0)
		return;

	struct stats_snapshot s = stats_total(m->cfg.processes);
	if (memcmp(&s, &m->last_stats, sizeof s) != 0 || s.active > 0)
		stats_print("STATISTICS", &s);
	m->last_stats = s;
}

static int master_run(struct master_ctx* m)
{
	for (unsigned slot = 1; slot <= m->cfg.processes; slot++)
		if (spawn_worker(m, slot) < 0)
		{
			begin_shutdown(m);
			break;
		}
	if (!m->stopping)
		log_msg("started worker processes: %u × %u threads (Ctrl+C to stop)",
			m->cfg.processes, m->cfg.threads);

	struct epoll_event events[4];
	while (m->alive > 0)
	{
		int n = epoll_wait(m->epoll_fd, events, 4, -1);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_ERRNO("epoll_wait");
			return -1;
		}
		for (int i = 0; i < n; i++)
		{
			if (events[i].data.fd == m->signal_fd)
				on_signals(m);
			else if (events[i].data.fd == m->timer_fd)
				on_stats_timer(m);
		}
	}
	log_msg("all worker processes terminated");

	for (unsigned i = 0; i < m->cfg.processes; i++)
	{
		struct stats_snapshot s = stats_read(&g_shared->procs[i]);
		char title[32];
		snprintf(title, sizeof title, "  p%u", i + 1);
		stats_print(title, &s);
	}
	struct stats_snapshot total = stats_total(m->cfg.processes);
	stats_print("TOTAL STATISTICS", &total);
	return 0;
}

static void usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [-p port] [-P processes] [-w threads] [-t timeout_s] [-i interval_s] [-q]\n"
		"  -p  TCP port, > 1023 (default %d)\n"
		"  -P  number of worker processes, 1..%d (default %d)\n"
		"  -w  threads per process pool (default %d)\n"
		"  -t  client timeout in seconds (default %d)\n"
		"  -i  statistics interval in seconds, 0 = off (default %d)\n"
		"  -q  quiet mode: suppress per-connection events\n",
		prog, DEFAULT_PORT, MAX_PROCESSES, DEFAULT_PROCESSES, DEFAULT_THREADS,
		DEFAULT_TIMEOUT_SEC, DEFAULT_STATS_SEC);
}

static bool parse_ulong(
	const char* s,
	const unsigned long min,
	const unsigned long max,
	unsigned long* out)
{
	char* end;
	errno = 0;
	const unsigned long v = strtoul(s, &end, 10);
	if (errno != 0 || end == s || *end != '\0' || *s == '-' || v < min || v > max)
		return false;
	*out = v;
	return true;
}

static int parse_args(int argc, char** argv, struct config* cfg)
{
	*cfg = (struct config){
		.port = DEFAULT_PORT,
		.processes = DEFAULT_PROCESSES,
		.threads = DEFAULT_THREADS,
		.timeout_ms = DEFAULT_TIMEOUT_SEC * 1000,
		.stats_interval_sec = DEFAULT_STATS_SEC,
	};

	int opt;
	unsigned long v;
	while ((opt = getopt(argc, argv, "p:P:w:t:i:qh")) != -1)
	{
		switch (opt)
		{
		case 'p':
			if (!parse_ulong(optarg, 1024, 65535, &v))
				goto bad;
			cfg->port = (uint16_t)v;
			break;
		case 'P':
			if (!parse_ulong(optarg, 1, MAX_PROCESSES, &v))
				goto bad;
			cfg->processes = (unsigned)v;
			break;
		case 'w':
			if (!parse_ulong(optarg, 1, 1024, &v))
				goto bad;
			cfg->threads = (unsigned)v;
			break;
		case 't':
			if (!parse_ulong(optarg, 1, 3600, &v))
				goto bad;
			cfg->timeout_ms = (int)v * 1000;
			break;
		case 'i':
			if (!parse_ulong(optarg, 0, 3600, &v))
				goto bad;
			cfg->stats_interval_sec = (unsigned)v;
			break;
		case 'q':
			g_verbose = false;
			break;
		default:
			goto bad;
		}
	}
	if (optind != argc)
		goto bad;
	return 0;

bad:
	usage(argv[0]);
	return -1;
}

int main(const int argc, char** argv)
{
	struct master_ctx m = {
		.pid = getpid(),
		.listen_fd = -1,
		.epoll_fd = -1,
		.signal_fd = -1,
		.timer_fd = -1,
	};
	if (parse_args(argc, argv, &m.cfg) < 0)
		return EXIT_FAILURE;

	log_msg("starting: %s (pid=%d, port=%u, processes=%u, threads each=%u, timeout=%d s)",
		SERVER_NAME, (int)m.pid, m.cfg.port, m.cfg.processes, m.cfg.threads,
		m.cfg.timeout_ms / 1000);

	int status = EXIT_FAILURE;
	if (master_init(&m) == 0 && master_run(&m) == 0)
		status = EXIT_SUCCESS;

	master_close(&m);
	log_msg("all sockets closed, server stopped");
	return status;
}
