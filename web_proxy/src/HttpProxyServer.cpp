#include "../include/HttpProxyServer.h"
#include "../include/HttpMessage.h"
#include "../include/HttpParser.h"
#include "../include/Logger.h"
#include "../include/SocketUtils.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <sys/socket.h>
#include <unistd.h>

namespace
{
constexpr size_t MaxHeaderSize = 128 * 1024;
constexpr size_t MaxRequestBody = 32 * 1024 * 1024;
constexpr size_t MaxResponseSize = 128 * 1024 * 1024;
constexpr int IoTimeoutSeconds = 15;

class SocketGuard
{
public:
	explicit SocketGuard(int fd)
		: m_fd(fd)
	{
	}
	~SocketGuard()
	{
		if (m_fd >= 0)
		{
			close(m_fd);
		}
	}
	SocketGuard(const SocketGuard&) = delete;
	SocketGuard& operator=(const SocketGuard&) = delete;

private:
	int m_fd;
};
} // namespace

HttpProxyServer::HttpProxyServer(int port, size_t workerCount, const std::string& cacheDirectory)
	: m_port(port)
	, m_cache(cacheDirectory)
	, m_pool(workerCount, 1024)
{
}

HttpProxyServer::~HttpProxyServer()
{
	if (m_serverFd >= 0)
	{
		close(m_serverFd);
		m_serverFd = -1;
	}
}

void HttpProxyServer::SendError(int clientFd, int status, const std::string& reason, const std::string& details)
{
	const std::string body = details + "\n";
	const std::string response = "HTTP/1.1 " + std::to_string(status) + " " + reason + "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body;
	SocketUtils::SendAll(clientFd, response);
}

int HttpProxyServer::Run()
{
	m_serverFd = SocketUtils::CreateServerSocket(m_port, 256);
	if (m_serverFd < 0)
	{
		Logger::Error("Cannot bind/listen on port " + std::to_string(m_port));
		return 1;
	}
	Logger::Info("HTTP proxy listening on 127.0.0.1:" + std::to_string(m_port));
	while (m_running)
	{
		sockaddr_storage clientAddress{};
		socklen_t addressLength = sizeof(clientAddress);
		const int clientFd = accept(m_serverFd, reinterpret_cast<sockaddr*>(&clientAddress), &addressLength);
		if (clientFd < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
			Logger::Error("accept() failed: " + std::string(std::strerror(errno)));
			continue;
		}
		SocketUtils::SetTimeouts(clientFd, IoTimeoutSeconds);
		if (!m_pool.Submit([this, clientFd] { HandleClient(clientFd); }))
		{
			SendError(clientFd, 503, "Service Unavailable", "Proxy worker queue is full");
			close(clientFd);
		}
	}
	close(m_serverFd);
	m_serverFd = -1;
	return 0;
}

