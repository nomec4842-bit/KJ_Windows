#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#pragma comment(lib, "comctl32.lib")
#endif

#include "hosting/VST3Host.h"
using namespace kj;

#include "hosting/VSTEditorWindow.h"
#include "hosting/VST3PlugFrame.h"
#include "hosting/VSTGuiThread.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cctype>
#include <filesystem>
#include <future>
#include <mutex>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>
#include <cstdint>

#include "pluginterfaces/base/ibstream.h"
#include "pluginterfaces/base/ipersistent.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/base/keycodes.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "pluginterfaces/vst/ivstmessage.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "public.sdk/source/vst/vstcomponent.h"
#include "public.sdk/source/vst/hosting/eventlist.h"

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

std::mutex gWindowClassMutex;
bool gWindowClassesRegistered = false;

std::string fuidToString(const Steinberg::FUID& id)
{
    Steinberg::FUID::String buffer{};
    id.toString(buffer);
    return std::string{buffer};
}

#ifdef _WIN32
class ScopedCurrentDirectory
{
public:
    explicit ScopedCurrentDirectory(const std::filesystem::path& newPath)
    {
        std::error_code ec;
        original_ = std::filesystem::current_path(ec);
        if (ec)
            return;

        std::filesystem::path target = newPath;
        if (!std::filesystem::is_directory(target, ec))
            target = newPath.parent_path();

        if (target.empty())
            return;

        auto absoluteTarget = std::filesystem::absolute(target, ec);
        if (ec)
            return;

        if (std::filesystem::is_directory(absoluteTarget, ec) &&
            ::SetCurrentDirectoryW(absoluteTarget.c_str()) != FALSE)
        {
            changed_ = true;
        }
    }

    ~ScopedCurrentDirectory()
    {
        if (!changed_ || original_.empty())
            return;

        std::error_code ec;
        std::filesystem::current_path(original_, ec);
    }

    ScopedCurrentDirectory(const ScopedCurrentDirectory&) = delete;
    ScopedCurrentDirectory& operator=(const ScopedCurrentDirectory&) = delete;

private:
    std::filesystem::path original_;
    bool changed_ = false;
};

std::filesystem::path ResolvePluginResourceDirectory(const std::filesystem::path& pluginPath)
{
    std::error_code ec;

    // Start from the binary path and walk up toward the bundle root.
    std::filesystem::path basePath = pluginPath;
    if (std::filesystem::is_regular_file(basePath, ec))
        basePath = basePath.parent_path();

    // Prefer the Contents/Resources directory when present (typical .vst3 layout).
    std::filesystem::path contentsPath = basePath;
    if (contentsPath.filename() != L"Contents")
    {
        auto candidate = basePath / "Contents";
        if (std::filesystem::is_directory(candidate, ec))
            contentsPath = candidate;
    }

    auto resourcePath = contentsPath / "Resources";
    if (std::filesystem::is_directory(resourcePath, ec))
        return resourcePath;

    // Fall back to a Resources folder adjacent to the binary, or the binary's directory.
    auto siblingResourcePath = basePath / "Resources";
    if (std::filesystem::is_directory(siblingResourcePath, ec))
        return siblingResourcePath;

    return basePath;
}
#endif

std::filesystem::path resolveVST3BundlePath(const std::filesystem::path& input)
{
    std::error_code ec;

    if (!input.has_extension() || input.extension() != ".vst3")
        return input;

    if (std::filesystem::is_regular_file(input, ec))
        return input;

    if (!std::filesystem::is_directory(input, ec))
        return input;

    const auto binaryRoot = input / "Contents" / "x86_64-win";
    if (!std::filesystem::is_directory(binaryRoot, ec))
        return input;

    auto findWithExtension = [&](const std::string& extension) -> std::filesystem::path {
        std::filesystem::directory_iterator it{binaryRoot, ec};
        if (ec)
            return {};

        for (const auto& entry : it)
        {
            if (!entry.is_regular_file(ec))
                continue;

            auto ext = entry.path().extension().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext == extension)
                return entry.path();
        }

        return {};
    };

    if (auto vst3 = findWithExtension(".vst3"); !vst3.empty())
        return vst3;

    if (auto dll = findWithExtension(".dll"); !dll.empty())
        return dll;

    return input;
}

class AudioProcessScope
{
public:
    AudioProcessScope(std::atomic<bool>& suspended, std::atomic<uint32_t>& activeCount)
        : suspended_(suspended), activeCount_(activeCount)
    {
        if (suspended_.load(std::memory_order_acquire))
            return;

        activeCount_.fetch_add(1, std::memory_order_acq_rel);
        if (suspended_.load(std::memory_order_acquire))
        {
            activeCount_.fetch_sub(1, std::memory_order_acq_rel);
            return;
        }

        engaged_ = true;
    }

    ~AudioProcessScope()
    {
        if (engaged_)
            activeCount_.fetch_sub(1, std::memory_order_acq_rel);
    }

    [[nodiscard]] bool engaged() const { return engaged_; }

private:
    std::atomic<bool>& suspended_;
    std::atomic<uint32_t>& activeCount_;
    bool engaged_ = false;
};

class VectorIBStream : public Steinberg::IBStream
{
public:
    explicit VectorIBStream(std::vector<uint8_t>& buffer) : writeBuffer_(&buffer)
    {
        writeBuffer_->clear();
    }

    VectorIBStream(const uint8_t* data, size_t size) : readData_(data), readSize_(size) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;

