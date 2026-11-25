#include "../include/SocketUtils.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

void SocketUtils::SetTimeouts(int socketFd, int timeoutSeconds)
{
	timeval timeout{};
	timeout.tv_sec = timeoutSeconds;
	setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
	setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

int SocketUtils::CreateServerSocket(int port, int backlog)
{
	int serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (serverFd < 0)
	{
		return -1;
	}
	int reuse = 1;
	setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	address.sin_port = htons(static_cast<uint16_t>(port));
	if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0 || listen(serverFd, backlog) < 0)
	{
		close(serverFd);
		return -1;
	}
	return serverFd;
}

int SocketUtils::ConnectToHost(const std::string& host, int port, int timeoutSeconds)
{
	addrinfo hints{};
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	addrinfo* result = nullptr;
	const std::string portText = std::to_string(port);
	if (getaddrinfo(host.c_str(), portText.c_str(), &hints, &result) != 0)
	{
		return -1;
	}
	int connectedFd = -1;
	for (addrinfo* item = result; item; item = item->ai_next)
	{
		int fd = socket(item->ai_family, item->ai_socktype, item->ai_protocol);
		if (fd < 0)
		{
			continue;
		}
		const int flags = fcntl(fd, F_GETFL, 0);
		fcntl(fd, F_SETFL, flags | O_NONBLOCK);
		int status = connect(fd, item->ai_addr, item->ai_addrlen);
		if (status < 0 && errno == EINPROGRESS)
		{
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(fd, &writeSet);
			timeval timeout{};
			timeout.tv_sec = timeoutSeconds;
			status = select(fd + 1, nullptr, &writeSet, nullptr, &timeout);
			if (status > 0)
			{
				int socketError = 0;
				socklen_t length = sizeof(socketError);
				getsockopt(fd, SOL_SOCKET, SO_ERROR, &socketError, &length);
				if (socketError != 0)
				{
					status = -1;
				}
				else
				{
					status = 0;
				}
			}
			else
			{
				status = -1;
			}
		}
		if (status == 0 || (status > 0 && errno != EINPROGRESS))
		{
			fcntl(fd, F_SETFL, flags);
			SetTimeouts(fd, timeoutSeconds);
			connectedFd = fd;
			break;
		}
		close(fd);
	}
	freeaddrinfo(result);
	return connectedFd;
}

bool SocketUtils::SendAll(int socketFd, const std::string& data) { return SendAll(socketFd, data.data(), data.size()); }

bool SocketUtils::SendAll(int socketFd, const char* data, size_t size)
{
	size_t sent = 0;
	while (sent < size)
	{
		const ssize_t count = send(socketFd, data + sent, size - sent, MSG_NOSIGNAL);
		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		if (count == 0)
		{
			return false;
		}
		sent += static_cast<size_t>(count);
	}
	return true;
}

bool SocketUtils::ReceiveHeaders(int socketFd, std::string& data, size_t maxBytes)
{
	char buffer[8192];
	while (data.find("\r\n\r\n") == std::string::npos)
	{
		const ssize_t count = recv(socketFd, buffer, sizeof(buffer), 0);
		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			return false;
		}
		if (count == 0)
		{
			return false;
		}
		data.append(buffer, static_cast<size_t>(count));
		if (data.size() > maxBytes)
		{
			return false;
		}
	}
	return true;
}

bool SocketUtils::ReceiveToEnd(int socketFd, std::string& data, size_t maxBytes)
{
	char buffer[16384];
	while (true)
	{
		const ssize_t count = recv(socketFd, buffer, sizeof(buffer), 0);
		if (count < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			if (errno == EAGAIN || errno == EWOULDBLOCK)
			{
				return false;
			}
			return false;
		}
		if (count == 0)
		{
			return true;
		}
		if (data.size() + static_cast<size_t>(count) > maxBytes)
		{
			return false;
		}
		data.append(buffer, static_cast<size_t>(count));
	}
}
