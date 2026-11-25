#pragma once
#include <atomic>
#include <cstdint>

class ProxyStatistics
{
public:
	void AddRequest()
	{
		++m_requests;
	}
	void AddHit()
	{
		++m_hits;
	}
	void AddMiss()
	{
		++m_misses;
	}
	void AddError()
	{
		++m_errors;
	}
	void AddBytes(uint64_t bytes)
	{
		m_bytes += bytes;
	}
	void Print() const;
	uint64_t GetRequestCount() const
	{
		return m_requests.load();
	}

private:
	std::atomic<uint64_t> m_requests{ 0 };
	std::atomic<uint64_t> m_hits{ 0 };
	std::atomic<uint64_t> m_misses{ 0 };
	std::atomic<uint64_t> m_errors{ 0 };
	std::atomic<uint64_t> m_bytes{ 0 };
};
