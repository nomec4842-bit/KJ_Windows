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

namespace
{

const wchar_t* apartmentName(COINIT apartment)
{
    switch (apartment)
    {
    case COINIT_APARTMENTTHREADED:
        return L"COINIT_APARTMENTTHREADED";
    case COINIT_MULTITHREADED:
        return L"COINIT_MULTITHREADED";
    default:
        return L"(unknown COINIT)";
    }
}

struct ComInitializationResult
{
    bool initialized {false};
    bool usedFallback {false};
    COINIT apartmentMode {COINIT_APARTMENTTHREADED};
    HRESULT hr {S_OK};
};

ComInitializationResult initializeCom(COINIT requested)
{
    ComInitializationResult result{};
    result.apartmentMode = requested;
    result.hr = CoInitializeEx(nullptr, requested);
    if (SUCCEEDED(result.hr))
    {
        result.initialized = true;
        return result;
    }

    if (result.hr == RPC_E_CHANGED_MODE)
    {
        result.usedFallback = true;
        result.apartmentMode = (requested == COINIT_APARTMENTTHREADED) ? COINIT_MULTITHREADED
                                                                      : COINIT_APARTMENTTHREADED;
        result.hr = CoInitializeEx(nullptr, result.apartmentMode);
        if (SUCCEEDED(result.hr))
            result.initialized = true;
    }

    return result;
}

} // namespace

void VST3AsyncLoader::loadPlugin(const std::wstring& path, COINIT comApartment)
{
    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true))
    {
        {
            std::lock_guard<std::mutex> lock(queueMutex_);
            queuedRequest_ = QueuedLoadRequest{path, comApartment};
        }

        std::wcerr << L"[KJ] Deferring plug-in load while another load is in progress: "
                    << path << std::endl;
        return;
    }

    loaded_.store(false, std::memory_order_release);
    failed_.store(false, std::memory_order_release);

    auto self = shared_from_this();
    std::thread([self, path, comApartment]() {
        self->workerLoad(path, comApartment);
    }).detach();
}

void VST3AsyncLoader::workerLoad(const std::wstring& path, COINIT comApartment)
{
    const auto comInit = initializeCom(comApartment);
    if (comInit.initialized)
    {
        std::wcerr << L"[KJ] VST3 loader initialized COM with "
                    << apartmentName(comInit.apartmentMode)
                    << (comInit.usedFallback ? L" after RPC_E_CHANGED_MODE" : L"")
                    << L" (HRESULT=0x" << std::hex << comInit.hr << std::dec << L")" << std::endl;
    }
    else if (FAILED(comInit.hr))
    {
        std::wcerr << L"[KJ] Failed to initialize COM for VST3 load (CoInitializeEx HRESULT=0x"
                    << std::hex << comInit.hr << std::dec << L")" << std::endl;
    }

    auto host = host_.lock();
    if (!host)
    {
        failed_.store(true, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        notifyLoaded(false);
        if (comInit.initialized)
            CoUninitialize();
        return;
    }

    if (FAILED(comInit.hr))
    {
        failed_.store(true, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        notifyLoaded(false);
        if (comInit.initialized)
            CoUninitialize();
        startQueuedLoadIfNeeded();
        return;
    }

    const auto utf8Path = std::filesystem::path(path).u8string();
    const bool success = host->load(utf8Path);

    loaded_.store(success, std::memory_order_release);
    failed_.store(!success, std::memory_order_release);
    loading_.store(false, std::memory_order_release);

    if (comInit.initialized)
        CoUninitialize();

    notifyLoaded(success);

    startQueuedLoadIfNeeded();
}

void VST3AsyncLoader::startQueuedLoadIfNeeded()
{
    std::optional<QueuedLoadRequest> queuedRequest;
    {
        std::lock_guard<std::mutex> lock(queueMutex_);
        if (queuedRequest_)
        {
            queuedRequest = std::move(queuedRequest_);
            queuedRequest_.reset();
        }
    }

    if (queuedRequest)
        loadPlugin(queuedRequest->path, queuedRequest->comApartment);
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
