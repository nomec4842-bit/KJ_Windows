#include "audio/thread_pool.h"

#include <algorithm>

namespace {
std::size_t clampThreadCount(std::size_t requested)
{
    constexpr std::size_t kMinThreads = 2;
    if (requested == 0)
        return kMinThreads;
    return std::max(kMinThreads, requested);
}
}

void notifyFinished(JobGroup& group)
{
    int previous = group.remaining.fetch_sub(1, std::memory_order_acq_rel);
    if (previous == 1)
    {
        std::lock_guard<std::mutex> lock(group.mtx);
        group.cv.notify_one();
    }
}

void waitUntilFinished(JobGroup& group)
{
    if (group.remaining.load(std::memory_order_acquire) == 0)
        return;

    std::unique_lock<std::mutex> lock(group.mtx);
    group.cv.wait(lock, [&group]() {
        return group.remaining.load(std::memory_order_acquire) == 0;
    });
}

ThreadPool::ThreadPool(std::size_t threadCount, std::size_t queueCapacity)
    : m_capacity(queueCapacity > 0 ? queueCapacity : 1),
      m_jobs(m_capacity)
{
    std::size_t workerCount = clampThreadCount(threadCount);
    m_workers.reserve(workerCount);
    for (std::size_t i = 0; i < workerCount; ++i)
    {
        m_workers.emplace_back([this]() { workerLoop(); });
    }
}

ThreadPool::~ThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop.store(true, std::memory_order_release);
    }
    m_condition.notify_all();
    for (auto& worker : m_workers)
    {
        if (worker.joinable())
            worker.join();
    }
}

bool ThreadPool::enqueue(Job job)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_size >= m_capacity || m_stop.load(std::memory_order_acquire))
        {
            return false;
        }

        m_jobs[m_tail] = std::move(job);
        m_tail = (m_tail + 1) % m_capacity;
        ++m_size;
    }
    m_condition.notify_one();
    return true;
}

void ThreadPool::workerLoop()
{
    while (true)
    {
        Job job;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]() {
                return m_stop.load(std::memory_order_acquire) || m_size > 0;
            });

            if (m_stop.load(std::memory_order_acquire) && m_size == 0)
                return;

            job = std::move(m_jobs[m_head]);
            m_head = (m_head + 1) % m_capacity;
            --m_size;
        }

        if (job)
        {
            job();
        }
    }
}