        *obj = nullptr;
        if (std::memcmp(iid, Steinberg::IBStream::iid, sizeof(TUID)) == 0 ||
            std::memcmp(iid, Steinberg::FUnknown::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<Steinberg::IBStream*>(this);
            addRef();
            return kResultOk;
        }
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override { return 1; }
    uint32 PLUGIN_API release() override { return 1; }

    tresult PLUGIN_API read(void* data, int32 numBytes, int32* numRead) override
    {
        if (!data || numBytes < 0 || !readData_)
            return kInvalidArgument;

        const int64 available = static_cast<int64>(readSize_) - position_;
        const int32 toRead = static_cast<int32>(std::max<int64>(0, std::min<int64>(available, numBytes)));

        if (toRead > 0)
            std::memcpy(data, readData_ + position_, static_cast<size_t>(toRead));

        position_ += toRead;
        if (numRead)
            *numRead = toRead;
        return kResultOk;
    }

    tresult PLUGIN_API write(void* data, int32 numBytes, int32* numWritten) override
    {
        if (!data || numBytes < 0 || !writeBuffer_)
            return kInvalidArgument;

        auto* bytes = reinterpret_cast<uint8_t*>(data);
        writeBuffer_->insert(writeBuffer_->end(), bytes, bytes + numBytes);
        position_ += numBytes;

        if (numWritten)
            *numWritten = numBytes;
        return kResultOk;
    }

    tresult PLUGIN_API seek(int64 pos, int32 mode, int64* result) override
    {
        int64 base = 0;
        switch (mode)
        {
            case Steinberg::IBStream::kIBSeekSet: base = 0; break;
            case Steinberg::IBStream::kIBSeekCur: base = position_; break;
            case Steinberg::IBStream::kIBSeekEnd:
                base = writeBuffer_ ? static_cast<int64>(writeBuffer_->size())
                                    : static_cast<int64>(readSize_);
                break;
            default: return kInvalidArgument;
        }

        int64 newPos = base + pos;
        if (newPos < 0)
            return kInvalidArgument;

        position_ = newPos;
        if (writeBuffer_ && position_ > static_cast<int64>(writeBuffer_->size()))
            writeBuffer_->resize(static_cast<size_t>(position_), 0);

        if (result)
            *result = position_;
        return kResultOk;
    }

    tresult PLUGIN_API tell(int64* pos) override
    {
        if (!pos)
            return kInvalidArgument;
        *pos = position_;
        return kResultOk;
    }

private:
    std::vector<uint8_t>* writeBuffer_ = nullptr;
    const uint8_t* readData_ = nullptr;
    size_t readSize_ = 0;
    int64 position_ = 0;
};

bool IsInvalidNormalizedValue(Steinberg::Vst::ParamValue value)
{
    return std::isnan(value);
}

bool IsInvalidPlainValue(Steinberg::Vst::ParamValue value)
{
    return std::isnan(value);
}

} // namespace

#ifdef _WIN32
namespace {
constexpr wchar_t kContainerWindowClassName[] = L"KJ_VST3_CONTAINER";
constexpr wchar_t kHeaderWindowClassName[] = L"KJ_VST3_HEADER";
constexpr wchar_t kFallbackWindowClassName[] = L"KJ_VST3_FALLBACK";
constexpr wchar_t kEditorWindowClassName[] = L"KJVSTEditorWindow";
constexpr wchar_t kStandaloneEditorWindowClassName[] = L"KJ_VST3_STANDALONE_EDITOR";
constexpr wchar_t kPluginViewWindowClassName[] = L"KJ_VST3_PLUGIN_VIEW_HOST";
constexpr int kHeaderHeight = 56;
constexpr UINT_PTR kHeaderFallbackButtonId = 1001;
constexpr UINT_PTR kHeaderCloseButtonId = 1002;
constexpr UINT_PTR kFallbackListViewId = 2001;
constexpr UINT_PTR kFallbackSliderId = 2002;
constexpr int kFallbackSliderRange = 1000;
constexpr UINT kFallbackRefreshMessage = WM_APP + 101;
constexpr UINT kShowVstEditorMessage = WM_APP + 40;
constexpr UINT_PTR kIdleTimerId = 3001;
constexpr UINT kIdleTimerIntervalMs = 15;
constexpr UINT_PTR kViewRepaintTimerId = 1;
constexpr UINT kViewRepaintIntervalMs = 16;

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    int required = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0)
        return std::wstring(value.begin(), value.end());

    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

std::wstring String128ToWide(const Steinberg::Vst::String128& value)
{
    Steinberg::UString string(const_cast<Steinberg::Vst::String128&>(value), VST3_STRING128_SIZE);
    Steinberg::char16 buffer[VST3_STRING128_SIZE] {};
    string.copyTo(buffer, VST3_STRING128_SIZE);

    std::wstring result;
    result.reserve(VST3_STRING128_SIZE);
    for (Steinberg::char16 character : buffer)
    {
        if (character == 0)
            break;
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}

void SetListViewItemTextWide(HWND listView, int itemIndex, int subItemIndex, const std::wstring& text)
{
    if (!listView || !::IsWindow(listView))
        return;

    LVITEMW item {};
    item.iItem = itemIndex;
    item.iSubItem = subItemIndex;
    item.mask = LVIF_TEXT;
    item.pszText = text.empty() ? const_cast<wchar_t*>(L"") : const_cast<wchar_t*>(text.c_str());

    ::SendMessageW(listView, LVM_SETITEMTEXTW, static_cast<WPARAM>(itemIndex),
                   reinterpret_cast<LPARAM>(&item));
}

float GetContentScaleForWindow(HWND hwnd)
{
    if (!hwnd)
        return 1.0f;

    float scale = 1.0f;

    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32)
    {
        using GetDpiForWindowProc = UINT(WINAPI*)(HWND);
        auto* getDpiForWindow = reinterpret_cast<GetDpiForWindowProc>(::GetProcAddress(user32, "GetDpiForWindow"));
        if (getDpiForWindow)
        {
            UINT dpi = getDpiForWindow(hwnd);
            if (dpi > 0)
                scale = static_cast<float>(dpi) / 96.0f;
        }
    }

    if (scale == 1.0f)
    {
        HDC dc = ::GetDC(hwnd);
        if (dc)
        {
            int dpiX = ::GetDeviceCaps(dc, LOGPIXELSX);
            ::ReleaseDC(hwnd, dc);
            if (dpiX > 0)
                scale = static_cast<float>(dpiX) / 96.0f;
        }
    }

    return std::max(scale, 1.0f);
}
} // namespace
#endif

namespace kj {

tresult PLUGIN_API VST3Host::HostApplication::queryInterface(const TUID _iid, void** obj)
{
    if (!obj)
        return kInvalidArgument;

    *obj = nullptr;
    if (std::memcmp(_iid, Steinberg::Vst::IHostApplication::iid, sizeof(TUID)) == 0)
        *obj = static_cast<Steinberg::Vst::IHostApplication*>(this);
    else if (std::memcmp(_iid, Steinberg::FUnknown::iid, sizeof(TUID)) == 0)
        *obj = static_cast<Steinberg::FUnknown*>(static_cast<Steinberg::Vst::IHostApplication*>(this));

    if (*obj)
    {
        addRef();
        return kResultOk;
    }

    return kNoInterface;
}

uint32 PLUGIN_API VST3Host::HostApplication::addRef()
{
    return ++refCount_;
}

uint32 PLUGIN_API VST3Host::HostApplication::release()
{
    return --refCount_;
}

tresult PLUGIN_API VST3Host::HostApplication::getName(Steinberg::Vst::String128 name)
{
    if (!name)
        return kInvalidArgument;

    Steinberg::UString hostName(name, VST3_STRING128_SIZE);
    hostName.fromAscii("KJ Host");
    return kResultOk;
}

tresult PLUGIN_API VST3Host::HostApplication::createInstance(Steinberg::TUID cid, Steinberg::TUID iid, void** obj)
{
    (void)cid;
    (void)iid;

    if (obj)
        *obj = nullptr;

    return kNoInterface;
}

class VST3Host::ComponentHandler : public Steinberg::Vst::IComponentHandler, public Steinberg::Vst::IComponentHandler2
{
public:
    explicit ComponentHandler(VST3Host& host) : host_(host) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;