void HttpProxyServer::HandleClient(int clientFd)
{
	SocketGuard clientGuard(clientFd);
	const auto started = std::chrono::steady_clock::now();
	m_statistics.AddRequest();
	if (m_statistics.GetRequestCount() % 10 == 0)
	{
		m_statistics.Print();
	}
	std::string incoming;
	if (!SocketUtils::ReceiveHeaders(clientFd, incoming, MaxHeaderSize))
	{
		SendError(clientFd, 400, "Bad Request", "Could not read complete HTTP headers");
		m_statistics.AddError();
		return;
	}
	const auto headerEnd = incoming.find("\r\n\r\n");
	HttpRequest request;
	std::string error;
	if (!HttpParser::ParseRequest(incoming, request, error))
	{
		SendError(clientFd, error.find("HTTPS") != std::string::npos ? 501 : 400, error.find("HTTPS") != std::string::npos ? "Not Implemented" : "Bad Request", error);
		m_statistics.AddError();
		return;
	}
	auto transferEncoding = request.m_headers.find("transfer-encoding");
	if (transferEncoding != request.m_headers.end() && HttpParser::ToLower(transferEncoding->second) != "identity")
	{
		SendError(clientFd, 501, "Not Implemented", "Chunked request bodies are not supported; use Content-Length");
		m_statistics.AddError();
		return;
	}
	size_t contentLength = 0;
	const auto lengthIt = request.m_headers.find("content-length");
	if (lengthIt != request.m_headers.end())
	{
		try
		{
			contentLength = std::stoull(lengthIt->second);
		}
		catch (...)
		{
			contentLength = MaxRequestBody + 1;
		}
	}
	if (contentLength > MaxRequestBody)
	{
		SendError(clientFd, 413, "Payload Too Large", "Request body exceeds 32 MiB");
		m_statistics.AddError();
		return;
	}
	const auto expectIt = request.m_headers.find("expect");
	if (expectIt != request.m_headers.end() && HttpParser::ToLower(expectIt->second) == "100-continue")
	{
		if (!SocketUtils::SendAll(clientFd, "HTTP/1.1 100 Continue\r\n\r\n"))
		{
			return;
		}
	}
	request.m_body = incoming.substr(headerEnd + 4);
	while (request.m_body.size() < contentLength)
	{
		char buffer[8192];
		const ssize_t count = recv(clientFd, buffer, sizeof(buffer), 0);
		if (count <= 0)
		{
			SendError(clientFd, 400, "Bad Request", "Incomplete request body");
			m_statistics.AddError();
			return;
		}
		request.m_body.append(buffer, static_cast<size_t>(count));
	}
	if (request.m_body.size() > contentLength)
	{
		request.m_body.resize(contentLength);
	}
	const std::string method = HttpParser::ToLower(request.m_method);
	if (method != "get" && method != "post" && method != "head")
	{
		SendError(clientFd, 405, "Method Not Allowed", "Supported methods: GET, HEAD, POST");
		m_statistics.AddError();
		return;
	}
	const std::string cacheKey = request.m_host + ":" + std::to_string(request.m_port) + request.m_path;
	Logger::Info("Accepted " + request.m_method + " " + request.m_host + request.m_path);
	if (method == "get")
	{
		const bool hasCredentials = request.m_headers.count("authorization") || request.m_headers.count("cookie");
		const auto cacheControl = request.m_headers.find("cache-control");
		const auto pragma = request.m_headers.find("pragma");
		const bool forceRevalidation = (cacheControl != request.m_headers.end() && HttpParser::ToLower(cacheControl->second).find("no-cache") != std::string::npos) || (pragma != request.m_headers.end() && HttpParser::ToLower(pragma->second).find("no-cache") != std::string::npos);
		if (hasCredentials)
		{
			Logger::Info("CACHE BYPASS (credentials) " + request.m_host + request.m_path);
			ForwardRequest(clientFd, request, cacheKey, nullptr);
		}
		else
		{
			CacheEntry entry = m_cache.Get(cacheKey);
			if (entry.m_found && entry.m_fresh && !forceRevalidation)
			{
				Logger::Info("CACHE HIT " + request.m_host + request.m_path);
				m_statistics.AddHit();
				SocketUtils::SendAll(clientFd, entry.m_response);
				m_statistics.AddBytes(entry.m_response.size());
				return;
			}
			Logger::Info(entry.m_found ? "CACHE STALE " + request.m_host + request.m_path : "CACHE MISS " + request.m_host + request.m_path);
			m_statistics.AddMiss();
			ForwardRequest(clientFd, request, cacheKey, entry.m_found ? &entry : nullptr);
		}
	}
	else
	{
		ForwardRequest(clientFd, request, cacheKey, nullptr);
	}
	const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count();
	Logger::Info("Completed " + request.m_method + " in " + std::to_string(elapsed) + " ms");
}

bool HttpProxyServer::ForwardRequest(int clientFd, const HttpRequest& request, const std::string& cacheKey, const CacheEntry* staleEntry)
{
	const int originFd = SocketUtils::ConnectToHost(request.m_host, request.m_port, IoTimeoutSeconds);
	if (originFd < 0)
	{
		SendError(clientFd, 502, "Bad Gateway", "Could not connect to target server");
		m_statistics.AddError();
		return false;
	}
	SocketGuard originGuard(originFd);
	const std::string outgoing = HttpParser::BuildOriginRequest(request, staleEntry != nullptr, staleEntry ? staleEntry->m_etag : "", staleEntry ? staleEntry->m_lastModified : "");
	if (!SocketUtils::SendAll(originFd, outgoing))
	{
		SendError(clientFd, 502, "Bad Gateway", "Could not send request to target server");
		m_statistics.AddError();
		return false;
	}
	std::string response;
	if (!SocketUtils::ReceiveToEnd(originFd, response, MaxResponseSize))
	{
		SendError(clientFd, 504, "Gateway Timeout", "Target server timed out or response exceeded 128 MiB");
		m_statistics.AddError();
		return false;
	}
	HttpResponse parsed;
	if (!HttpParser::ParseResponse(response, parsed))
	{
		SendError(clientFd, 502, "Bad Gateway", "Target server returned an invalid HTTP response");
		m_statistics.AddError();
		return false;
	}
	if (parsed.m_statusCode == 304 && staleEntry)
	{
		m_cache.Refresh(cacheKey, response);
		SocketUtils::SendAll(clientFd, staleEntry->m_response);
		m_statistics.AddBytes(staleEntry->m_response.size());
		Logger::Info("CACHE REVALIDATED " + request.m_host + request.m_path);
		return true;
	}
	if (request.m_method == "GET" && parsed.m_statusCode == 200 && !request.m_headers.count("authorization") && !request.m_headers.count("cookie"))
	{
		m_cache.Store(cacheKey, response);
	}
	SocketUtils::SendAll(clientFd, response);
	m_statistics.AddBytes(response.size());
	return true;
}
