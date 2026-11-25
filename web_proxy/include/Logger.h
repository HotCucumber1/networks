#pragma once
#include <mutex>
#include <string>

class Logger {
public:
    static void Info(const std::string& message);
    static void Error(const std::string& message);
private:
    static void Write(const std::string& level, const std::string& message);
    static std::mutex m_mutex;
};
