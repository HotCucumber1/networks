#define _GNU_SOURCE

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define DEFAULT_CLIENT_NAME "Dmitriy Rybakov"
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "5555"

enum
{
	MAX_MESSAGE = 256,
	MAX_NAME = 128,
	NUMBER_MIN = 1,
	NUMBER_MAX = 100,
	IO_TIMEOUT_SEC = 15,
};

__attribute__((format(printf, 1, 2))) static void log_msg(const char* fmt, ...)
{
	struct timespec ts;
	struct tm tm;
	clock_gettime(CLOCK_REALTIME, &ts);
	localtime_r(&ts.tv_sec, &tm);
	printf("%02d:%02d:%02d.%03ld [client] ", tm.tm_hour, tm.tm_min, tm.tm_sec, ts.tv_nsec / 1000000);

	va_list ap;
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

#define LOG_ERRNO(what) log_msg("ERROR: %s: %s", (what), strerror(errno))

static bool parse_long(const char* s, long* out)
{
	char* end;
	errno = 0;
	long v = strtol(s, &end, 10);
	while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')
		end++;
	if (errno != 0 || end == s || *end != '\0' || v < INT_MIN || v > INT_MAX)
		return false;
	*out = v;
	return true;
}

static int read_number(long* out)
{
	char line[64];

	for (;;)
	{
		printf("Enter an integer from %d to %d: ", NUMBER_MIN, NUMBER_MAX);
		fflush(stdout);
		if (!fgets(line, sizeof line, stdin))
		{
			log_msg("ERROR: stdin closed, no number entered");
			return -1;
		}
		if (!isatty(STDIN_FILENO))
			fputs(line, stdout);
		if (parse_long(line, out))
			break;
		log_msg("not a valid integer, try again");
	}

	if (*out < NUMBER_MIN || *out > NUMBER_MAX)
		log_msg("WARNING: number %ld is out of range [%d..%d] — server will treat it as a shutdown command",
			*out, NUMBER_MIN, NUMBER_MAX);
	return 0;
}

static int connect_to_server(const char* host, const char* port)
{
	const struct addrinfo hints = {
		.ai_family = AF_UNSPEC,
		.ai_socktype = SOCK_STREAM
	};
	struct addrinfo* list;
	const int rc = getaddrinfo(host, port, &hints, &list);
	if (rc != 0)
	{
		log_msg("ERROR: getaddrinfo(%s): %s", host, gai_strerror(rc));
		return -1;
	}

	int fd = -1;
	for (const struct addrinfo* ai = list; ai; ai = ai->ai_next)
	{
		char addr[INET6_ADDRSTRLEN] = "?";
		const void* src = ai->ai_family == AF_INET
			? (const void*)&((struct sockaddr_in*)ai->ai_addr)->sin_addr
			: (const void*)&((struct sockaddr_in6*)ai->ai_addr)->sin6_addr;
		inet_ntop(ai->ai_family, src, addr, sizeof addr);

		fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
		if (fd < 0)
		{
			LOG_ERRNO("socket");
			continue;
		}
		log_msg("socket: TCP socket created (fd=%d)", fd);

		struct timeval tv = { .tv_sec = IO_TIMEOUT_SEC };
		setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);

		log_msg("connect: connecting to %s:%s ...", addr, port);
		if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0)
		{
			log_msg("connect: connection established");
			break;
		}
		LOG_ERRNO("connect");
		close(fd);
		log_msg("close: socket closed (fd=%d)", fd);
		fd = -1;
	}

	freeaddrinfo(list);
	return fd;
}

static int send_all(const int fd, const char* buf, size_t len)
{
	while (len > 0)
	{
		const ssize_t n = send(fd, buf, len, MSG_NOSIGNAL);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			LOG_ERRNO("send");
			return -1;
		}
		buf += n;
		len -= (size_t)n;
	}
	return 0;
}

