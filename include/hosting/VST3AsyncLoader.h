#pragma once

#ifdef _WIN32

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace kj {

class VST3Host;

class VST3AsyncLoader : public std::enable_shared_from_this<VST3AsyncLoader>
{
public:
    static std::shared_ptr<VST3AsyncLoader> create(std::shared_ptr<VST3Host> host);

    void loadPlugin(const std::wstring& path);
    void setOnLoaded(std::function<void(bool success)> fn);

private:
    explicit VST3AsyncLoader(std::shared_ptr<VST3Host> host);

    void workerLoad(const std::wstring& path);
    void notifyLoaded(bool success);

    std::weak_ptr<VST3Host> host_;
    std::function<void(bool)> onLoaded_;

    std::atomic<bool> loading_ {false};
    std::atomic<bool> loaded_ {false};
    std::atomic<bool> failed_ {false};
};

} // namespace kj

#endif // _WIN32
