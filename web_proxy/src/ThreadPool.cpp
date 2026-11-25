#include "../include/ThreadPool.h"
#include <utility>

ThreadPool::ThreadPool(size_t threadCount, size_t queueLimit)
	: m_queueLimit(queueLimit)
{
	for (size_t i = 0; i < threadCount; ++i)
	{
		m_workers.emplace_back([this] { Worker(); });
	}
}
ThreadPool::~ThreadPool()
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		m_stopping = true;
	}
	m_condition.notify_all();
	for (auto& worker : m_workers)
	{
		if (worker.joinable())
		{
			worker.join();
		}
	}
}
bool ThreadPool::Submit(std::function<void()> task)
{
	{
		std::lock_guard<std::mutex> lock(m_mutex);
		if (m_stopping || m_tasks.size() >= m_queueLimit)
			return false;
		m_tasks.push(std::move(task));
	}
	m_condition.notify_one();
	return true;
}
void ThreadPool::Worker()
{
	while (true)
	{
		std::function<void()> task;
		{
			std::unique_lock<std::mutex> lock(m_mutex);
			m_condition.wait(lock, [this] { return m_stopping || !m_tasks.empty(); });
			if (m_stopping && m_tasks.empty())
				return;
			task = std::move(m_tasks.front());
			m_tasks.pop();
		}
		try
		{
			task();
		}
		catch (...)
		{
		}
	}
}
