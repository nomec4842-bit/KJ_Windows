#pragma once

#if !defined(_WIN32) && !defined(_WIN64)
#error "VSTGuiThread is only supported on Windows; ensure _WIN32 or _WIN64 is defined."
#endif

#ifdef _WIN32

#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>
#include <windows.h>

#include "pluginterfaces/base/funknown.h"
#include "hosting/VST3Host.h"
#include "pluginterfaces/gui/iplugview.h"

struct Track;

namespace kj {

struct VstUiState
{
    bool showLoader = false;
    bool editorAvailable = false;
    bool editorLoading = false;
    std::shared_ptr<VST3Host> host;
};

class VSTGuiThread
{
public:
    using RunLoopPtr =
#if SMTG_OS_LINUX
        Steinberg::IPtr<Steinberg::Linux::IRunLoop>;
#else
        Steinberg::IPtr<Steinberg::FUnknown>;
#endif

    static VSTGuiThread& instance();

    VSTGuiThread(const VSTGuiThread&) = delete;
    VSTGuiThread& operator=(const VSTGuiThread&) = delete;

    std::future<bool> post(std::function<void()> task);
    bool isGuiThread() const;
    void shutdown();
    HWND ensureSafeParentWindow();
    RunLoopPtr getRunLoop();

#if SMTG_OS_LINUX
    Steinberg::tresult registerTimerHandler(Steinberg::Linux::ITimerHandler* handler,
                                            Steinberg::Linux::TimerInterval milliseconds);
    Steinberg::tresult unregisterTimerHandler(Steinberg::Linux::ITimerHandler* handler);
#else
    Steinberg::tresult registerTimerHandler(void* handler, Steinberg::uint32 milliseconds = 0);
    Steinberg::tresult unregisterTimerHandler(void* handler);
#endif

private:
    VSTGuiThread();
    ~VSTGuiThread();

    void ensureStarted();
    void threadMain();
    void drainTasks();
    HWND createSafeParentWindowOnGuiThread();
#if SMTG_OS_LINUX
    Steinberg::tresult registerTimerInternal(
        Steinberg::Linux::ITimerHandler* handler,
        Steinberg::Linux::TimerInterval milliseconds);
    Steinberg::tresult unregisterTimerInternal(Steinberg::Linux::ITimerHandler* handler);
    void handleTimer(UINT_PTR timerId);
    void clearTimersOnGuiThread();
#endif

#if SMTG_OS_LINUX
    class RunLoop;
#endif

    static constexpr UINT kRunTaskMessage = WM_APP + 0x230;

    std::atomic<DWORD> threadId_ {0};
    std::thread thread_;
    std::atomic<bool> running_ {false};
    std::condition_variable threadStartedCv_;
    std::mutex threadStartMutex_;

    struct PendingTask
    {
        std::function<void()> fn;
        std::promise<bool> promise;
    };

    std::mutex queueMutex_;
    std::queue<PendingTask> tasks_;

#if SMTG_OS_LINUX
    std::atomic<UINT_PTR> nextTimerId_ {1};
    std::mutex timerMutex_;
    std::unordered_map<UINT_PTR, Steinberg::IPtr<Steinberg::Linux::ITimerHandler>> timersById_;
    std::unordered_map<Steinberg::Linux::ITimerHandler*, UINT_PTR> timerIdsByHandler_;
#endif
    RunLoopPtr runLoop_;

    std::atomic<HWND> safeParentWindow_ {nullptr};
    std::once_flag safeParentWindowClassRegistered_;
};

constexpr UINT kShowVstEditorMessage = WM_APP + 40;

VstUiState queryVstUiState(int activeTrackId, const ::Track* activeTrack);
bool handleShowVstEditor(HWND parent, int trackId);
bool promptAndLoadVstPlugin(HWND parent, int trackId);
std::filesystem::path getDefaultVstPluginPath();

} // namespace kj

#endif // _WIN32
