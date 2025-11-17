#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

struct JobGroup
{
    std::atomic<int> remaining{0};
    std::mutex mtx;
    std::condition_variable cv;
};

void notifyFinished(JobGroup& group);
void waitUntilFinished(JobGroup& group);

class ThreadPool
{
public:
    using Job = std::function<void()>;

    ThreadPool(std::size_t threadCount, std::size_t queueCapacity);
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool enqueue(Job job);

    std::size_t capacity() const noexcept { return m_capacity; }

private:
    void workerLoop();

    const std::size_t m_capacity;
    std::vector<Job> m_jobs;
    std::vector<std::thread> m_workers;

    std::size_t m_head = 0;
    std::size_t m_tail = 0;
    std::size_t m_size = 0;

    std::mutex m_mutex;
    std::condition_variable m_condition;
    std::atomic<bool> m_stop{false};
};

