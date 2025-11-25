#pragma once

#ifdef _WIN32

#include <atomic>
#include <condition_variable>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <windows.h>

namespace kj {

class VSTGuiThread
{
public:
    static VSTGuiThread& instance();

    VSTGuiThread(const VSTGuiThread&) = delete;
    VSTGuiThread& operator=(const VSTGuiThread&) = delete;

    std::future<bool> post(std::function<void()> task);
    bool isGuiThread() const;
    void shutdown();

private:
    VSTGuiThread();
    ~VSTGuiThread();

    void ensureStarted();
    void threadMain();
    void drainTasks();

    static constexpr UINT kRunTaskMessage = WM_APP + 0x230;

    std::atomic<DWORD> threadId_ {0};
    std::thread thread_;
    std::atomic<bool> running_ {false};

    struct PendingTask
    {
        std::function<void()> fn;
        std::promise<bool> promise;
    };

    std::mutex queueMutex_;
    std::queue<PendingTask> tasks_;
};

} // namespace kj

#endif // _WIN32

