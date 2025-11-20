#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

class AudioThreadPool
{
public:
    explicit AudioThreadPool(std::size_t threadCount);
    ~AudioThreadPool();

    template <typename Func>
    auto submit(Func&& func) -> std::future<typename std::invoke_result_t<Func>>;

    bool isStopping();

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

    std::shared_ptr<std::packaged_task<ResultType()>> task;
    std::future<ResultType> future;

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_stop)
        {
            std::promise<ResultType> promise;
            promise.set_exception(std::make_exception_ptr(std::runtime_error("AudioThreadPool is stopping")));
            return promise.get_future();
        }

        task = std::make_shared<std::packaged_task<ResultType()>>(std::forward<Func>(func));
        future = task->get_future();
        m_tasks.emplace([task]() { (*task)(); });
    }

    m_condition.notify_one();
    return future;
}

inline bool AudioThreadPool::isStopping()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_stop;
}

