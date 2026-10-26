#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <netinet/in.h>
#include <poll.h>
#include <string>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace fs = std::filesystem;
namespace
{
constexpr int kDefaultPort = 8080;
constexpr int kMaxRequestBytes = 16 * 1024;
constexpr int kBacklog = 128;
constexpr int kIoTimeoutMs = 5000;

std::atomic g_stopping{ false };
std::mutex g_log_mutex;

void Log(const std::string& message)
{
	std::lock_guard lock(g_log_mutex);
	std::cout << message << std::endl;
}

void SignalHandler(int)
{
	g_stopping.store(true, std::memory_order_relaxed);
}

std::string ReasonPhrase(const int status)
{
	switch (status)
	{
	case 200:
		return "OK";
	case 400:
		return "Bad Request";
	case 403:
		return "Forbidden";
	case 404:
		return "Not Found";
	case 405:
		return "Method Not Allowed";
	case 413:
		return "Payload Too Large";
	case 500:
		return "Internal Server Error";
	case 503:
		return "Service Unavailable";
	default:
		return "Error";
	}
}

std::string MimeType(const fs::path& path)
{
	static const std::map<std::string, std::string> types{
		{ ".html", "text/html; charset=utf-8" },
		{ ".htm", "text/html; charset=utf-8" },
		{ ".css", "text/css; charset=utf-8" },
		{ ".jpg", "image/jpeg" },
		{ ".jpeg", "image/jpeg" },
		{ ".png", "image/png" },
		{ ".txt", "text/plain; charset=utf-8" },
		{ ".js", "text/javascript; charset=utf-8" },
		{ ".svg", "image/svg+xml" },
		{ ".ico", "image/x-icon" }
	};
	auto ext = path.extension().string();
	std::ranges::transform(ext, ext.begin(), [](unsigned char c) {
		return static_cast<char>(std::tolower(c));
	});
	const auto it = types.find(ext);
	return it == types.end()
		? "application/octet-stream"
		: it->second;
}

bool SendAll(const int fd, const char* data, std::size_t size)
{
	std::size_t sent = 0;
	while (sent < size)
	{
		const ssize_t n = ::send(fd, data + sent, size - sent, MSG_NOSIGNAL);
		if (n > 0)
		{
			sent += static_cast<std::size_t>(n);
			continue;
		}
		if (n < 0 && errno == EINTR)
		{
			continue;
		}
		return false;
	}
	return true;
}

bool SendResponse(
	int fd,
	int status,
	const std::string& type,
	const std::string& body)
{
	const auto header = "HTTP/1.1 "
		+ std::to_string(status)
		+ " " + ReasonPhrase(status)
		+ "\r\n" + "Content-Type: "
		+ type + "\r\n"
		+ "Content-Length: " + std::to_string(body.size())
		+ "\r\n" + "Connection: close\r\nX-Content-Type-Options: nosniff\r\n\r\n";
	return SendAll(fd, header.data(), header.size()) && SendAll(fd, body.data(), body.size());
}

bool SendFileResponse(const int fd, const fs::path& path, std::uintmax_t size)
{
	const std::string header = "HTTP/1.1 200 OK\r\nContent-Type: " + MimeType(path) + "\r\n" + "Content-Length: " + std::to_string(size) + "\r\n"
																																		   "Connection: close\r\n"
																																		   "X-Content-Type-Options: nosniff\r\n\r\n";
	if (!SendAll(fd, header.data(), header.size()))
		return false;

	std::ifstream file(path, std::ios::binary);
	if (!file)
		return false;
	char buffer[16 * 1024];
	while (file)
	{
		file.read(buffer, sizeof(buffer));
		const auto count = file.gcount();
		if (count > 0 && !SendAll(fd, buffer, static_cast<std::size_t>(count)))
			return false;
	}
	return !file.bad();
}

bool ReadRequest(int fd, std::string& request)
{
	request.clear();
	char buffer[2048];
	while (request.find("\r\n\r\n") == std::string::npos)
	{
		pollfd pfd{ fd, POLLIN, 0 };
		int ready;
		do
		{
			ready = ::poll(&pfd, 1, kIoTimeoutMs);
		} while (ready < 0 && errno == EINTR);
		if (ready <= 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)))
			return false;

		const ssize_t n = ::recv(fd, buffer, sizeof(buffer), 0);
		if (n == 0)
			return false;
		if (n < 0)
		{
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			return false;
		}
		request.append(buffer, static_cast<std::size_t>(n));
		if (request.size() > kMaxRequestBytes)
			return false;
	}
	return true;
}

