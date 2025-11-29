#ifdef _WIN32

#include "hosting/VST3AsyncLoader.h"

#include "hosting/VST3Host.h"
#include "hosting/VSTGuiThread.h"

#include <filesystem>

namespace kj {

std::shared_ptr<VST3AsyncLoader> VST3AsyncLoader::create(std::shared_ptr<VST3Host> host)
{
    return std::shared_ptr<VST3AsyncLoader>(new VST3AsyncLoader(std::move(host)));
}

VST3AsyncLoader::VST3AsyncLoader(std::shared_ptr<VST3Host> host)
    : host_(std::move(host))
{
}

void VST3AsyncLoader::setOnLoaded(std::function<void()> fn)
{
    onLoaded_ = std::move(fn);
}

void VST3AsyncLoader::loadPlugin(const std::wstring& path)
{
    bool expected = false;
    if (!loading_.compare_exchange_strong(expected, true))
        return;

    loaded_.store(false, std::memory_order_release);
    failed_.store(false, std::memory_order_release);

    auto self = shared_from_this();
    std::thread([self, path]() {
        self->workerLoad(path);
    }).detach();
}

void VST3AsyncLoader::workerLoad(const std::wstring& path)
{
    auto host = host_.lock();
    if (!host)
    {
        failed_.store(true, std::memory_order_release);
        loading_.store(false, std::memory_order_release);
        notifyLoaded();
        return;
    }

    const auto utf8Path = std::filesystem::path(path).u8string();
    const bool success = host->load(utf8Path);

    loaded_.store(success, std::memory_order_release);
    failed_.store(!success, std::memory_order_release);
    loading_.store(false, std::memory_order_release);

    notifyLoaded();
}

void VST3AsyncLoader::notifyLoaded()
{
    auto self = shared_from_this();
    VSTGuiThread::instance().post([self]() {
        if (self->onLoaded_)
            self->onLoaded_();
    });
}

} // namespace kj

#endif // _WIN32
