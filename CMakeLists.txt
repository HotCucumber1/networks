cmake_minimum_required(VERSION 3.28.3)
project(networks)

set(CMAKE_CXX_STANDARD 26)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

add_executable(dns_resolver dns_resolver/dns_resolver.cpp)
add_executable(smtp_client smtp_client/smtp_client.cpp)
add_executable(web_server web_server/webserver.cpp)
add_executable(sockets_server sockets/server.c)
add_executable(sockets_client sockets/client.c)
add_executable(sockets_loadtest sockets/loadtest.c)
add_executable(rdt_sender rdtp_project/rdt_sender.cpp)
add_executable(rdt_receiver rdtp_project/rdt_receiver.cpp)

add_executable(udp_pinger_server udp_pinger/udp_pinger_server.cpp)
add_executable(udp_pinger_client udp_pinger/udp_pinger_client.cpp)

add_executable(proxy_server
        web_proxy/src/main.cpp
        web_proxy/src/HttpParser.cpp
        web_proxy/src/SocketUtils.cpp
        web_proxy/src/CacheManager.cpp
        web_proxy/src/ThreadPool.cpp
        web_proxy/src/Logger.cpp
        web_proxy/src/ProxyStatistics.cpp
        web_proxy/src/HttpProxyServer.cpp
)
target_include_directories(proxy_server PRIVATE include)
find_package(Threads REQUIRED)
target_link_libraries(proxy_server PRIVATE Threads::Threads)
