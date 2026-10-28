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