        *obj = nullptr;
        if (std::memcmp(iid, IComponentHandler2::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<IComponentHandler2*>(this);
            addRef();
            return kResultOk;
        }
        if (std::memcmp(iid, IComponentHandler::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<IComponentHandler*>(this);
            addRef();
            return kResultOk;
        }
        if (std::memcmp(iid, FUnknown::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<FUnknown*>(static_cast<IComponentHandler*>(this));
            addRef();
            return kResultOk;
        }
        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override
    {
        return ++refCount_;
    }

    uint32 PLUGIN_API release() override
    {
        uint32 newCount = --refCount_;
        if (newCount == 0)
            delete this;
        return newCount;
    }

    tresult PLUGIN_API beginEdit(Steinberg::Vst::ParamID paramId) override
    {
        (void)paramId;
        return kResultOk;
    }

    tresult PLUGIN_API performEdit(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue value) override
    {
        host_.onControllerParameterChanged(paramId, value);
        return kResultOk;
    }

    tresult PLUGIN_API endEdit(Steinberg::Vst::ParamID paramId) override
    {
        (void)paramId;
        return kResultOk;
    }

    tresult PLUGIN_API restartComponent(Steinberg::int32 flags) override
    {
        host_.onRestartComponent(flags);
        return kResultOk;
    }

    tresult PLUGIN_API setDirty(Steinberg::TBool state) override
    {
        (void)state;
        return kResultOk;
    }

    tresult PLUGIN_API requestOpenEditor(Steinberg::FIDString name) override
    {
        host_.onComponentRequestOpenEditor(name);
        return kResultOk;
    }

    tresult PLUGIN_API startGroupEdit() override
    {
        return kResultOk;
    }

    tresult PLUGIN_API finishGroupEdit() override
    {
        return kResultOk;
    }

private:
    std::atomic<uint32> refCount_ {1};
    VST3Host& host_;
};

class VST3Host::NonRealtimeScope
{
public:
    explicit NonRealtimeScope(VST3Host& host) : host_(host)
    {
        host_.suspendProcessing();
        lock_ = std::unique_lock<std::mutex>(host_.processMutex_);
    }

    ~NonRealtimeScope()
    {
        lock_.unlock();
        host_.resumeProcessing();
    }

    NonRealtimeScope(const NonRealtimeScope&) = delete;
    NonRealtimeScope& operator=(const NonRealtimeScope&) = delete;

private:
    VST3Host& host_;
    std::unique_lock<std::mutex> lock_;
};

VST3Host::~VST3Host()
{
    unload();
}

void VST3Host::suspendProcessing()
{
    processingSuspended_.store(true, std::memory_order_release);
    waitForProcessingToComplete();
}

void VST3Host::resumeProcessing()
{
    processingSuspended_.store(false, std::memory_order_release);
}

void VST3Host::waitForProcessingToComplete()
{
    while (activeProcessCount_.load(std::memory_order_acquire) != 0)
        std::this_thread::yield();
}

bool VST3Host::waitForPluginReady()
{
    std::unique_lock<std::mutex> lock(loadingMutex_);
    constexpr auto kLoadTimeout = std::chrono::seconds(10);
    if (!loadingCv_.wait_for(lock, kLoadTimeout, [this] { return !loadingInProgress_; }))
    {
        std::cerr << "[KJ] Timed out while waiting for the plug-in to finish loading.\n";
        return false;
    }

    if (!pluginReady_)
        std::cerr << "[KJ] Plug-in failed to finish loading successfully.\n";

    return pluginReady_;
}

void VST3Host::markLoadStarted()
{
    // This synchronization is allowed because plugin loading is non-real-time.
    std::lock_guard<std::mutex> lock(loadingMutex_);
    loadingInProgress_ = true;
    pluginReady_ = false;
}

void VST3Host::markLoadFinished(bool success)
{
    // This synchronization is allowed because plugin loading is non-real-time.
    {
        std::lock_guard<std::mutex> lock(loadingMutex_);
        loadingInProgress_ = false;
        pluginReady_ = success;
    }
    loadingCv_.notify_all();
}

bool VST3Host::load(const std::string& pluginPath)
{
    markLoadStarted();
    bool finished = false;
    const auto finish = [this, &finished](bool success) {
        markLoadFinished(success);
        finished = true;
        return success;
    };

    try
    {
        NonRealtimeScope scope(*this);
        unloadLocked();

        std::string error;

#ifdef _WIN32
        pluginPath_.clear();
        pluginNameW_.clear();
        pluginVendorW_.clear();
        fallbackParameters_.clear();
        fallbackVisible_ = false;
        fallbackSelectedIndex_ = -1;
        resetFallbackEditState();
#endif

        auto resolvedPath = resolveVST3BundlePath(std::filesystem::u8path(pluginPath));
        auto resolvedPathStr = resolvedPath.u8string();
        std::cout << "[VST3] Resolved bundle path: " << resolvedPathStr << std::endl;

        auto module = Module::create(resolvedPathStr, error);
        if (!module)
        {
            std::cerr << "Failed to load plugin: " << error << std::endl;
            return finish(false);
        }

        auto factory = module->getFactory();
        auto factory3 = Steinberg::FUnknownPtr<Steinberg::IPluginFactory3>(factory.get());
        if (!factory3)
        {
            std::cerr << "[KJ] Plugin does not expose IPluginFactory3." << std::endl;
            return finish(false);
        }

        auto classes = factory.classInfos();
        const ClassInfo* componentClass = nullptr;
        const ClassInfo* controllerClass = nullptr;

        for (const auto& info : classes)
        {
            if (info.category() == kVstAudioEffectClass)
                componentClass = &info;
            else if (info.category() == kVstComponentControllerClass)
                controllerClass = &info;
        }

        if (!componentClass)
        {
            std::cerr << "No valid audio effect found in " << resolvedPathStr << std::endl;
            return finish(false);
        }

#ifdef _WIN32
        pluginPath_ = resolvedPath;
        pluginNameW_ = Utf8ToWide(componentClass->name());
        pluginVendorW_ = Utf8ToWide(componentClass->vendor());
#endif

        IComponent* rawComponent = nullptr;
        if (factory3->createInstance(componentClass->ID().data(), IComponent::iid,
                                     reinterpret_cast<void**>(&rawComponent)) != kResultOk ||
            !rawComponent)
        {
            std::cerr << "Failed to instantiate component: " << componentClass->name() << std::endl;
            return finish(false);
        }

        Steinberg::IPtr<Steinberg::Vst::IComponent> component = Steinberg::owned(rawComponent);

        if (component->initialize(&hostApplication_) != kResultOk)
        {
            std::cerr << "[KJ] Component initialization failed.\n";
            return finish(false);
        }

        TUID componentControllerTuid{};
        const tresult componentControllerResult =
            component->getControllerClassId(componentControllerTuid);
        const Steinberg::FUID componentControllerId =
            componentControllerResult == kResultOk
                ? Steinberg::FUID::fromTUID(componentControllerTuid)
                : Steinberg::FUID{};

        Steinberg::FUID enumeratedControllerId;
        if (controllerClass)
            enumeratedControllerId = Steinberg::FUID::fromTUID(controllerClass->ID().data());

        Steinberg::FUID controllerClassId =
            componentControllerId.isValid() ? componentControllerId : enumeratedControllerId;

        if (componentControllerId.isValid() && enumeratedControllerId.isValid() &&
            componentControllerId != enumeratedControllerId)
        {
            std::cerr << "[KJ] Component reports controller CID " << fuidToString(componentControllerId) <<
                " but factory enumerates " << fuidToString(enumeratedControllerId) << ".\n";
        }

        Steinberg::IPtr<Steinberg::Vst::IEditController> controller;
        if (controllerClassId.isValid())
        {
            IEditController* rawController = nullptr;
            if (factory3->createInstance(controllerClassId.toTUID(), IEditController::iid,
                                         reinterpret_cast<void**>(&rawController)) != kResultOk ||
                !rawController)
            {
                const auto controllerName = controllerClass ? controllerClass->name() : "<controller>";
                std::cerr << "Failed to instantiate controller: " << controllerName << std::endl;
                return finish(false);
            }

            controller = Steinberg::owned(rawController);
        }
        else
        {
            controller = Steinberg::FUnknownPtr<IEditController>(component);
        }

        if (!controller)
        {
            std::cerr << "Failed to acquire controller." << std::endl;
            return finish(false);
        }

        if (controller->initialize(&hostApplication_) != kResultOk)
        {
            std::cerr << "[KJ] Controller initialization failed.\n";
            return finish(false);
        }
        controllerInitialized_ = true;

        auto componentHandler = new ComponentHandler(*this);
        controller->setComponentHandler(static_cast<Steinberg::Vst::IComponentHandler*>(componentHandler));

        auto componentConnectionPoint =
            Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint>(component);
        auto controllerConnectionPoint =
            Steinberg::FUnknownPtr<Steinberg::Vst::IConnectionPoint>(controller);
        if (!componentConnectionPoint || !controllerConnectionPoint)
        {
            std::cerr << "[KJ] Plug-in does not expose IConnectionPoint on component/controller.\n";
            controller->setComponentHandler(nullptr);
            componentHandler->release();
            return finish(false);
        }

        if (componentConnectionPoint->connect(controllerConnectionPoint) != kResultOk ||
            controllerConnectionPoint->connect(componentConnectionPoint) != kResultOk)
        {
            std::cerr << "[KJ] Component/controller connection failed.\n";
            controller->setComponentHandler(nullptr);
            componentHandler->release();
            return finish(false);
        }

        {
            std::vector<uint8_t> componentState;
            VectorIBStream stateWriter(componentState);
            if (component->getState(&stateWriter) == kResultOk && !componentState.empty())
            {
                VectorIBStream stateReader(componentState.data(), componentState.size());
                controller->setComponentState(&stateReader);
            }
        }

        auto processor = Steinberg::FUnknownPtr<IAudioProcessor>(component);
        if (!processor)
        {
            std::cerr << "Component does not implement IAudioProcessor.\n";
            controller->setComponentHandler(nullptr);
            componentHandler->release();
            return finish(false);
        }

        module_ = module;
        component_ = component;
        processor_ = processor;
        controller_ = controller;
        view_ = nullptr;
        currentViewType_.clear();

        componentHandler_ = componentHandler;
        inputParameterChanges_.setMaxParameters(controller_->getParameterCount());

        parameterChangeQueue_.clear();
        eventQueue_.clear();
        processParameterChanges_.clear();
        processEvents_.clear();

#ifdef _WIN32
        refreshFallbackParameters();
        updateHeaderTexts();
#endif

        return finish(true);
    }
    catch (const std::exception& e)
    {
        std::cerr << "[KJ] Exception while loading plug-in: " << e.what() << "\n";
    }
    catch (...)
    {
        std::cerr << "[KJ] Unknown exception while loading plug-in.\n";
    }

    if (!finished)
        finish(false);

    return false;
}

bool VST3Host::prepare(double sampleRate, int blockSize)
{
    NonRealtimeScope scope(*this);
    if (!processor_)
        return false;

    processingActive_ = false;
    mainInputBusIndex_ = -1;
    mainOutputBusIndex_ = -1;
    inputArrangement_ = SpeakerArr::kEmpty;
    outputArrangement_ = SpeakerArr::kEmpty;

    const auto chooseArrangement = [](const Steinberg::Vst::BusInfo& info) {
        if (info.channelCount >= 2)
            return SpeakerArr::kStereo;
        if (info.channelCount == 1)
            return SpeakerArr::kMono;
        return SpeakerArr::kEmpty;
    };

    const auto findMainBus = [&](Steinberg::int32 busCount, Steinberg::Vst::BusDirection direction,
                                 std::vector<Steinberg::Vst::BusInfo>& infos) {
        Steinberg::int32 found = -1;
        infos.clear();
        infos.resize(static_cast<size_t>(busCount));
        for (Steinberg::int32 i = 0; i < busCount; ++i)
        {
            Steinberg::Vst::BusInfo info {};
            if (component_->getBusInfo(Steinberg::Vst::kAudio, direction, i, info) != kResultOk)
                continue;

            infos[static_cast<size_t>(i)] = info;

            if (info.busType == Steinberg::Vst::kMain)
                return i;

            if (found < 0)
                found = i;
        }
        return found;
    };

    const Steinberg::int32 inputBusCount = component_ ? component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput) : 0;
    const Steinberg::int32 outputBusCount = component_ ? component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput) : 0;

    std::vector<Steinberg::Vst::BusInfo> inputBusInfos;
    std::vector<Steinberg::Vst::BusInfo> outputBusInfos;

    if (component_)
    {
        mainInputBusIndex_ = findMainBus(inputBusCount, Steinberg::Vst::kInput, inputBusInfos);
        mainOutputBusIndex_ = findMainBus(outputBusCount, Steinberg::Vst::kOutput, outputBusInfos);
    }

    if (mainOutputBusIndex_ < 0 || mainOutputBusIndex_ >= outputBusCount)
        return false;

    std::vector<Steinberg::Vst::SpeakerArrangement> inputArrangements(static_cast<size_t>(inputBusCount), SpeakerArr::kEmpty);
    std::vector<Steinberg::Vst::SpeakerArrangement> outputArrangements(static_cast<size_t>(outputBusCount), SpeakerArr::kEmpty);

    if (mainInputBusIndex_ >= 0 && mainInputBusIndex_ < inputBusCount)
    {
        inputArrangement_ = chooseArrangement(inputBusInfos[static_cast<size_t>(mainInputBusIndex_)]);
        inputArrangements[static_cast<size_t>(mainInputBusIndex_)] = inputArrangement_;
    }

    outputArrangement_ = chooseArrangement(outputBusInfos[static_cast<size_t>(mainOutputBusIndex_)]);
    outputArrangements[static_cast<size_t>(mainOutputBusIndex_)] = outputArrangement_;

    const auto canProcess32 = processor_->canProcessSampleSize(kSample32);
    if (canProcess32 != kResultOk && canProcess32 != kResultTrue && canProcess32 != kNotImplemented)
        return false;

    if (component_)
    {
        if (component_->setActive(true) != kResultOk)
            return false;

        for (Steinberg::int32 i = 0; i < inputBusCount; ++i)
        {
            if (component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, i, true) != kResultOk)
                return false;
        }

        for (Steinberg::int32 i = 0; i < outputBusCount; ++i)
        {
            if (component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, i, true) != kResultOk)
                return false;
        }
    }

