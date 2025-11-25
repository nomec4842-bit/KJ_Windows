#ifdef _WIN32

#include "hosting/VSTGuiThread.h"

#include <iostream>

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

    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        tasks_.push(std::move(pending));
    }

    if (threadId_.load(std::memory_order_acquire) != 0)
        ::PostThreadMessageW(threadId_.load(std::memory_order_acquire), kRunTaskMessage, 0, 0);

    return future;
}

void VSTGuiThread::ensureStarted()
{
    if (running_.load(std::memory_order_acquire))
        return;

    running_.store(true, std::memory_order_release);
    thread_ = std::thread([this]() { threadMain(); });
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
}

void VSTGuiThread::threadMain()
{
    threadId_.store(::GetCurrentThreadId(), std::memory_order_release);

    MSG msg {};
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

#endif // _WIN32

