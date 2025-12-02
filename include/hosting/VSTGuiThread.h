#pragma once

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
#include <windows.h>

#include "hosting/VST3Host.h"

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
    std::condition_variable threadStartedCv_;
    std::mutex threadStartMutex_;

    struct PendingTask
    {
        std::function<void()> fn;
        std::promise<bool> promise;
    };

    std::mutex queueMutex_;
    std::queue<PendingTask> tasks_;
};

constexpr UINT kShowVstEditorMessage = WM_APP + 40;

VstUiState queryVstUiState(int activeTrackId, const ::Track* activeTrack);
bool handleShowVstEditor(HWND parent, int trackId);
bool promptAndLoadVstPlugin(HWND parent, int trackId);
std::filesystem::path getDefaultVstPluginPath();

} // namespace kj

#endif // _WIN32
