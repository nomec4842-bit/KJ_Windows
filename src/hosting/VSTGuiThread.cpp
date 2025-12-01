#ifdef _WIN32

#include "hosting/VSTGuiThread.h"

#include <iostream>

namespace kj {

using namespace kj;

VSTGuiThread::VSTGuiThread() = default;
VSTGuiThread::~VSTGuiThread()
{
    shutdown();
}

VSTGuiThread& VSTGuiThread::instance()
{
    static VSTGuiThread thread;
    thread.ensureStarted();
    return thread;
}

bool VSTGuiThread::isGuiThread() const
{
    return threadId_.load(std::memory_order_acquire) == ::GetCurrentThreadId();
}

std::future<bool> VSTGuiThread::post(std::function<void()> task)
{
    ensureStarted();

    PendingTask pending;
    pending.fn = std::move(task);
    auto future = pending.promise.get_future();

    std::unique_lock<std::mutex> startLock(threadStartMutex_);
    threadStartedCv_.wait(startLock, [this]() {
        return threadId_.load(std::memory_order_acquire) != 0
            || !running_.load(std::memory_order_acquire);
    });

    const auto threadId = threadId_.load(std::memory_order_acquire);
    if (threadId == 0)
    {
        pending.promise.set_value(false);
        return future;
    }

    startLock.unlock();

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        tasks_.push(std::move(pending));
    }

    bool posted = false;
    for (int attempt = 0; attempt < 3 && running_.load(std::memory_order_acquire); ++attempt)
    {
        if (::PostThreadMessageW(threadId, kRunTaskMessage, 0, 0))
        {
            posted = true;
            break;
        }
        ::Sleep(1);
    }

    if (!posted)
        std::cerr << "[VSTGuiThread] Failed to post wake message to GUI thread." << std::endl;

    return future;
}

void VSTGuiThread::ensureStarted()
{
    bool expectedRunning = false;
    if (running_.compare_exchange_strong(expectedRunning, true, std::memory_order_acq_rel))
        thread_ = std::thread([this]() { threadMain(); });

    std::unique_lock<std::mutex> lock(threadStartMutex_);
    threadStartedCv_.wait(lock, [this]() {
        return threadId_.load(std::memory_order_acquire) != 0
            || !running_.load(std::memory_order_acquire);
    });
}

void VSTGuiThread::shutdown()
{
    const bool wasRunning = running_.exchange(false, std::memory_order_acq_rel);
    if (!wasRunning)
        return;

    if (threadId_.load(std::memory_order_acquire) != 0)
        ::PostThreadMessageW(threadId_.load(std::memory_order_acquire), WM_QUIT, 0, 0);

    if (thread_.joinable())
        thread_.join();

    // Ensure the next startup waits for a fresh thread ID rather than using a stale value.
    threadId_.store(0, std::memory_order_release);
}

void VSTGuiThread::threadMain()
{
    threadId_.store(::GetCurrentThreadId(), std::memory_order_release);

    MSG msg {};
    ::PeekMessageW(&msg, nullptr, 0, 0, PM_NOREMOVE);

    {
        std::lock_guard<std::mutex> lock(threadStartMutex_);
        threadStartedCv_.notify_all();
    }

    while (running_.load(std::memory_order_acquire))
    {
        BOOL result = ::GetMessageW(&msg, nullptr, 0, 0);
        if (result == -1)
            continue;
        if (result == 0)
            break;

        if (msg.message == kRunTaskMessage)
        {
            drainTasks();
            continue;
        }

        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }

    drainTasks();

    // Clear the thread ID so future wakeups won't target a dead GUI thread.
    threadId_.store(0, std::memory_order_release);
}

void VSTGuiThread::drainTasks()
{
    std::queue<PendingTask> pending;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        std::swap(pending, tasks_);
    }

    while (!pending.empty())
    {
        auto task = std::move(pending.front());
        pending.pop();

        try
        {
            if (task.fn)
                task.fn();
            task.promise.set_value(true);
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[VSTGuiThread] Task threw exception: " << ex.what() << std::endl;
            task.promise.set_value(false);
        }
        catch (...)
        {
            std::cerr << "[VSTGuiThread] Task threw unknown exception." << std::endl;
            task.promise.set_value(false);
        }
    }
}

} // namespace kj

#endif // _WIN32