    const auto arrangementResult = processor_->setBusArrangements(inputArrangements.empty() ? nullptr : inputArrangements.data(),
                                                                  inputBusCount,
                                                                  outputArrangements.empty() ? nullptr : outputArrangements.data(),
                                                                  outputBusCount);
    if (arrangementResult != kResultOk && arrangementResult != kResultTrue && arrangementResult != kNotImplemented)
        return false;

    ProcessSetup setup {};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;

    auto result = processor_->setupProcessing(setup);
    if (result != kResultOk)
        return false;

    processor_->setProcessing(true);

    preparedSampleRate_ = sampleRate;
    preparedMaxBlockSize_ = blockSize;
    processContext_ = {};
    processContext_.sampleRate = sampleRate;
    processContext_.tempo = 120.0;
    processContext_.timeSigNumerator = 4;
    processContext_.timeSigDenominator = 4;
    processContext_.projectTimeSamples = 0;
    processContext_.continousTimeSamples = 0;
    processContext_.projectTimeMusic = 0.0;
    processContext_.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kContTimeValid |
                             ProcessContext::kProjectTimeMusicValid;

    const Steinberg::int32 maxInputChannels = SpeakerArr::getChannelCount(inputArrangement_);
    const Steinberg::int32 maxOutputChannels = SpeakerArr::getChannelCount(outputArrangement_);

