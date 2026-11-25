#pragma once
#include <mutex>
#include <string>

struct CacheEntry
{
	bool m_found = false;
	bool m_fresh = false;
	std::string m_response;
	std::string m_etag;
	std::string m_lastModified;
};

class CacheManager
{
public:
	explicit CacheManager(std::string directory);
	CacheEntry Get(const std::string& key) const;
	void Store(const std::string& key, const std::string& response) const;
	void Refresh(const std::string& key, const std::string& response304) const;
	size_t GetEntryCount() const;

private:
	std::string GetPath(const std::string& key, const std::string& extension) const;
	static std::string HashKey(const std::string& key);
	static long long GetExpiry(const std::string& response);
	std::string m_directory;
	mutable std::mutex m_mutex;
};
