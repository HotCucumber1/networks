#pragma once
#include <map>
#include <string>

struct HttpRequest {
    std::string m_method;
    std::string m_target;
    std::string m_version;
    std::string m_host;
    int m_port = 80;
    std::string m_path;
    std::map<std::string, std::string> m_headers;
    std::string m_body;
};

struct HttpResponse {
    int m_statusCode = 0;
    std::string m_raw;
    std::map<std::string, std::string> m_headers;
};
