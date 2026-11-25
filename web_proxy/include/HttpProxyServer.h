#pragma once
#include "CacheManager.h"
#include "ProxyStatistics.h"
#include "ThreadPool.h"
#include <atomic>
#include <string>

struct HttpRequest;

class HttpProxyServer {
public:
    HttpProxyServer(int port, size_t workerCount, const std::string& cacheDirectory);
    ~HttpProxyServer();
    int Run();
private:
    void HandleClient(int clientFd);
    void SendError(int clientFd, int status, const std::string& reason, const std::string& details);
    bool ForwardRequest(int clientFd, const HttpRequest& request, const std::string& cacheKey, const CacheEntry* staleEntry);
    int m_port;
    int m_serverFd = -1;
    CacheManager m_cache;
    ThreadPool m_pool;
    ProxyStatistics m_statistics;
    std::atomic<bool> m_running{true};
};