std::string TrimQueryAndDecodePath(std::string target)
{
	const auto query = target.find('?');
	if (query != std::string::npos)
		target.resize(query);

	std::string decoded;
	decoded.reserve(target.size());
	auto hex = [](char c) -> int {
		if (c >= '0' && c <= '9')
			return c - '0';
		if (c >= 'a' && c <= 'f')
			return c - 'a' + 10;
		if (c >= 'A' && c <= 'F')
			return c - 'A' + 10;
		return -1;
	};
	for (std::size_t i = 0; i < target.size(); ++i)
	{
		if (target[i] == '%')
		{
			if (i + 2 >= target.size())
				return {};
			const int hi = hex(target[i + 1]), lo = hex(target[i + 2]);
			if (hi < 0 || lo < 0)
				return {};
			const char c = static_cast<char>((hi << 4) | lo);
			if (c == '\0')
				return {};
			decoded.push_back(c);
			i += 2;
		}
		else
		{
			decoded.push_back(target[i]);
		}
	}
	if (decoded.empty() || decoded.front() != '/' || decoded.find('\\') != std::string::npos)
		return {};
	return decoded;
}

void HandleClient(int client_fd, const fs::path& web_root, const std::string& peer)
{
	struct FdGuard
	{
		int fd;
		~FdGuard()
		{
			if (fd >= 0)
				::close(fd);
		}
	} guard{ client_fd };

	const timeval timeout{ 5, 0 };
	::setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	::setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

	std::string request;
	if (!ReadRequest(client_fd, request))
	{
		Log("[WARN] " + peer + " request unreadable, timed out, or too large");
		SendResponse(client_fd, request.size() > kMaxRequestBytes ? 413 : 400,
			"text/plain; charset=utf-8",
			request.size() > kMaxRequestBytes ? "Request Too Large\n" : "Bad Request\n");
		return;
	}

	const auto line_end = request.find("\r\n");
	if (line_end == std::string::npos)
	{
		SendResponse(client_fd, 400, "text/plain; charset=utf-8", "Bad Request\n");
		return;
	}
	const std::string request_line = request.substr(0, line_end);
	const auto first_space = request_line.find(' ');
	const auto second_space = first_space == std::string::npos
		? std::string::npos
		: request_line.find(' ', first_space + 1);
	if (first_space == std::string::npos || second_space == std::string::npos || request_line.substr(second_space + 1).rfind("HTTP/1.", 0) != 0)
	{
		SendResponse(client_fd, 400, "text/plain; charset=utf-8", "Bad Request\n");
		return;
	}
	const std::string method = request_line.substr(0, first_space);
	const std::string raw_target = request_line.substr(first_space + 1,
		second_space - first_space - 1);
	if (method != "GET")
	{
		SendResponse(client_fd, 405, "text/plain; charset=utf-8",
			"Method Not Allowed\n");
		Log("[HTTP] " + peer + " " + method + " " + raw_target + " -> 405");
		return;
	}

	const std::string target = TrimQueryAndDecodePath(raw_target);
	if (target.empty())
	{
		SendResponse(client_fd, 400, "text/plain; charset=utf-8", "Bad Request\n");
		return;
	}

	std::string relative = target.substr(1);
	if (relative.empty() || target.back() == '/')
		relative += "index.html";
	std::error_code ec;
	const fs::path root = fs::canonical(web_root, ec);
	if (ec)
	{
		SendResponse(client_fd, 500, "text/plain; charset=utf-8", "Server Error\n");
		return;
	}
	const fs::path candidate = fs::canonical(root / fs::path(relative), ec);
	if (ec)
	{
		SendResponse(client_fd, 404, "text/plain; charset=utf-8", "File Not Found\n");
		Log("[HTTP] " + peer + " GET " + target + " -> 404");
		return;
	}
	auto root_it = root.begin(), file_it = candidate.begin();
	for (; root_it != root.end() && file_it != candidate.end(); ++root_it, ++file_it)
	{
		if (*root_it != *file_it)
			break;
	}
	if (root_it != root.end())
	{
		SendResponse(client_fd, 403, "text/plain; charset=utf-8", "Forbidden\n");
		Log("[HTTP] " + peer + " GET " + target + " -> 403");
		return;
	}
	if (!fs::is_regular_file(candidate, ec) || ec)
	{
		SendResponse(client_fd, 404, "text/plain; charset=utf-8", "File Not Found\n");
		Log("[HTTP] " + peer + " GET " + target + " -> 404");
		return;
	}
	const auto size = fs::file_size(candidate, ec);
	if (ec)
	{
		SendResponse(client_fd, 500, "text/plain; charset=utf-8", "Server Error\n");
		return;
	}

	const bool ok = SendFileResponse(client_fd, candidate, size);
	Log("[HTTP] " + peer + " GET " + target + " -> " + (ok ? "200" : "connection/write error"));
}

std::string PeerName(const sockaddr_in& address)
{
	char ip[INET_ADDRSTRLEN]{};
	::inet_ntop(AF_INET, &address.sin_addr, ip, sizeof(ip));
	return std::string(ip) + ":" + std::to_string(ntohs(address.sin_port));
}
} // namespace