static ssize_t recv_line(const int fd, char* buf, size_t cap)
{
	size_t len = 0;

	while (len < cap - 1)
	{
		ssize_t n = recv(fd, buf + len, cap - 1 - len, 0);
		if (n < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				log_msg("ERROR: recv: server did not respond within %d s", IO_TIMEOUT_SEC);
			else
				LOG_ERRNO("recv");
			return -1;
		}
		if (n == 0)
		{
			log_msg("ERROR: recv: server closed connection without sending a reply");
			return -1;
		}
		char* nl = memchr(buf + len, '\n', (size_t)n);
		len += (size_t)n;
		if (nl)
		{
			*nl = '\0';
			return nl - buf;
		}
	}
	log_msg("ERROR: server reply exceeds %zu bytes", cap - 1);
	return -1;
}

static bool parse_reply(char* line, char* name, size_t name_cap, long* number)
{
	size_t len = strlen(line);
	if (len > 0 && line[len - 1] == '\r')
		line[--len] = '\0';

	char* sep = strrchr(line, '|');
	if (!sep || sep == line || (size_t)(sep - line) >= name_cap)
		return false;
	*sep = '\0';
	if (!parse_long(sep + 1, number))
		return false;
	memcpy(name, line, (size_t)(sep - line) + 1);
	return true;
}

static void usage(const char* prog)
{
	fprintf(stderr,
		"Usage: %s [-s host] [-p port] [-n client_name]\n"
		"  defaults: -s %s -p %s -n \"%s\"\n",
		prog, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_CLIENT_NAME);
}

int main(const int argc, char** argv)
{
	const char* host = DEFAULT_HOST;
	const char* port = DEFAULT_PORT;
	const char* client_name = DEFAULT_CLIENT_NAME;

	int opt;
	while ((opt = getopt(argc, argv, "s:p:n:h")) != -1)
	{
		switch (opt)
		{
		case 's':
			host = optarg;
			break;
		case 'p':
			port = optarg;
			break;
		case 'n':
			client_name = optarg;
			break;
		default:
			usage(argv[0]);
			return EXIT_FAILURE;
		}
	}
	if (optind != argc || strlen(client_name) >= MAX_NAME || strchr(client_name, '|'))
	{
		usage(argv[0]);
		return EXIT_FAILURE;
	}

	log_msg("starting: %s", client_name);

	long my_number;
	if (read_number(&my_number) < 0)
		return EXIT_FAILURE;

	const int fd = connect_to_server(host, port);
	if (fd < 0)
	{
		log_msg("could not connect to %s:%s", host, port);
		return EXIT_FAILURE;
	}

	int status = EXIT_FAILURE;
	char request[MAX_MESSAGE];
	const int request_len = snprintf(request, sizeof request, "%s|%ld\n", client_name, my_number);
	if (send_all(fd, request, (size_t)request_len) < 0)
		goto out;
	log_msg("send: %d bytes sent: \"%s|%ld\"", request_len, client_name, my_number);

	char reply[MAX_MESSAGE];
	const ssize_t reply_len = recv_line(fd, reply, sizeof reply);
	if (reply_len < 0)
		goto out;
	log_msg("recv: %zd bytes received: \"%s\"", reply_len + 1, reply);

	char server_name[MAX_NAME];
	long server_number;
	if (!parse_reply(reply, server_name, sizeof server_name, &server_number))
	{
		log_msg("ERROR: invalid server reply format");
		goto out;
	}

	printf("\n");
	printf("  Client name   : %s\n", client_name);
	printf("  Server name   : %s\n", server_name);
	printf("  Client number : %ld\n", my_number);
	printf("  Server number : %ld\n", server_number);
	printf("  Sum           : %lld\n", (long long)my_number + server_number);
	printf("\n");
	status = EXIT_SUCCESS;

out:
	close(fd);
	log_msg("close: socket closed, exiting");
	return status;
}
