#include "../include/CacheManager.h"
#include "../include/HttpParser.h"
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <utility>

CacheManager::CacheManager(std::string directory)
	: m_directory(std::move(directory))
{
	std::filesystem::create_directories(m_directory);
}

std::string CacheManager::HashKey(const std::string& key)
{
	uint64_t hash = 1469598103934665603ULL;
	for (unsigned char c : key)
	{
		hash ^= c;
		hash *= 1099511628211ULL;
	}
	std::ostringstream out;
	out << std::hex << hash;
	return out.str();
}

std::string CacheManager::GetPath(const std::string& key, const std::string& extension) const { return m_directory + "/" + HashKey(key) + extension; }

long long CacheManager::GetExpiry(const std::string& raw)
{
	HttpResponse response;
	if (!HttpParser::ParseResponse(raw, response))
	{
		return 0;
	}
	if (response.m_headers.count("set-cookie") || response.m_headers.count("vary"))
	{
		return 0;
	}
	long long ttl = 60;
	bool requiresValidation = false;
	const auto control = response.m_headers.find("cache-control");
	if (control != response.m_headers.end())
	{
		const std::string value = HttpParser::ToLower(control->second);
		if (value.find("no-store") != std::string::npos || value.find("private") != std::string::npos)
		{
			return 0;
		}
		requiresValidation = value.find("no-cache") != std::string::npos;
		const auto pos = value.find("max-age=");
		if (pos != std::string::npos)
		{
			try
			{
				ttl = std::stoll(value.substr(pos + 8));
			}
			catch (...)
			{
				ttl = 0;
			}
		}
	}
	if (requiresValidation)
	{
		ttl = 0;
	}
	if (ttl < 0)
	{
		ttl = 0;
	}
	if (ttl > 86400)
	{
		ttl = 86400;
	}
	return static_cast<long long>(std::time(nullptr)) + ttl;
}

CacheEntry CacheManager::Get(const std::string& key) const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	CacheEntry entry;
	std::ifstream file(GetPath(key, ".cache"), std::ios::binary);
	if (!file)
	{
		return entry;
	}
	entry.m_response.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
	if (entry.m_response.empty())
	{
		return entry;
	}
	HttpResponse parsed;
	if (!HttpParser::ParseResponse(entry.m_response, parsed) || parsed.m_statusCode != 200)
	{
		return entry;
	}
	entry.m_found = true;
	auto etag = parsed.m_headers.find("etag");
	if (etag != parsed.m_headers.end())
	{
		entry.m_etag = etag->second;
	}
	auto modified = parsed.m_headers.find("last-modified");
	if (modified != parsed.m_headers.end())
	{
		entry.m_lastModified = modified->second;
	}
	std::ifstream meta(GetPath(key, ".meta"));
	long long expiry = 0;
	if (meta)
	{
		meta >> expiry;
	}
	entry.m_fresh = expiry > static_cast<long long>(std::time(nullptr));
	return entry;
}

void CacheManager::Store(const std::string& key, const std::string& response) const
{
	const long long expiry = GetExpiry(response);
	if (expiry == 0)
	{
		return;
	}
	std::lock_guard lock(m_mutex);
	const std::string base = GetPath(key, ".cache");
	const std::string temp = base + ".tmp";
	{
		std::ofstream file(temp, std::ios::binary | std::ios::trunc);
		if (!file)
		{
			return;
		}
		file.write(response.data(), static_cast<std::streamsize>(response.size()));
		if (!file.good())
		{
			return;
		}
	}
	std::error_code error;
	std::filesystem::rename(temp, base, error);
	if (error)
	{
		std::filesystem::remove(temp);
		return;
	}
	std::ofstream meta(GetPath(key, ".meta"), std::ios::trunc);
	meta << expiry;
}

void CacheManager::Refresh(const std::string& key, const std::string& response304) const
{
	long long expiry = GetExpiry(response304);
	if (expiry == 0)
	{
		expiry = static_cast<long long>(std::time(nullptr)) + 60;
	}
	std::lock_guard<std::mutex> lock(m_mutex);
	std::ofstream meta(GetPath(key, ".meta"), std::ios::trunc);
	meta << expiry;
}

size_t CacheManager::GetEntryCount() const
{
	std::lock_guard<std::mutex> lock(m_mutex);
	size_t count = 0;
	for (const auto& item : std::filesystem::directory_iterator(m_directory))
	{
		if (item.path().extension() == ".cache")
		{
			++count;
		}
	}
	return count;
}
