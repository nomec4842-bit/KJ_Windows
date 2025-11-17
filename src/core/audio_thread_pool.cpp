#include "core/audio_thread_pool.h"

AudioThreadPool::AudioThreadPool(std::size_t threadCount)
{
    if (threadCount == 0)
        threadCount = 1;

    m_workers.reserve(threadCount);
    for (std::size_t i = 0; i < threadCount; ++i)
    {
        m_workers.emplace_back([this]() { workerLoop(); });
    }
}

AudioThreadPool::~AudioThreadPool()
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
    }
    m_condition.notify_all();
    for (auto& worker : m_workers)
    {
        if (worker.joinable())
            worker.join();
    }
}

void AudioThreadPool::workerLoop()
{
    while (true)
    {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_condition.wait(lock, [this]() { return m_stop || !m_tasks.empty(); });
            if (m_stop && m_tasks.empty())
                return;
            task = std::move(m_tasks.front());
            m_tasks.pop();
        }
        task();
    }
}

