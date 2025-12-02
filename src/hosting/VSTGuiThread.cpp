#ifdef _WIN32

#include "hosting/VSTGuiThread.h"

#include "core/track_type_vst.h"
#include "hosting/VST3Host.h"

#include <array>
#include <iostream>
#include <commdlg.h>
#include <filesystem>

namespace kj {

using namespace kj;

namespace {

constexpr const wchar_t* kSafeParentWindowClass = L"KJ_VST3_SAFE_PARENT";

LRESULT CALLBACK SafeParentWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

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
    DWORD lastError = ERROR_SUCCESS;
    for (int attempt = 0; attempt < 3 && running_.load(std::memory_order_acquire); ++attempt)
    {
        if (::PostThreadMessageW(threadId, kRunTaskMessage, 0, 0))
        {
            posted = true;
            break;
        }
        lastError = ::GetLastError();
        ::Sleep(1);
    }

    if (!posted)
    {
        std::cerr << "[VSTGuiThread] Failed to post wake message to GUI thread (error "
                  << lastError << ")." << std::endl;

        std::queue<PendingTask> failedTasks;
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            std::swap(failedTasks, tasks_);
        }

        while (!failedTasks.empty())
        {
            auto failed = std::move(failedTasks.front());
            failedTasks.pop();
            failed.promise.set_value(false);
        }
    }

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

HWND VSTGuiThread::createSafeParentWindowOnGuiThread()
{
    auto current = safeParentWindow_.load(std::memory_order_acquire);
    if (current && ::IsWindow(current))
        return current;

    std::call_once(safeParentWindowClassRegistered_, []() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &SafeParentWndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kSafeParentWindowClass;
        ::RegisterClassExW(&wc);
    });

    HWND created = ::CreateWindowExW(WS_EX_TOOLWINDOW, kSafeParentWindowClass, L"KJ VST3 Editor Host",
                                     WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN, CW_USEDEFAULT,
                                     CW_USEDEFAULT, 640, 480, nullptr, nullptr, ::GetModuleHandleW(nullptr),
                                     nullptr);
    if (!created)
        return nullptr;

    ::ShowWindow(created, SW_SHOWNOACTIVATE);
    safeParentWindow_.store(created, std::memory_order_release);
    return created;
}

HWND VSTGuiThread::ensureSafeParentWindow()
{
    ensureStarted();

    auto current = safeParentWindow_.load(std::memory_order_acquire);
    if (current && ::IsWindow(current))
        return current;

    auto promise = std::make_shared<std::promise<HWND>>();
    auto future = promise->get_future();

    auto task = [this, promise]() {
        promise->set_value(createSafeParentWindowOnGuiThread());
    };

    if (isGuiThread())
    {
        task();
    }
    else
    {
        auto result = post(task);
        if (!result.valid())
            return nullptr;
        result.wait();
    }

    try
    {
        return future.get();
    }
    catch (...)
    {
        return nullptr;
    }
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

    auto parent = safeParentWindow_.load(std::memory_order_acquire);
    if (parent && ::IsWindow(parent))
    {
        ::DestroyWindow(parent);
        safeParentWindow_.store(nullptr, std::memory_order_release);
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

std::filesystem::path getDefaultVstPluginPath()
{
    std::array<wchar_t, MAX_PATH> buffer{};
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size())
        return {};

    std::filesystem::path exePath(buffer.data());
    return exePath.parent_path() / L"plugins" / L"TestPlugin.vst3";
}

VstUiState queryVstUiState(int activeTrackId, const ::Track* activeTrack)
{
    VstUiState state{};
    if (activeTrack)
    {
        state.showLoader = activeTrack->type == TrackType::VST;
        if (state.showLoader)
            state.host = activeTrack->vstHost;
    }
    else if (activeTrackId > 0)
    {
        TrackType trackType = trackGetType(activeTrackId);
        state.showLoader = trackType == TrackType::VST;
        if (state.showLoader)
            state.host = trackGetVstHost(activeTrackId);
    }

    if (state.host)
    {
        state.editorAvailable = state.host->isPluginReady();
        state.editorLoading = state.host->isPluginLoading();
    }

    return state;
}

bool handleShowVstEditor(HWND parent, int trackId)
{
    auto host = trackGetVstHost(trackId);
    if (host && host->isPluginReady())
    {
        auto task = [host, parent]() { host->showPluginUI(parent); };
        auto& guiThread = VSTGuiThread::instance();
        if (guiThread.isGuiThread())
        {
            task();
            return true;
        }

        auto result = guiThread.post(task);
        return result.valid() && result.get();
    }

    if (host && host->isPluginLoading())
    {
        std::cout << "[GUI] VST3 plug-in is still loading; editor will open when ready." << std::endl;
    }
    else
    {
        std::cout << "[GUI] VST3 plug-in editor request received but plug-in is not ready." << std::endl;
    }

    return false;
}

bool promptAndLoadVstPlugin(HWND parent, int trackId)
{
    wchar_t fileBuffer[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = parent;
    ofn.lpstrFilter = L"VST3 Plug-ins\0*.vst3\0All Files\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"vst3";

    std::filesystem::path pluginPath;
    if (GetOpenFileNameW(&ofn))
    {
        pluginPath = std::filesystem::path(fileBuffer);
    }
    else
    {
        pluginPath = getDefaultVstPluginPath();
        if (!pluginPath.empty())
            std::wcout << L"[GUI] Using default VST3 path: " << pluginPath.c_str() << std::endl;
    }

    if (pluginPath.empty())
    {
        std::cout << "[GUI] No VST3 plug-in selected." << std::endl;
        return false;
    }

    if (!std::filesystem::exists(pluginPath))
        std::wcout << L"[GUI] Selected plug-in path does not exist: " << pluginPath.c_str() << std::endl;

    auto host = trackEnsureVstHost(trackId);
    if (!host)
    {
        std::cerr << "[GUI] Failed to obtain VST3 host for track." << std::endl;
        return false;
    }

    host->setOnPluginLoaded([parent, trackId](bool success) {
        if (success)
            PostMessageW(parent, kShowVstEditorMessage, static_cast<WPARAM>(trackId), 0);
        else
            std::cerr << "[GUI] VST3 plug-in did not finish loading; editor will not be shown." << std::endl;
    });

    host->loadPluginAsync(pluginPath.wstring());
    std::cout << "[GUI] Requested async VST3 plug-in load: " << pluginPath.string() << std::endl;
    return true;
}

} // namespace kj

#endif // _WIN32

