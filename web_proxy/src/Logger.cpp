#include "../include/Logger.h"
#include <ctime>
#include <iomanip>
#include <iostream>

std::mutex Logger::m_mutex;

void Logger::Write(const std::string& level, const std::string& message)
{
	std::lock_guard<std::mutex> lock(m_mutex);
	const auto now = std::time(nullptr);
	std::tm timeInfo{};
	localtime_r(&now, &timeInfo);
	std::cout << std::put_time(&timeInfo, "%Y-%m-%d %H:%M:%S") << " [" << level << "] " << message << std::endl;
}

void Logger::Info(const std::string& message)
{
	Write("INFO", message);
}

void Logger::Error(const std::string& message)
{
	Write("ERROR", message);
}
