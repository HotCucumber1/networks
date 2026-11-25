#include "../include/HttpProxyServer.h"
#include "../include/Logger.h"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <thread>

int main(int argc, char* argv[])
{
	int port = 8888;
	size_t workers = std::max(4u, std::thread::hardware_concurrency());
	std::string cacheDirectory = "proxy_cache";
	if (argc > 1)
	{
		port = std::atoi(argv[1]);
	}
	if (argc > 2)
	{
		workers = static_cast<size_t>(std::max(1, std::atoi(argv[2])));
	}
	if (argc > 3)
	{
		cacheDirectory = argv[3];
	}
	if (port < 1 || port > 65535 || workers > 256)
	{
		std::cerr << "Usage: " << argv[0] << " [port:1-65535] [workers:1-256] [cache_directory]\n";
		return 1;
	}
	Logger::Info("Starting proxy: port=" + std::to_string(port) + ", workers=" + std::to_string(workers) + ", cache=" + cacheDirectory);
	HttpProxyServer server(port, workers, cacheDirectory);
	return server.Run();
}
