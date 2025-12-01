#ifdef _WIN32

#include "hosting/VST3AsyncLoader.h"

#include "hosting/VST3Host.h"
#include "hosting/VSTGuiThread.h"

#include <objbase.h>

#include <filesystem>
#include <iostream>

namespace kj {

std::shared_ptr<VST3AsyncLoader> VST3AsyncLoader::create(std::shared_ptr<VST3Host> host)
{
    return std::shared_ptr<VST3AsyncLoader>(new VST3AsyncLoader(std::move(host)));
}

VST3AsyncLoader::VST3AsyncLoader(std::shared_ptr<VST3Host> host)
    : host_(std::move(host))
{
}

void VST3AsyncLoader::setOnLoaded(std::function<void(bool success)> fn)
{
    onLoaded_ = std::move(fn);
}

void VST3AsyncLoader::loadPlugin(const std::wstring& path)
{
    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true))
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queuedPath_ = path;
        }

        std::wcerr << L"[KJ] Plug-in load request suppressed because a load is already in progress: "
                    << path << std::endl;

        notifyLoaded(false);
        return;
    }

    loaded_.store(false, std::memory_order_release);
    failed_.store(false, std::memory_order_release);

    auto self = shared_from_this();
    std::thread([self, path]() {
        self->workerLoad(path);
    }).detach();
}

void VST3AsyncLoader::workerLoad(const std::wstring& path)
{
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool comInitialized = SUCCEEDED(hr);
    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        std::wcerr << L"[KJ] Failed to initialize COM for VST3 load (CoInitializeEx HRESULT=0x"
                    << std::hex << hr << std::dec << L")" << std::endl;
    }

    auto host = host_.lock();
    if (!host)
    {
        failed_.store(true, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        notifyLoaded(false);
        if (comInitialized)
            CoUninitialize();
        return;
    }

    if (FAILED(hr) && hr != RPC_E_CHANGED_MODE)
    {
        failed_.store(true, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        notifyLoaded(false);
        if (comInitialized)
            CoUninitialize();
        startQueuedLoadIfNeeded();
        return;
    }

    const auto utf8Path = std::filesystem::path(path).u8string();
    const bool success = host->load(utf8Path);

    loaded_.store(success, std::memory_order_release);
    failed_.store(!success, std::memory_order_release);
    loading_.store(false, std::memory_order_release);

    if (comInitialized)
        CoUninitialize();

    notifyLoaded(success);

    startQueuedLoadIfNeeded();
}

void VST3AsyncLoader::startQueuedLoadIfNeeded()
{
    std::optional<std::wstring> queuedPath;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queuedPath_)
        {
            queuedPath = std::move(queuedPath_);
            queuedPath_.reset();
        }
    }

    if (queuedPath)
        loadPlugin(*queuedPath);
}

void VST3AsyncLoader::notifyLoaded(bool success)
{
    auto self = shared_from_this();
    VSTGuiThread::instance().post([self, success]() {
        if (self->onLoaded_)
            self->onLoaded_(success);
    });
}

} // namespace kj

#endif // _WIN32