    internalOut_.assign(static_cast<size_t>(maxOutputChannels), std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
    outputChannelPointers_.assign(static_cast<size_t>(maxOutputChannels), nullptr);

    if (mainInputBusIndex_ >= 0 && maxInputChannels > 0)
    {
        internalIn_.assign(static_cast<size_t>(maxInputChannels), std::vector<float>(static_cast<size_t>(blockSize), 0.0f));
        inputChannelPointers_.assign(static_cast<size_t>(maxInputChannels), nullptr);
    }
    else
    {
        internalIn_.clear();
        inputChannelPointers_.clear();
    }

    const size_t parameterQueueCapacity = std::max<size_t>(static_cast<size_t>(controller_->getParameterCount()) * 4, 64);
    parameterChangeQueue_.reset(parameterQueueCapacity);
    processParameterChanges_.clear();
    processParameterChanges_.reserve(parameterChangeQueue_.capacity());

    const size_t eventQueueCapacity = std::max<size_t>(static_cast<size_t>(blockSize) * 4, 128);
    eventQueue_.reset(eventQueueCapacity);
    processEvents_.clear();
    processEvents_.reserve(eventQueue_.capacity());

    processingActive_ = true;
    return true;
}

void VST3Host::queueParameterChange(ParamID paramId, ParamValue value, bool notifyController)
{
    if (paramId == kNoParamId || IsInvalidNormalizedValue(value))
        return;

    ParamValue clamped = std::clamp(value, 0.0, 1.0);

    if (notifyController && controller_)
    {
        Steinberg::FUnknownPtr<Steinberg::Vst::IEditControllerHostEditing> hostEditing(controller_);
        if (hostEditing)
            hostEditing->beginEditFromHost(paramId);

        controller_->setParamNormalized(paramId, clamped);

        if (hostEditing)
            hostEditing->endEditFromHost(paramId);
    }

    PendingParameterChange change {paramId, clamped};
    if (!parameterChangeQueue_.push(change))
        parameterChangeQueue_.pushOverwrite(change);
}

void VST3Host::queueEvent(const Steinberg::Vst::Event& ev)
{
    if (!eventQueue_.push(ev))
        eventQueue_.pushOverwrite(ev);
}

void VST3Host::queueNoteEvent(const Steinberg::Vst::Event& ev)
{
    queueEvent(ev);
}

void VST3Host::setTransportState(const HostTransportState& state)
{
    processContext_ = {};
    processContext_.sampleRate = preparedSampleRate_;
    processContext_.projectTimeSamples = static_cast<Steinberg::Vst::TSamples>(state.samplePosition);
    processContext_.continousTimeSamples = static_cast<Steinberg::Vst::TSamples>(state.samplePosition);
    if (preparedSampleRate_ > 0.0)
        processContext_.projectTimeMusic = (state.samplePosition / preparedSampleRate_) * (state.tempo / 60.0);
    processContext_.tempo = state.tempo;
    processContext_.timeSigNumerator = state.timeSigNum;
    processContext_.timeSigDenominator = state.timeSigDen;

    processContext_.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid;

    // The VST3 spec states projectTimeSamples is always valid, so there is no dedicated
    // state flag for it (kProjectTimeSamplesValid does not exist). Use the continuous time
    // flag to indicate the timeline position we provide.
    processContext_.state |= ProcessContext::kContTimeValid;
    if (state.playing)
        processContext_.state |= ProcessContext::kPlaying;
}

bool VST3Host::saveState(std::vector<uint8_t>& outState) const
{
    if (!component_ || !controller_)
        return false;

    std::vector<uint8_t> componentState;
    std::vector<uint8_t> controllerState;

    {
        VectorIBStream componentStream(componentState);
        if (component_->getState(&componentStream) != kResultOk)
            return false;
    }

    {
        VectorIBStream controllerStream(controllerState);
        if (controller_->getState(&controllerStream) != kResultOk)
            return false;
    }

    outState.clear();
    auto appendChunk = [&outState](const std::vector<uint8_t>& chunk) {
        uint32_t size = static_cast<uint32_t>(chunk.size());
        const uint8_t* sizeBytes = reinterpret_cast<const uint8_t*>(&size);
        outState.insert(outState.end(), sizeBytes, sizeBytes + sizeof(uint32_t));
        outState.insert(outState.end(), chunk.begin(), chunk.end());
    };

    appendChunk(componentState);
    appendChunk(controllerState);
    return true;
}

bool VST3Host::loadState(const uint8_t* data, size_t size)
{
    if (!component_ || !controller_ || !data || size == 0)
        return false;

    if (size < sizeof(uint32_t))
        return false;

    const uint8_t* cursor = data;
    size_t remaining = size;

    auto readChunk = [&cursor, &remaining](const uint8_t*& chunkData, size_t& chunkSize) -> bool {
        if (remaining < sizeof(uint32_t))
            return false;

        uint32_t declaredSize = 0;
        std::memcpy(&declaredSize, cursor, sizeof(uint32_t));
        cursor += sizeof(uint32_t);
        remaining -= sizeof(uint32_t);

        if (remaining < declaredSize)
            return false;

        chunkData = cursor;
        chunkSize = declaredSize;
        cursor += declaredSize;
        remaining -= declaredSize;
        return true;
    };

    const uint8_t* componentData = nullptr;
    size_t componentSize = 0;
    if (!readChunk(componentData, componentSize))
        return false;

    const uint8_t* controllerData = nullptr;
    size_t controllerSize = 0;
    if (!readChunk(controllerData, controllerSize))
        return false;

    {
        VectorIBStream componentStream(componentData, componentSize);
        if (component_->setState(&componentStream) != kResultOk)
            return false;
    }

    VectorIBStream controllerStream(controllerData, controllerSize);
    return controller_->setState(&controllerStream) == kResultOk;
}

void VST3Host::onControllerParameterChanged(ParamID paramId, ParamValue value)
{
    queueParameterChange(paramId, value, false);
}

void VST3Host::onRestartComponent(int32 flags)
{
    if ((flags & kParamValuesChanged) == 0)
        return;
}

#ifdef _WIN32

void VST3Host::resetFallbackEditState()
{
    fallbackVisible_ = false;
    fallbackSelectedIndex_ = -1;
}

void VST3Host::refreshFallbackParameters()
{
    fallbackParameters_.clear();

    if (!controller_)
        return;

    const int32 parameterCount = controller_->getParameterCount();
    fallbackParameters_.reserve(static_cast<size_t>(parameterCount));

    for (int32 i = 0; i < parameterCount; ++i)
    {
        FallbackParameter param;
        if (controller_->getParameterInfo(i, param.info) == kResultOk)
        {
            param.normalizedValue = controller_->getParamNormalized(param.info.id);
            fallbackParameters_.push_back(param);
        }
    }
}

void VST3Host::updateHeaderTexts()
{
    // Placeholder for updating any cached UI header text; currently handled elsewhere.
}

void VST3Host::destroyPluginUI()
{
    if (editorWindow_)
    {
        editorWindow_->close();
        editorWindow_.reset();
    }

    view_ = nullptr;
    currentViewType_.clear();
    resetFallbackEditState();
}

#endif // _WIN32

void VST3Host::onComponentRequestOpenEditor(const char* viewType)
{
    const char* requested = (viewType && *viewType) ? viewType : Steinberg::Vst::ViewType::kEditor;
    requestedViewType_ = requested;

#ifdef _WIN32
    HWND targetParent = nullptr;
    if (lastParentWindow_ && ::IsWindow(lastParentWindow_))
        targetParent = lastParentWindow_;

    openEditor(targetParent);
#else
    (void)requested;
#endif
}

bool VST3Host::createViewForRequestedType(const char* preferredType,
                                          Steinberg::IPtr<Steinberg::IPlugView>& outView,
                                          std::string& usedType,
                                          Steinberg::Vst::IEditController* controllerOverride)
{
    if (!controllerOverride && !controllerInitialized_)
        return false;

    Steinberg::IPtr<Steinberg::Vst::IEditController> controllerRef = controllerOverride ? controllerOverride : controller_;
    if (!controllerRef)
        return false;

    const char* fallbackType = Steinberg::Vst::ViewType::kEditor;
    usedType = preferredType && *preferredType ? preferredType : fallbackType;

#ifdef _WIN32
    ScopedCurrentDirectory resourceDirectory(ResolvePluginResourceDirectory(pluginPath_));
#endif

    Steinberg::IPtr<Steinberg::IPlugView> newView = controllerRef->createView(usedType.c_str());
    if (!newView && std::strcmp(usedType.c_str(), fallbackType) != 0)
    {
        // Fall back to the standard editor type if the plug-in rejected the requested view.
        newView = controllerRef->createView(fallbackType);
        usedType = fallbackType;
    }

    if (!newView)
        return false;

#ifdef _WIN32
    if (newView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
    {
        // Without HWND support we cannot host the native UI inside our Windows container.
        std::cerr << "[KJ] Plug-in editor does not support HWND platform windows." << std::endl;
        return false;
    }
#endif

    outView = newView;
    return true;
}

void VST3Host::process(float** outputs, int numChannels, int numSamples)
{
    process(nullptr, 0, outputs, numChannels, numSamples);
}

void VST3Host::process(float** inputs, int numInputChannels, float** outputs, int numOutputChannels, int numSamples)
{
    AudioProcessScope processScope(processingSuspended_, activeProcessCount_);
    if (!processScope.engaged())
        return;
    parameterChangeQueue_.popAll(processParameterChanges_);
    eventQueue_.popAll(processEvents_);

    processInternal(inputs, numInputChannels, outputs, numOutputChannels, numSamples, processParameterChanges_,
                    processEvents_);
}

void VST3Host::processInternal(float** inputs, int numInputChannels, float** outputs, int numOutputChannels, int numSamples,
                               const std::vector<PendingParameterChange>& changes,
                               const std::vector<Steinberg::Vst::Event>& events)
{
    if (outputs)
    {
        const auto samplesToClear = static_cast<size_t>(std::max(0, numSamples));
        for (int ch = 0; ch < numOutputChannels; ++ch)
        {
            if (outputs[ch])
                std::fill(outputs[ch], outputs[ch] + samplesToClear, 0.0f);
        }
    }

    if (!processor_ || !processingActive_ || !outputs || numSamples <= 0)
        return;

    inputParameterChanges_.clearQueue();
    for (const auto& change : changes)
    {
        if (change.id == kNoParamId)
            continue;

        int32 index = 0;
        if (auto* queue = inputParameterChanges_.addParameterData(change.id, index))
            queue->addPoint(0, change.value, index);
    }

    inputEventList_.clear();
    inputEventList_.setMaxSize(std::max<int32>(static_cast<int32>(events.size()), 1));
    for (auto ev : events)
    {
        inputEventList_.addEvent(ev);
    }

    const Steinberg::int32 expectedInputChannels = (mainInputBusIndex_ >= 0)
                                                       ? SpeakerArr::getChannelCount(inputArrangement_)
                                                       : 0;
    const Steinberg::int32 expectedOutputChannels = SpeakerArr::getChannelCount(outputArrangement_);

    AudioBusBuffers inputBuses[1] {};
    AudioBusBuffers outputBuses[1] {};

    Steinberg::int32 numInputBuses = 0;
    if (mainInputBusIndex_ >= 0 && inputs && expectedInputChannels > 0)
    {
        inputBuses[0].numChannels = std::min<Steinberg::int32>(numInputChannels, expectedInputChannels);
        inputBuses[0].channelBuffers32 = inputs;
        numInputBuses = (inputBuses[0].numChannels > 0) ? 1 : 0;
    }

    outputBuses[0].numChannels = std::min<Steinberg::int32>(numOutputChannels, expectedOutputChannels);
    outputBuses[0].channelBuffers32 = outputs;
    Steinberg::int32 numOutputBuses = (outputBuses[0].numChannels > 0) ? 1 : 0;

    ProcessData data {};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numSamples;
    data.numInputs = numInputBuses;
    data.numOutputs = numOutputBuses;
    data.inputs = (numInputBuses > 0) ? inputBuses : nullptr;
    data.outputs = (numOutputBuses > 0) ? outputBuses : nullptr;
    data.processContext = &processContext_;

    if (inputParameterChanges_.getParameterCount() > 0)
        data.inputParameterChanges = &inputParameterChanges_;
    if (inputEventList_.getEventCount() > 0)
        data.inputEvents = &inputEventList_;

    processor_->process(data);

    inputParameterChanges_.clearQueue();
    inputEventList_.clear();
}

void VST3Host::renderAudio(float** out, int numChannels, int numSamples)
{
    AudioProcessScope processScope(processingSuspended_, activeProcessCount_);
    if (!processScope.engaged())
        return;
    if (!out || numChannels <= 0 || numSamples <= 0)
        return;

    const Steinberg::int32 expectedOutputChannels = SpeakerArr::getChannelCount(outputArrangement_);
    const Steinberg::int32 expectedInputChannels = (mainInputBusIndex_ >= 0)
                                                       ? SpeakerArr::getChannelCount(inputArrangement_)
                                                       : 0;

    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (out[ch])
            std::fill(out[ch], out[ch] + static_cast<size_t>(numSamples), 0.0f);
    }

    if (!processor_ || !processingActive_ || expectedOutputChannels <= 0)
        return;

    if (static_cast<Steinberg::int32>(internalOut_.size()) < expectedOutputChannels)
        return;

    const Steinberg::int32 actualOutputChannels = std::min<Steinberg::int32>(expectedOutputChannels,
                                                                             static_cast<Steinberg::int32>(internalOut_.size()));
    const Steinberg::int32 actualInputChannels = std::min<Steinberg::int32>(expectedInputChannels,
                                                                            static_cast<Steinberg::int32>(internalIn_.size()));

    processParameterChanges_.clear();
    processEvents_.clear();
    parameterChangeQueue_.popAll(processParameterChanges_);
    eventQueue_.popAll(processEvents_);

    const int maxChunkSize = (preparedMaxBlockSize_ > 0) ? preparedMaxBlockSize_ : numSamples;
    static const std::vector<PendingParameterChange> kEmptyChanges;
    static const std::vector<Steinberg::Vst::Event> kEmptyEvents;

    bool automationApplied = false;
    int processed = 0;
    while (processed < numSamples)
    {
        const int remaining = numSamples - processed;
        const int chunkSize = std::min(remaining, maxChunkSize);

        for (Steinberg::int32 ch = 0; ch < actualOutputChannels; ++ch)
        {
            auto& buffer = internalOut_[static_cast<size_t>(ch)];
            const auto toClear = std::min<size_t>(buffer.size(), static_cast<size_t>(chunkSize));
            std::fill(buffer.begin(), buffer.begin() + toClear, 0.0f);
            outputChannelPointers_[static_cast<size_t>(ch)] = buffer.data();
        }

        float** inputBuffers = nullptr;
        if (actualInputChannels > 0 && !internalIn_.empty())
        {
            for (Steinberg::int32 ch = 0; ch < actualInputChannels; ++ch)
            {
                auto& buffer = internalIn_[static_cast<size_t>(ch)];
                const auto toClear = std::min<size_t>(buffer.size(), static_cast<size_t>(chunkSize));
                std::fill(buffer.begin(), buffer.begin() + toClear, 0.0f);
                inputChannelPointers_[static_cast<size_t>(ch)] = buffer.data();
            }
            inputBuffers = inputChannelPointers_.data();
        }

        const auto& chunkChanges = automationApplied ? kEmptyChanges : processParameterChanges_;
        const auto& chunkEvents = automationApplied ? kEmptyEvents : processEvents_;

        processInternal(inputBuffers, actualInputChannels, outputChannelPointers_.data(), actualOutputChannels, chunkSize,
                        chunkChanges, chunkEvents);
        automationApplied = true;

        for (int ch = 0; ch < numChannels && ch < actualOutputChannels; ++ch)
        {
            if (out[ch])
            {
                std::memcpy(out[ch] + processed, internalOut_[static_cast<size_t>(ch)].data(),
                            static_cast<size_t>(chunkSize) * sizeof(float));
            }
        }

        processed += chunkSize;
    }
}

void VST3Host::unload()
{
    markLoadStarted();
    NonRealtimeScope scope(*this);
    unloadLocked();
    markLoadFinished(false);
}

void VST3Host::unloadLocked()
{
#ifdef _WIN32
    destroyPluginUI();
#endif

    if (processor_)
        processor_->setProcessing(false);

    if (processor_ && component_)
    {
        const Steinberg::int32 inputBusCount = component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kInput);
        const Steinberg::int32 outputBusCount = component_->getBusCount(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput);

        std::vector<Steinberg::Vst::SpeakerArrangement> emptyInputs(static_cast<size_t>(inputBusCount), SpeakerArr::kEmpty);
        std::vector<Steinberg::Vst::SpeakerArrangement> emptyOutputs(static_cast<size_t>(outputBusCount), SpeakerArr::kEmpty);

        processor_->setBusArrangements(emptyInputs.empty() ? nullptr : emptyInputs.data(), inputBusCount,
                                       emptyOutputs.empty() ? nullptr : emptyOutputs.data(), outputBusCount);
    }
    if (component_)
        component_->setActive(false);

