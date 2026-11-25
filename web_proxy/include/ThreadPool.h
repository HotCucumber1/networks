#pragma once
#include <condition_variable>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

class ThreadPool {
public:
    ThreadPool(size_t threadCount, size_t queueLimit);
    ~ThreadPool();
    bool Submit(std::function<void()> task);
private:
    void Worker();
    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    size_t m_queueLimit;
    bool m_stopping = false;
};
