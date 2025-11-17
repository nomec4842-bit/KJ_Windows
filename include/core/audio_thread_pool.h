#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <type_traits>
#include <vector>

class AudioThreadPool
{
public:
    explicit AudioThreadPool(std::size_t threadCount);
    ~AudioThreadPool();

    template <typename Func>
    auto submit(Func&& func) -> std::future<typename std::invoke_result_t<Func>>;

private:
    void workerLoop();

    std::vector<std::thread> m_workers;
    std::queue<std::function<void()>> m_tasks;
    std::mutex m_mutex;
    std::condition_variable m_condition;
    bool m_stop = false;
};

template <typename Func>
inline auto AudioThreadPool::submit(Func&& func) -> std::future<typename std::invoke_result_t<Func>>
{
    using ResultType = typename std::invoke_result_t<Func>;
    auto task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Func>(func));
    std::future<ResultType> future = task->get_future();
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_tasks.emplace([task]() { (*task)(); });
    }
    m_condition.notify_one();
    return future;
}