    processingActive_ = false;

    internalIn_.clear();
    internalOut_.clear();
    inputChannelPointers_.clear();
    outputChannelPointers_.clear();
    if (controller_ && componentHandler_)
        controller_->setComponentHandler(nullptr);

    view_ = nullptr;
    currentViewType_.clear();
    controllerInitialized_ = false;

    if (controller_)
        controller_->terminate();
    if (component_)
        component_->terminate();

    controller_ = nullptr;
    processor_ = nullptr;
    component_ = nullptr;
    module_ = nullptr;

    if (componentHandler_)
    {
        componentHandler_->release();
        componentHandler_ = nullptr;
    }

    preparedSampleRate_ = 0.0;
    preparedMaxBlockSize_ = 0;
    mainInputBusIndex_ = -1;
    mainOutputBusIndex_ = -1;
    inputArrangement_ = SpeakerArr::kEmpty;
    outputArrangement_ = SpeakerArr::kEmpty;
    processContext_ = {};

    inputParameterChanges_.setMaxParameters(0);
    inputParameterChanges_.clearQueue();
    inputEventList_.clear();
    processEvents_.clear();
    eventQueue_.clear();

    requestedViewType_ = Steinberg::Vst::ViewType::kEditor;

#ifdef _WIN32
    lastParentWindow_ = nullptr;
    viewHostWindow_ = nullptr;
    pluginPath_.clear();
#endif