int main(int argc, char** argv)
{
	int port = kDefaultPort;
	fs::path webRoot = "www";
	try
	{
		if (argc > 1)
			port = std::stoi(argv[1]);
		if (argc > 2)
			webRoot = argv[2];
	}
	catch (...)
	{
		std::cerr << "Usage: " << argv[0] << " [port>1023] [web-root]\n";
		return 2;
	}
	if (port <= 1023 || port > 65535)
	{
		std::cerr << "Port must be in range 1024..65535\n";
		return 2;
	}
	std::error_code ec;
	webRoot = fs::canonical(webRoot, ec);
	if (ec || !fs::is_directory(webRoot))
	{
		std::cerr << "Web root must be an existing directory\n";
		return 2;
	}

	struct sigaction sa{};
	sa.sa_handler = SignalHandler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	::sigaction(SIGINT, &sa, nullptr);
	::signal(SIGPIPE, SIG_IGN);

	const int listen_fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
	if (listen_fd < 0)
	{
		std::cerr << "socket: " << std::strerror(errno) << '\n';
		return 1;
	}
	int yes = 1;
	::setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_ANY);
	address.sin_port = htons(static_cast<uint16_t>(port));
	if (::bind(listen_fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0)
	{
		std::cerr << "bind: " << std::strerror(errno) << '\n';
		::close(listen_fd);
		return 1;
	}
	if (::listen(listen_fd, kBacklog) < 0)
	{
		std::cerr << "listen: " << std::strerror(errno) << '\n';
		::close(listen_fd);
		return 1;
	}

	const int epoll_fd = ::epoll_create1(EPOLL_CLOEXEC);
	const int wake_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
	if (epoll_fd < 0 || wake_fd < 0)
	{
		std::cerr << "epoll/eventfd: " << std::strerror(errno) << '\n';
		if (epoll_fd >= 0)
			::close(epoll_fd);
		if (wake_fd >= 0)
			::close(wake_fd);
		::close(listen_fd);
		return 1;
	}
	epoll_event listener_event{};
	listener_event.events = EPOLLIN;
	listener_event.data.fd = listen_fd;
	epoll_event wake_event{};
	wake_event.events = EPOLLIN;
	wake_event.data.fd = wake_fd;
	if (::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &listener_event) < 0 || ::epoll_ctl(epoll_fd, EPOLL_CTL_ADD, wake_fd, &wake_event) < 0)
	{
		std::cerr << "epoll_ctl: " << std::strerror(errno) << '\n';
		::close(wake_fd);
		::close(epoll_fd);
		::close(listen_fd);
		return 1;
	}

	std::mutex workers_mutex;
	std::vector<std::jthread> workers;
	std::jthread shutdown_watcher([&](const std::stop_token& stop) {
		while (!stop.stop_requested() && !g_stopping.load(std::memory_order_relaxed))
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
		}
		const uint64_t one = 1;
		(void)::write(wake_fd, &one, sizeof(one));
	});

	Log("[START] C++20 epoll web server listening on http://0.0.0.0:" + std::to_string(port) + " (root: " + webRoot.string() + ")");
	Log("[INFO] Ctrl+C for graceful shutdown");

	epoll_event events[32]{};
	while (!g_stopping.load(std::memory_order_relaxed))
	{
		const int count = ::epoll_wait(epoll_fd, events, 32, -1);
		if (count < 0)
		{
			if (errno == EINTR)
				continue;
			Log(std::string("[ERROR] epoll_wait: ") + std::strerror(errno));
			break;
		}
		for (int i = 0; i < count; ++i)
		{
			if (events[i].data.fd == wake_fd)
			{
				uint64_t value;
				(void)::read(wake_fd, &value, sizeof(value));
				g_stopping.store(true, std::memory_order_relaxed);
				break;
			}
			if (events[i].data.fd != listen_fd)
				continue;
			for (;;)
			{
				sockaddr_in client_address{};
				socklen_t client_length = sizeof(client_address);
				const int client_fd = ::accept4(listen_fd,
					reinterpret_cast<sockaddr*>(&client_address), &client_length,
					SOCK_CLOEXEC);
				if (client_fd < 0)
				{
					if (errno == EINTR)
						continue;
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						break;
					Log(std::string("[WARN] accept: ") + std::strerror(errno));
					break;
				}
				const std::string peer = PeerName(client_address);
				std::lock_guard lock(workers_mutex);
				workers.emplace_back([client_fd, webRoot, peer](std::stop_token) {
					HandleClient(client_fd, webRoot, peer);
				});
				Log("[ACCEPT] " + peer);
			}
		}
	}

	Log("[STOP] stopping accept loop; waiting for active connections...");
	::close(listen_fd);
	shutdown_watcher.request_stop();
	if (shutdown_watcher.joinable())
		shutdown_watcher.join();
	{
		std::lock_guard lock(workers_mutex);
		workers.clear();
	}
	::close(wake_fd);
	::close(epoll_fd);
	Log("[STOP] server stopped cleanly");
	return 0;
}
