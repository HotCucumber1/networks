#pragma once
#include <string>
#include <sys/types.h>

class SocketUtils {
public:
    static int CreateServerSocket(int port, int backlog);
    static int ConnectToHost(const std::string& host, int port, int timeoutSeconds);
    static bool SendAll(int socketFd, const std::string& data);
    static bool SendAll(int socketFd, const char* data, size_t size);
    static bool ReceiveHeaders(int socketFd, std::string& data, size_t maxBytes);
    static bool ReceiveToEnd(int socketFd, std::string& data, size_t maxBytes);
    static void SetTimeouts(int socketFd, int timeoutSeconds);
};