    parameterChangeQueue_.clear();
    processParameterChanges_.clear();
}

bool VST3Host::isPluginLoaded() const
{
    return component_ != nullptr;
}

bool VST3Host::isPluginReady() const
{
    std::lock_guard<std::mutex> lock(loadingMutex_);
    return !loadingInProgress_ && pluginReady_;
}

bool VST3Host::isPluginLoading() const
{
    std::lock_guard<std::mutex> lock(loadingMutex_);
    return loadingInProgress_;
}

bool VST3Host::waitUntilReady()
{
    return waitForPluginReady();
}

#ifdef _WIN32

std::wstring VST3Host::getPluginDisplayName() const
{
    if (!pluginNameW_.empty())
        return pluginNameW_;
    return std::wstring(L"VST3 Plug-in");
}

bool VST3Host::createEditorViewOnGui(Steinberg::IPtr<Steinberg::IPlugView>& outView, Steinberg::ViewRect& rect)
{
    if (!controller_ || !controllerInitialized_)
        return false;

    const char* preferredType = requestedViewType_.empty() ? Steinberg::Vst::ViewType::kEditor
                                                           : requestedViewType_.c_str();

    std::string usedType;
    if (!createViewForRequestedType(preferredType, outView, usedType))
        return false;

    if (!outView || outView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
        return false;

    if (outView->getSize(&rect) != kResultTrue)
    {
        rect.left = 0;
        rect.top = 0;
        rect.right = 800;
        rect.bottom = 600;
    }

    currentViewType_ = usedType;
    return true;
}

bool VST3Host::resizePluginViewWindow(HWND window, const Steinberg::ViewRect& rect)
{
    if (!window || !::IsWindow(window))
        return false;

    const int width = std::max<int>(1, rect.getWidth());
    const int height = std::max<int>(1, rect.getHeight());

    RECT desired {0, 0, width, height};
    DWORD style = static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(::GetWindowLongPtrW(window, GWL_EXSTYLE));
    ::AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    return ::SetWindowPos(window, nullptr, 0, 0, desired.right - desired.left, desired.bottom - desired.top,
                          SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE) != FALSE;
}

void VST3Host::storeCurrentViewRect(const Steinberg::ViewRect& rect)
{
    currentViewRect_ = rect;
    hasCurrentViewRect_ = true;
}

void VST3Host::clearCurrentViewRect()
{
    currentViewRect_ = {};
    hasCurrentViewRect_ = false;
}

bool VST3Host::ShowPluginEditor()
{
    auto self = shared_from_this();
    VSTGuiThread::instance().post([self]() {
        if (!self || !self->controller_)
            return;

        if (!self->waitForPluginReady())
            return;

        if (!self->editorWindow_)
            self->editorWindow_ = VSTEditorWindow::create(self);

        if (self->editorWindow_)
            self->editorWindow_->show();
    });

    return true;
}

void VST3Host::showPluginUI(void* /*parentWindowHandle*/)
{
    ShowPluginEditor();
}

void VST3Host::openEditor(void* nativeWindowHandle)
{
    (void)nativeWindowHandle;
    ShowPluginEditor();
}

void VST3Host::asyncLoadPluginEditor(void* parentWindowHandle)
{
    lastParentWindow_ = reinterpret_cast<HWND>(parentWindowHandle);
    ShowPluginEditor();
}

#endif // _WIN32

} // namespace kj
