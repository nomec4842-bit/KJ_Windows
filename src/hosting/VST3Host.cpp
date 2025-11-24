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

#include "hosting/VST3PlugFrame.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
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

// -----------------------------------------------------------------------------
// Dedicated VST editor window declarations
// -----------------------------------------------------------------------------
static LRESULT CALLBACK VSTEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
static const wchar_t* kVSTEditorClassName = L"KJ_VSTEDITOR";

struct VSTEditorState {
    kj::VST3Host* host = nullptr;
    Steinberg::IPlugView* view = nullptr;
    HWND parent = nullptr;
    bool attached = false;
};

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

#define pluginView_ view_.get()

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

#ifdef _WIN32
void VST3Host::setPlugFrame(PlugFrame* frame)
{
    if (plugFrame_ == frame)
        return;

    if (plugFrame_)
        plugFrame_->release();

    plugFrame_ = frame;
}
#endif

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

        auto module = Module::create(pluginPath, error);
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
            std::cerr << "No valid audio effect found in " << pluginPath << std::endl;
            return finish(false);
        }

#ifdef _WIN32
        pluginPath_ = std::filesystem::u8path(pluginPath);
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

        Steinberg::FObject hostContext;
        if (component->initialize(&hostContext) != kResultOk)
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

        if (controller->initialize(&hostContext) != kResultOk)
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

    if (component_)
    {
        component_->setActive(true);

        if (mainInputBusIndex_ >= 0)
            component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kInput, mainInputBusIndex_, true);
        component_->activateBus(Steinberg::Vst::kAudio, Steinberg::Vst::kOutput, mainOutputBusIndex_, true);
    }

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
#ifdef _WIN32
    syncFallbackParameterValue(paramId, value);
#endif
}

void VST3Host::onRestartComponent(int32 flags)
{
    if ((flags & kParamValuesChanged) == 0)
        return;

#ifdef _WIN32
    refreshFallbackParameters();
    updateFallbackSlider(false);
    updateFallbackValueLabel();
#endif
}

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

bool VST3Host::ensureViewForRequestedType()
{
    if (!controllerInitialized_ || !controller_)
        return false;

    // Ask the controller for its native editor view using the requested type (defaulting to kEditor).
    const char* preferredType = requestedViewType_.empty() ? Steinberg::Vst::ViewType::kEditor
                                                           : requestedViewType_.c_str();

    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        if (view_ && !currentViewType_.empty() && currentViewType_ == preferredType)
            return true;

        if (view_ && viewAttached_)
            return true;
    }

    Steinberg::IPtr<Steinberg::IPlugView> newView;
    std::string usedType;
    if (!createViewForRequestedType(preferredType, newView, usedType))
        return false;

    std::lock_guard<std::mutex> lock(viewMutex_);
    view_ = newView;
    currentViewType_ = usedType;
    viewAttached_ = false;
    frameAttached_ = false;
    return true;
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

bool VST3Host::ShowPluginEditor()
{
#ifdef _WIN32
    if (!waitForPluginReady())
    {
        std::cerr << "[KJ] Cannot open plug-in editor because the plug-in is not ready.\n";
        return false;
    }
    if (!controller_)
    {
        std::cerr << "[KJ] Cannot open plug-in editor without a valid controller.\n";
        return false;
    }

    {
        std::unique_lock<std::mutex> lock(standaloneEditorMutex_);
        if (standaloneEditorThreadRunning_.load())
        {
            HWND existing = standaloneEditorWindow_;
            if (existing && ::IsWindow(existing))
            {
                ::ShowWindow(existing, SW_SHOWNORMAL);
                ::UpdateWindow(existing);
            }
            return true;
        }

        std::thread staleThread;
        if (standaloneEditorThread_.joinable())
            staleThread = std::move(standaloneEditorThread_);

        lock.unlock();
        if (staleThread.joinable())
            staleThread.join();
        lock.lock();

        if (standaloneEditorThreadRunning_.load())
        {
            HWND existing = standaloneEditorWindow_;
            if (existing && ::IsWindow(existing))
            {
                ::ShowWindow(existing, SW_SHOWNORMAL);
                ::UpdateWindow(existing);
            }
            return true;
        }

        std::promise<bool> startPromise;
        auto startFuture = startPromise.get_future();
        std::wstring windowTitle = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;

        standaloneEditorThreadShouldExit_.store(false);
        standaloneEditorThreadRunning_.store(true);

        standaloneEditorThread_ = std::thread([this, promise = std::move(startPromise), windowTitle]() mutable {
            if (!ensureViewForRequestedType())
            {
                promise.set_value(false);
                standaloneEditorThreadRunning_.store(false);
                return;
            }

            Steinberg::IPtr<Steinberg::IPlugView> localView;
            {
                std::lock_guard<std::mutex> viewLock(viewMutex_);
                localView = view_;
                viewAttached_ = false;
                frameAttached_ = false;
            }

            if (!localView)
            {
                promise.set_value(false);
                standaloneEditorThreadRunning_.store(false);
                return;
            }

            if (localView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
            {
                std::cerr << "[KJ] Plug-in editor does not support HWND platform windows." << std::endl;
                promise.set_value(false);
                standaloneEditorThreadRunning_.store(false);
                return;
            }

            HWND hwnd = nullptr;
            bool attached = false;
            bool promiseSatisfied = false;

            auto cleanup = [this, &localView, &hwnd, &attached]() {
                if (attached && localView)
                    localView->removed();

                if (frameAttached_ && localView)
                {
                    localView->setFrame(nullptr);
                    frameAttached_ = false;
                }

                viewAttached_ = false;

                if (plugFrame_)
                {
                    plugFrame_->setActiveView(nullptr);
                    plugFrame_->setHostWindow(nullptr);
                    plugFrame_->clearCachedRect();
                }

                if (hwnd && ::IsWindow(hwnd))
                    ::DestroyWindow(hwnd);

                {
                    std::lock_guard<std::mutex> guard(standaloneEditorMutex_);
                    standaloneEditorWindow_ = nullptr;
                    standaloneEditorView_ = nullptr;
                }

                standaloneEditorThreadRunning_.store(false);
                standaloneEditorThreadShouldExit_.store(false);
            };

            auto fail = [&](const char* message) {
                if (message)
                    std::cerr << message << std::endl;
                if (!promiseSatisfied)
                {
                    promise.set_value(false);
                    promiseSatisfied = true;
                }
                cleanup();
            };

            try
            {
                Steinberg::ViewRect rect {};
                if (localView->getSize(&rect) != kResultTrue)
                {
                    rect.left = 0;
                    rect.top = 0;
                    rect.right = 640;
                    rect.bottom = 480;
                }

                const int width = std::max<int>(1, rect.getWidth());
                const int height = std::max<int>(1, rect.getHeight());

                static std::once_flag classFlag;
                static ATOM editorClassAtom = 0;
                std::call_once(classFlag, []() {
                    WNDCLASSEXW wc {};
                    wc.cbSize = sizeof(WNDCLASSEXW);
                    wc.style = CS_HREDRAW | CS_VREDRAW;
                    wc.lpfnWndProc = &VST3Host::StandaloneEditorWndProc;
                    wc.cbClsExtra = 0;
                    wc.cbWndExtra = 0;
                    wc.hInstance = ::GetModuleHandleW(nullptr);
                    wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
                    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
                    wc.lpszClassName = kStandaloneEditorWindowClassName;
                    editorClassAtom = ::RegisterClassExW(&wc);
                });

                if (!editorClassAtom)
                {
                    fail("[KJ] Failed to register stand-alone editor window class.");
                    return;
                }

                HINSTANCE instance = ::GetModuleHandleW(nullptr);
                hwnd = ::CreateWindowExW(0, kStandaloneEditorWindowClassName, windowTitle.c_str(),
                                         WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT,
                                         width, height, nullptr, nullptr, instance, this);
                if (!hwnd)
                {
                    fail("[KJ] Failed to create stand-alone editor window.");
                    return;
                }

                ::ShowWindow(hwnd, SW_SHOWNORMAL);
                ::UpdateWindow(hwnd);

                if (!plugFrame_)
                    plugFrame_ = new PlugFrame(*this);

                plugFrame_->setHostWindow(hwnd);
                plugFrame_->setActiveView(localView);
                plugFrame_->setCachedRect(rect);

                if (localView->setFrame(plugFrame_) != kResultOk)
                {
                    fail("[KJ] Failed to provide host frame to plug-in view.");
                    return;
                }
                frameAttached_ = true;

                if (localView->attached(reinterpret_cast<void*>(hwnd), Steinberg::kPlatformTypeHWND) != kResultOk)
                {
                    localView->setFrame(nullptr);
                    frameAttached_ = false;
                    plugFrame_->setActiveView(nullptr);
                    plugFrame_->setHostWindow(nullptr);
                    plugFrame_->clearCachedRect();
                    fail("[KJ] Failed to attach plug-in editor view to HWND.");
                    return;
                }

                attached = true;
                viewAttached_ = true;
                localView->onSize(&rect);

                {
                    std::lock_guard<std::mutex> guard(standaloneEditorMutex_);
                    standaloneEditorWindow_ = hwnd;
                    standaloneEditorView_ = localView;
                }

                if (!promiseSatisfied)
                {
                    promise.set_value(true);
                    promiseSatisfied = true;
                }

                MSG msg {};
                while (!standaloneEditorThreadShouldExit_.load())
                {
                    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
                    {
                        if (msg.message == WM_QUIT)
                        {
                            standaloneEditorThreadShouldExit_.store(true);
                            break;
                        }

                        ::TranslateMessage(&msg);
                        ::DispatchMessageW(&msg);
                    }

                    if (!::IsWindow(hwnd))
                        break;

                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }

                cleanup();
            }
            catch (...)
            {
                fail("[KJ] Unexpected exception while creating the plug-in editor view.");
            }
        });

        lock.unlock();

        bool started = startFuture.get();
        if (!started)
        {
            ClosePluginEditor();
            return false;
        }
        return true;
    }
#else
    std::cerr << "[KJ] Native plug-in editor is only supported on Windows.\\n";
    return false;
#endif
}

void VST3Host::openEditor(void* hwnd)
{
    asyncLoadPluginEditor(hwnd);
}

void VST3Host::asyncLoadPluginEditor(void* parentWindowHandle)
{
#ifdef _WIN32
    if (!isPluginReady())
    {
        std::cout << "[KJ] Cannot show plug-in UI because the plug-in did not finish loading.\n";
        return;
    }
    if (!component_ || !controller_)
    {
        std::cerr << "[KJ] Cannot show GUI before plugin is fully loaded.\n";
        return;
    }

    HWND parentWindow = reinterpret_cast<HWND>(parentWindowHandle);
    if (parentWindow)
        lastParentWindow_ = parentWindow;

    int trackId = owningTrackId_.load(std::memory_order_acquire);
    if (parentWindow && trackId > 0)
        ::PostMessageW(parentWindow, kShowVstEditorMessage, static_cast<WPARAM>(trackId), 0);
#else
    (void)parentWindowHandle;
    std::cerr << "[KJ] Plugin UI is only supported on Windows.\\n";
#endif
}

#ifdef _WIN32
// -----------------------------------------------------------------------------
// Creates the dedicated plugin editor window
// -----------------------------------------------------------------------------
HWND VST3Host::createEditorWindow(HWND parent)
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc   = VSTEditorWndProc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = kVSTEditorClassName;

    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kVSTEditorClassName,
        L"Plugin Editor",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE,
        CW_USEDEFAULT, CW_USEDEFAULT,
        800, 600,
        parent,
        nullptr,
        wc.hInstance,
        this
    );

    return hwnd;
}
void VST3Host::showPluginUI(void* parentWindowPtr)
{
    if (!controller_ || !component_ || !pluginView_)
    {
        std::cout << "[VST3Host] Editor cannot be shown: missing controller/view." << std::endl;
        return;
    }

    if (!pluginView_->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND))
    {
        std::cout << "[VST3Host] Plugin does not support HWND platform." << std::endl;
        return;
    }

    HWND parentHwnd = reinterpret_cast<HWND>(parentWindowPtr);

    if (!editorWindow_)
    {
        editorWindow_ = createEditorWindow(parentHwnd);
    }

    ShowWindow(editorWindow_, SW_SHOW);
    SetForegroundWindow(editorWindow_);
}
void VST3Host::ClosePluginEditor()
{
    std::thread threadToJoin;
    {
        std::unique_lock<std::mutex> lock(standaloneEditorMutex_);
        HWND window = standaloneEditorWindow_;
        if (standaloneEditorThreadRunning_.load())
        {
            standaloneEditorThreadShouldExit_.store(true);
            if (window && ::IsWindow(window))
                ::PostMessageW(window, WM_CLOSE, 0, 0);
        }

        if (standaloneEditorThread_.joinable())
            threadToJoin = std::move(standaloneEditorThread_);
    }

    if (threadToJoin.joinable())
        threadToJoin.join();
}

bool VST3Host::ensureEditorWindowClass()
{
    static std::once_flag classFlag;
    static ATOM editorClassAtom = 0;

    std::call_once(classFlag, []() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &VST3Host::PluginEditorWndProc;
        wc.cbClsExtra = 0;
        wc.cbWndExtra = 0;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kEditorWindowClassName;
        wc.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
        wc.hIconSm = wc.hIcon;

        editorClassAtom = ::RegisterClassExW(&wc);
    });

    return editorClassAtom != 0;
}

bool VST3Host::ensureWindowClasses()
{
    std::lock_guard<std::mutex> lock(gWindowClassMutex);
    if (gWindowClassesRegistered)
        return true;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW existingClass {};
    existingClass.cbSize = sizeof(existingClass);
    if (::GetClassInfoExW(instance, kContainerWindowClassName, &existingClass))
    {
        gWindowClassesRegistered = true;
        return true;
    }

    WNDCLASSEXW containerClass {};
    containerClass.cbSize = sizeof(containerClass);
    containerClass.style = CS_DBLCLKS;
    containerClass.lpfnWndProc = &VST3Host::ContainerWndProc;
    containerClass.cbClsExtra = 0;
    containerClass.cbWndExtra = sizeof(LONG_PTR);
    containerClass.hInstance = instance;
    containerClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    containerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    containerClass.lpszClassName = kContainerWindowClassName;
    containerClass.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    containerClass.hIconSm = containerClass.hIcon;

    if (!::RegisterClassExW(&containerClass))
        return false;

    WNDCLASSEXW headerClass = containerClass;
    headerClass.lpfnWndProc = &VST3Host::HeaderWndProc;
    headerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    headerClass.lpszClassName = kHeaderWindowClassName;
    headerClass.hIcon = nullptr;
    headerClass.hIconSm = nullptr;
    if (!::RegisterClassExW(&headerClass))
        return false;

    WNDCLASSEXW fallbackClass = containerClass;
    fallbackClass.lpfnWndProc = &VST3Host::FallbackWndProc;
    fallbackClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    fallbackClass.lpszClassName = kFallbackWindowClassName;
    fallbackClass.hIcon = nullptr;
    fallbackClass.hIconSm = nullptr;
    if (!::RegisterClassExW(&fallbackClass))
        return false;

    WNDCLASSEXW viewHostClass = containerClass;
    viewHostClass.lpfnWndProc = &VST3Host::PluginViewHostWndProc;
    viewHostClass.hbrBackground = nullptr;
    viewHostClass.lpszClassName = kPluginViewWindowClassName;
    viewHostClass.hIcon = nullptr;
    viewHostClass.hIconSm = nullptr;
    if (!::RegisterClassExW(&viewHostClass))
        return false;

    gWindowClassesRegistered = true;
    return true;
}

bool VST3Host::ensureCommonControls()
{
    INITCOMMONCONTROLSEX initControls {};
    initControls.dwSize = sizeof(initControls);
    initControls.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    return ::InitCommonControlsEx(&initControls) != FALSE;
}

bool VST3Host::createContainerWindow(HWND parentWindow)
{
    if (!ensureWindowClasses() || !ensureCommonControls())
        return false;

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        if (parentWindow)
            ::SetWindowLongPtrW(containerWindow_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parentWindow));
        return true;
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD exStyle = WS_EX_TOOLWINDOW;

    int defaultWidth = 520;
    int defaultHeight = 420;
    RECT desired {0, 0, defaultWidth, defaultHeight};
    ::AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    std::wstring title = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;

    containerWindow_ = ::CreateWindowExW(exStyle, kContainerWindowClassName, title.c_str(), style, CW_USEDEFAULT,
                                         CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
                                         parentWindow, nullptr, instance, this);

    if (containerWindow_)
        ::PostMessageW(containerWindow_, WM_KJ_OPENEDITOR, 0, 0);

    return containerWindow_ != nullptr;
}

void VST3Host::closeContainerWindow()
{
    if (containerWindow_ && ::IsWindow(containerWindow_))
        ::ShowWindow(containerWindow_, SW_HIDE);
}

void VST3Host::onOpenEditorMessage(HWND hwnd)
{
    if (!containerWindow_)
        containerWindow_ = hwnd;

    if (!isPluginReady())
    {
        std::cerr << "[KJ] Ignoring editor open request because the plug-in is not ready.\n";
        return;
    }

    if (!controller_)
    {
        std::cerr << "[KJ] Cannot open plug-in editor without a controller.\n";
        return;
    }

    if (!ensureViewForRequestedType())
    {
        std::cerr << "[KJ] Plug-in has no usable editor view. Showing fallback controls.\n";
        showFallbackControls(true);
        return;
    }

    Steinberg::IPtr<Steinberg::IPlugView> viewCopy;
    {
        std::lock_guard<std::mutex> lock(viewMutex_);
        viewCopy = view_;
    }

    if (!viewCopy)
    {
        std::cerr << "[KJ] Failed to acquire plug-in view for editor attachment.\n";
        showFallbackControls(true);
        return;
    }

    if (viewCopy->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
    {
        std::cerr << "[KJ] Plug-in view does not support HWND embedding.\n";
        showFallbackControls(true);
        return;
    }

    HWND viewHostWindow = ensurePluginViewHost();
    if (!viewHostWindow || !::IsWindow(viewHostWindow))
    {
        std::cerr << "[KJ] Plug-in view host window is unavailable.\n";
        return;
    }

    Steinberg::ViewRect initialRect {};
    if (viewCopy->getSize(&initialRect) != kResultTrue)
    {
        initialRect.left = 0;
        initialRect.top = 0;
        initialRect.right = 800;
        initialRect.bottom = 600;
    }

    if (!plugFrame_)
        plugFrame_ = new PlugFrame(*this);

    plugFrame_->setHostWindow(viewHostWindow);
    plugFrame_->setActiveView(viewCopy);
    plugFrame_->setCachedRect(initialRect);

    viewAttached_ = false;
    frameAttached_ = false;

    auto failAttach = [&]() {
        if (frameAttached_ && viewCopy)
            viewCopy->setFrame(nullptr);
        frameAttached_ = false;
        viewAttached_ = false;
        plugFrame_->setActiveView(nullptr);
        plugFrame_->setHostWindow(nullptr);
        plugFrame_->clearCachedRect();
    };

    if (viewCopy->setFrame(plugFrame_) != kResultOk)
    {
        std::cerr << "[KJ] Failed to provide host frame to plug-in view.\n";
        failAttach();
        showFallbackControls(true);
        return;
    }

    frameAttached_ = true;

    resizePluginViewWindow(viewHostWindow, initialRect, true);

    if (viewCopy->attached(reinterpret_cast<void*>(viewHostWindow), Steinberg::kPlatformTypeHWND) != kResultOk)
    {
        std::cerr << "[KJ] Failed to attach plug-in editor view to HWND.\n";
        failAttach();
        showFallbackControls(true);
        return;
    }

    viewAttached_ = true;

    Steinberg::ViewRect notifyRect = initialRect;
    if (viewCopy->getSize(&notifyRect) == kResultTrue)
        plugFrame_->setCachedRect(notifyRect);

    resizePluginViewWindow(viewHostWindow, notifyRect, true);
    viewCopy->onSize(&notifyRect);
    storeCurrentViewRect(notifyRect);

    if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(viewCopy))
    {
        float scale = GetContentScaleForWindow(viewHostWindow);
        scaleSupport->setContentScaleFactor(scale);
    }

    ::ShowWindow(viewHostWindow, SW_SHOWNORMAL);
    ::UpdateWindow(viewHostWindow);
    ::SetFocus(viewHostWindow);

    showFallbackControls(false);
    updateHeaderTexts();
}

void VST3Host::onContainerCreated(HWND hwnd)
{
    containerWindow_ = hwnd;

    if (!headerFontsCreated_)
    {
        NONCLIENTMETRICSW metrics {sizeof(metrics)};
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            headerTextFont_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW titleFont = metrics.lfMessageFont;
            titleFont.lfWeight = FW_BOLD;
            headerTitleFont_ = ::CreateFontIndirectW(&titleFont);
            headerFontsCreated_ = true;
        }
        else
        {
            headerTextFont_ = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            headerTitleFont_ = headerTextFont_;
            headerFontsCreated_ = false;
        }
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    headerWindow_ = ::CreateWindowExW(0, kHeaderWindowClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, kHeaderHeight,
                                      hwnd, nullptr, instance, this);

    contentWindow_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                       0, kHeaderHeight, 0, 0, hwnd, nullptr, instance, nullptr);
    ::SetWindowLongPtrW(contentWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    ensurePluginViewHost();
    ensureFallbackWindow();
    updateHeaderTexts();

    if (idleTimerId_ == 0)
        idleTimerId_ = ::SetTimer(hwnd, kIdleTimerId, kIdleTimerIntervalMs, nullptr);
}

void VST3Host::onContainerResized(int width, int height)
{
    if (headerWindow_ && ::IsWindow(headerWindow_))
        ::MoveWindow(headerWindow_, 0, 0, width, kHeaderHeight, TRUE);

    int contentHeight = std::max(0, height - kHeaderHeight);
    if (contentWindow_ && ::IsWindow(contentWindow_))
        ::MoveWindow(contentWindow_, 0, kHeaderHeight, width, contentHeight, TRUE);

    if (viewHostWindow_ && ::IsWindow(viewHostWindow_))
        ::MoveWindow(viewHostWindow_, 0, 0, width, contentHeight, TRUE);

    if (fallbackWindow_ && ::IsWindow(fallbackWindow_))
        ::MoveWindow(fallbackWindow_, 0, 0, width, contentHeight, TRUE);

    if (view_ && viewAttached_)
    {
        // Inform the plug-in about the new drawable region whenever our host window resizes.
        Steinberg::ViewRect resizeRect {};
        resizeRect.left = 0;
        resizeRect.top = 0;
        resizeRect.right = width;
        resizeRect.bottom = contentHeight;
        view_->onSize(&resizeRect);
        storeCurrentViewRect(resizeRect);
    }
}

void VST3Host::onContainerDestroyed()
{
    containerWindow_ = nullptr;
    headerWindow_ = nullptr;
    headerTitleStatic_ = nullptr;
    headerVendorStatic_ = nullptr;
    headerStatusStatic_ = nullptr;
    headerFallbackButton_ = nullptr;
    headerCloseButton_ = nullptr;
    contentWindow_ = nullptr;
    viewHostWindow_ = nullptr;
    fallbackWindow_ = nullptr;
    fallbackListView_ = nullptr;
    fallbackSlider_ = nullptr;
    fallbackValueStatic_ = nullptr;

    HWND timerWindow = containerWindow_;
    if (idleTimerId_ != 0 && timerWindow && ::IsWindow(timerWindow))
    {
        ::KillTimer(timerWindow, idleTimerId_);
        idleTimerId_ = 0;
    }

    if (plugFrame_)
        plugFrame_->setHostWindow(nullptr);

    clearCurrentViewRect();

    if (headerFontsCreated_)
    {
        if (headerTitleFont_)
            ::DeleteObject(headerTitleFont_);
        if (headerTextFont_)
            ::DeleteObject(headerTextFont_);
        headerTitleFont_ = nullptr;
        headerTextFont_ = nullptr;
        headerFontsCreated_ = false;
    }

    fallbackVisible_ = false;
    fallbackSelectedIndex_ = -1;
    fallbackEditing_ = false;
    fallbackEditingParamId_ = 0;
}
HWND VST3Host::ensurePluginViewHost()
{
    if (!contentWindow_ || !::IsWindow(contentWindow_))
        return nullptr;

    if (viewHostWindow_ && !::IsWindow(viewHostWindow_))
        viewHostWindow_ = nullptr;

    // Lazily create a dedicated child HWND that will host the plug-in's native view.
    if (!viewHostWindow_)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        viewHostWindow_ =
            ::CreateWindowExW(WS_EX_NOPARENTNOTIFY, kPluginViewWindowClassName, L"",
                              WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS | WS_VISIBLE,
                              0, 0, 0, 0, contentWindow_, nullptr, instance, this);
    }

    if (viewHostWindow_ && ::IsWindow(viewHostWindow_))
    {
        ::ShowWindow(viewHostWindow_, SW_SHOWNORMAL);
        if (plugFrame_)
            plugFrame_->setHostWindow(viewHostWindow_);
    }

    return viewHostWindow_;
}

void VST3Host::onIdleTimer()
{
}

void VST3Host::cleanupEditorWindowResources()
{
    if (editorView_ && viewAttached_)
    {
        editorView_->removed();
        viewAttached_ = false;
    }

    if (editorView_ && frameAttached_)
    {
        editorView_->setFrame(nullptr);
        frameAttached_ = false;
    }

    if (plugFrame_)
    {
        plugFrame_->setActiveView(nullptr);
        plugFrame_->setHostWindow(nullptr);
        plugFrame_->clearCachedRect();
        plugFrame_->release();
        plugFrame_ = nullptr;
    }

    editorView_ = nullptr;
    editorWindow_ = nullptr;
    clearCurrentViewRect();
}

bool VST3Host::applyViewRect(HWND hostWindow, const Steinberg::ViewRect& rect)
{
    if (!hostWindow || !::IsWindow(hostWindow))
        return false;

    return resizePluginViewWindow(hostWindow, rect, true);
}

bool VST3Host::AttachView(Steinberg::IPlugView* view, HWND parentWindow)
{
    if (!view || !parentWindow || !::IsWindow(parentWindow))
        return false;

    viewAttached_ = false;
    frameAttached_ = false;

    // Ensure the plug-in understands how to embed itself into a native HWND container.
    if (view->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
        return false;

    if (!plugFrame_)
        plugFrame_ = new PlugFrame(*this);

    plugFrame_->setHostWindow(parentWindow);
    plugFrame_->setActiveView(view);

    Steinberg::ViewRect targetRect {};
    if (view->getSize(&targetRect) != kResultTrue)
    {
        targetRect.left = 0;
        targetRect.top = 0;
        targetRect.right = 800;
        targetRect.bottom = 600;
    }

    plugFrame_->setCachedRect(targetRect);

    if (view->setFrame(plugFrame_) != kResultOk)
    {
        plugFrame_->setActiveView(nullptr);
        plugFrame_->setHostWindow(nullptr);
        plugFrame_->clearCachedRect();
        return false;
    }

    frameAttached_ = true;

    resizePluginViewWindow(parentWindow, targetRect, true);

    // Hand the native HWND to the plug-in so it can create its controls as children of our window.
    if (view->attached(reinterpret_cast<void*>(parentWindow), Steinberg::kPlatformTypeHWND) != kResultOk)
    {
        view->setFrame(nullptr);
        frameAttached_ = false;
        plugFrame_->setActiveView(nullptr);
        plugFrame_->setHostWindow(nullptr);
        plugFrame_->clearCachedRect();
        return false;
    }

    viewAttached_ = true;

    Steinberg::ViewRect notifyRect {};
    if (view->getSize(&notifyRect) != kResultTrue)
        notifyRect = targetRect;

    plugFrame_->setCachedRect(notifyRect);

    resizePluginViewWindow(parentWindow, notifyRect, true);
    view->onSize(&notifyRect);

    if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(view))
    {
        float scale = GetContentScaleForWindow(parentWindow);
        scaleSupport->setContentScaleFactor(scale);
    }

    ::ShowWindow(parentWindow, SW_SHOWNORMAL);
    ::UpdateWindow(parentWindow);
    ::SetFocus(parentWindow);

    return true;
}

bool VST3Host::resizePluginViewWindow(HWND window, const Steinberg::ViewRect& rect, bool adjustContainer)
{
    const int width = std::max<int>(1, rect.getWidth());
    const int height = std::max<int>(1, rect.getHeight());

    bool moved = false;
    if (window && ::IsWindow(window))
    {
        // The plug-in lives in a child HWND so we honour resize requests with the standard Win32 APIs.
        moved = ::MoveWindow(window, 0, 0, width, height, TRUE) != FALSE;
        if (!moved)
        {
            moved = ::SetWindowPos(window, nullptr, 0, 0, width, height,
                                   SWP_NOZORDER | SWP_NOACTIVATE) != FALSE;
        }
    }

    if (moved && adjustContainer)
        updateWindowSizeForContent(width, height);

    storeCurrentViewRect(rect);
    return moved;
}

void VST3Host::storeCurrentViewRect(const Steinberg::ViewRect& rect)
{
    currentViewRect_ = rect;
    hasCurrentViewRect_ = true;
    if (plugFrame_)
        plugFrame_->setCachedRect(rect);
}

void VST3Host::clearCurrentViewRect()
{
    currentViewRect_ = {};
    hasCurrentViewRect_ = false;
    if (plugFrame_)
        plugFrame_->clearCachedRect();
}

void VST3Host::updateWindowSizeForContent(int contentWidth, int contentHeight)
{
    if (!containerWindow_ || !::IsWindow(containerWindow_))
        return;

    int width = std::max(contentWidth, 200);
    int height = std::max(contentHeight, 150);

    RECT clientRect {0, 0, width, height + kHeaderHeight};
    DWORD style = static_cast<DWORD>(::GetWindowLongW(containerWindow_, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(::GetWindowLongW(containerWindow_, GWL_EXSTYLE));
    ::AdjustWindowRectEx(&clientRect, style, FALSE, exStyle);

    ::SetWindowPos(containerWindow_, nullptr, 0, 0, clientRect.right - clientRect.left,
                   clientRect.bottom - clientRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void VST3Host::updateHeaderTexts()
{
    std::wstring title = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;
    if (containerWindow_ && ::IsWindow(containerWindow_))
        ::SetWindowTextW(containerWindow_, title.c_str());

    if (headerTitleStatic_ && ::IsWindow(headerTitleStatic_))
        ::SetWindowTextW(headerTitleStatic_, title.c_str());

    std::wstring vendorText = pluginVendorW_.empty() ? std::wstring(L"Vendor: Unknown")
                                                    : std::wstring(L"Vendor: ") + pluginVendorW_;
    if (headerVendorStatic_ && ::IsWindow(headerVendorStatic_))
        ::SetWindowTextW(headerVendorStatic_, vendorText.c_str());

    std::wstring status;
    if (!view_)
        status = L"Fallback controls (no custom editor)";
    else if (fallbackVisible_)
        status = L"Fallback controls active";
    else
        status = L"Custom editor active";

    if (headerStatusStatic_ && ::IsWindow(headerStatusStatic_))
        ::SetWindowTextW(headerStatusStatic_, status.c_str());

    if (headerFallbackButton_ && ::IsWindow(headerFallbackButton_))
    {
        if (view_)
        {
            std::wstring buttonText = fallbackVisible_ ? std::wstring(L"Show Editor")
                                                       : std::wstring(L"Show Fallback");
            ::SetWindowTextW(headerFallbackButton_, buttonText.c_str());
            ::EnableWindow(headerFallbackButton_, TRUE);
        }
        else
        {
            ::SetWindowTextW(headerFallbackButton_, L"Fallback Only");
            ::EnableWindow(headerFallbackButton_, FALSE);
        }
    }
}

void VST3Host::handleHeaderCommand(UINT commandId)
{
    switch (commandId)
    {
    case kHeaderFallbackButtonId:
        if (view_)
            showFallbackControls(!fallbackVisible_);
        break;
    case kHeaderCloseButtonId:
        closeContainerWindow();
        break;
    default:
        break;
    }
}

void VST3Host::showFallbackControls(bool show)
{
    ensureFallbackWindow();
    HWND hostWindow = ensurePluginViewHost();

    bool shouldShowFallback = show || !view_;
    if (shouldShowFallback != fallbackVisible_)
        resetFallbackEditState();

    fallbackVisible_ = shouldShowFallback;

    if (fallbackWindow_ && ::IsWindow(fallbackWindow_))
        ::ShowWindow(fallbackWindow_, fallbackVisible_ ? SW_SHOW : SW_HIDE);

    if (hostWindow && ::IsWindow(hostWindow))
        ::ShowWindow(hostWindow, (!fallbackVisible_ && view_) ? SW_SHOW : SW_HIDE);

    if (fallbackVisible_)
    {
        refreshFallbackParameters();
        updateFallbackSlider(false);
        updateFallbackValueLabel();
    }

    if (view_ && viewAttached_ && !fallbackVisible_)
    {
        ViewRect rect {};
        if (view_->getSize(&rect) == kResultTrue)
            applyViewRect(hostWindow, rect);
    }
    else if (fallbackVisible_)
    {
        updateWindowSizeForContent(480, 360);
    }

    updateHeaderTexts();
}

void VST3Host::ensureFallbackWindow()
{
    if (!contentWindow_ || !::IsWindow(contentWindow_))
        return;

    if (!fallbackWindow_)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        fallbackWindow_ = ::CreateWindowExW(0, kFallbackWindowClassName, L"", WS_CHILD,
                                            0, 0, 0, 0, contentWindow_, nullptr, instance, this);
    }
}

void VST3Host::refreshFallbackParameters()
{
    fallbackParameters_.clear();

    if (!controller_)
    {
        if (fallbackListView_ && ::IsWindow(fallbackListView_))
            ::SendMessageW(fallbackListView_, LVM_DELETEALLITEMS, 0, 0);
        return;
    }

    const int32 parameterCount = controller_->getParameterCount();
    fallbackParameters_.reserve(parameterCount);

    for (int32 index = 0; index < parameterCount; ++index)
    {
        Steinberg::Vst::ParameterInfo info {};
        if (controller_->getParameterInfo(index, info) != kResultOk)
            continue;

        if ((info.flags & ParameterInfo::kIsReadOnly) != 0)
            continue;

        FallbackParameter parameter {info, controller_->getParamNormalized(info.id)};
        fallbackParameters_.push_back(parameter);
    }

    if (!fallbackListView_ || !::IsWindow(fallbackListView_))
        return;

    ::SendMessageW(fallbackListView_, LVM_DELETEALLITEMS, 0, 0);

    LVITEMW item {};
    item.mask = LVIF_TEXT;

    int row = 0;
    for (const auto& parameter : fallbackParameters_)
    {
        std::wstring name = getParameterName(parameter);
        item.iItem = row;
        item.pszText = name.empty() ? const_cast<wchar_t*>(L"Parameter")
                                    : const_cast<wchar_t*>(name.c_str());
        int inserted = static_cast<int>(::SendMessageW(fallbackListView_, LVM_INSERTITEMW, 0,
                                                       reinterpret_cast<LPARAM>(&item)));
        if (inserted >= 0)
        {
            std::wstring value = getFallbackDisplayString(parameter);
            SetListViewItemTextWide(fallbackListView_, inserted, 1, value);
        }
        ++row;
    }
}
void VST3Host::onFallbackParameterSelected(int index)
{
    if (index < 0 || index >= static_cast<int>(fallbackParameters_.size()))
    {
        fallbackSelectedIndex_ = -1;
        if (fallbackSlider_ && ::IsWindow(fallbackSlider_))
            ::EnableWindow(fallbackSlider_, FALSE);
        updateFallbackValueLabel();
        resetFallbackEditState();
        return;
    }

    fallbackSelectedIndex_ = index;
    resetFallbackEditState();

    if (fallbackSlider_ && ::IsWindow(fallbackSlider_))
    {
        ::EnableWindow(fallbackSlider_, TRUE);
        ::SendMessageW(fallbackSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, kFallbackSliderRange));
        double normalized = std::clamp(fallbackParameters_[index].normalizedValue, 0.0, 1.0);
        LRESULT sliderPos = static_cast<LRESULT>(std::lround(normalized * kFallbackSliderRange));
        ::SendMessageW(fallbackSlider_, TBM_SETPOS, TRUE, sliderPos);
    }

    updateFallbackValueLabel();
}

void VST3Host::updateFallbackSlider(bool resetSelection)
{
    if (!fallbackListView_ || !::IsWindow(fallbackListView_))
        return;

    if (fallbackParameters_.empty())
    {
        ::EnableWindow(fallbackSlider_, FALSE);
        fallbackSelectedIndex_ = -1;
        updateFallbackValueLabel();
        return;
    }

    if (resetSelection || fallbackSelectedIndex_ < 0 ||
        fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
    {
        fallbackSelectedIndex_ = 0;
        ListView_SetItemState(fallbackListView_, fallbackSelectedIndex_, LVIS_SELECTED, LVIS_SELECTED);
        ListView_EnsureVisible(fallbackListView_, fallbackSelectedIndex_, FALSE);
    }

    onFallbackParameterSelected(fallbackSelectedIndex_);
}

void VST3Host::applyFallbackSliderChange(bool finalChange)
{
    if (!fallbackSlider_ || !::IsWindow(fallbackSlider_) || !controller_)
        return;

    if (fallbackSelectedIndex_ < 0 || fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
        return;

    auto& parameter = fallbackParameters_[fallbackSelectedIndex_];

    int sliderPosition = static_cast<int>(::SendMessageW(fallbackSlider_, TBM_GETPOS, 0, 0));
    double normalized = std::clamp(static_cast<double>(sliderPosition) / kFallbackSliderRange, 0.0, 1.0);

    if (!fallbackEditing_)
    {
        if (auto hostEditing = Steinberg::FUnknownPtr<Steinberg::Vst::IEditControllerHostEditing>(controller_))
            hostEditing->beginEditFromHost(parameter.info.id);
        fallbackEditing_ = true;
        fallbackEditingParamId_ = parameter.info.id;
    }

    controller_->setParamNormalized(parameter.info.id, normalized);

    queueParameterChange(parameter.info.id, normalized, false);

    parameter.normalizedValue = normalized;

    if (fallbackListView_ && ::IsWindow(fallbackListView_))
    {
        std::wstring value = getFallbackDisplayString(parameter);
        SetListViewItemTextWide(fallbackListView_, fallbackSelectedIndex_, 1, value);
    }

    updateFallbackValueLabel();

    if (finalChange && fallbackEditing_)
    {
        if (auto hostEditing = Steinberg::FUnknownPtr<Steinberg::Vst::IEditControllerHostEditing>(controller_))
            hostEditing->endEditFromHost(parameter.info.id);
        fallbackEditing_ = false;
        fallbackEditingParamId_ = 0;
    }
}

void VST3Host::updateFallbackValueLabel()
{
    if (!fallbackValueStatic_ || !::IsWindow(fallbackValueStatic_))
        return;

    if (fallbackSelectedIndex_ < 0 || fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
    {
        ::SetWindowTextW(fallbackValueStatic_, L"No editable parameters");
        return;
    }

    const auto& parameter = fallbackParameters_[fallbackSelectedIndex_];
    std::wstring label = getParameterName(parameter);
    std::wstring value = getFallbackDisplayString(parameter);
    std::wstring units = String128ToWide(parameter.info.units);
    if (!value.empty())
    {
        label += L": ";
        label += value;
        if (!units.empty())
        {
            label += L" ";
            label += units;
        }
    }
    ::SetWindowTextW(fallbackValueStatic_, label.c_str());
}

void VST3Host::resetFallbackEditState()
{
    if (fallbackEditing_ && controller_ && fallbackEditingParamId_ != 0)
    {
        if (auto hostEditing = Steinberg::FUnknownPtr<Steinberg::Vst::IEditControllerHostEditing>(controller_))
            hostEditing->endEditFromHost(fallbackEditingParamId_);
    }
    fallbackEditing_ = false;
    fallbackEditingParamId_ = 0;
}

std::wstring VST3Host::getFallbackDisplayString(const FallbackParameter& param) const
{
    if (!controller_)
        return {};

    Steinberg::Vst::String128 buffer {};
    if (controller_->getParamStringByValue(param.info.id, param.normalizedValue, buffer) == kResultTrue)
    {
        std::wstring text = String128ToWide(buffer);
        if (!text.empty())
            return text;
    }

    auto plainValue = controller_->normalizedParamToPlain(param.info.id, param.normalizedValue);
    if (!IsInvalidPlainValue(plainValue))
    {
        std::wstringstream stream;
        stream << std::fixed << std::setprecision(3) << plainValue;
        return stream.str();
    }

    std::wstringstream stream;
    stream << std::fixed << std::setprecision(3) << param.normalizedValue;
    return stream.str();
}

std::wstring VST3Host::getParameterName(const FallbackParameter& param) const
{
    std::wstring name = String128ToWide(param.info.title);
    if (!name.empty())
        return name;

    name = String128ToWide(param.info.shortTitle);
    if (!name.empty())
        return name;

    std::wstringstream stream;
    stream << L"Param " << param.info.id;
    return stream.str();
}

void VST3Host::syncFallbackParameterValue(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue value)
{
    (void)paramId;
    if (!fallbackWindow_ || !::IsWindow(fallbackWindow_))
        return;

    if (IsInvalidNormalizedValue(value))
        return;

    ::PostMessageW(fallbackWindow_, kFallbackRefreshMessage, 0, 0);
}

namespace
{

constexpr int16_t kShiftKeyModifier = static_cast<int16_t>(1 << 0);
constexpr int16_t kAlternateKeyModifier = static_cast<int16_t>(1 << 1);
constexpr int16_t kCommandKeyModifier = static_cast<int16_t>(1 << 2);
constexpr int16_t kControlKeyModifier = static_cast<int16_t>(1 << 3);

static_assert(kShiftKeyModifier == static_cast<int16_t>(Steinberg::kShiftKey),
              "Unexpected Steinberg::KeyModifier::kShiftKey value");
static_assert(kAlternateKeyModifier == static_cast<int16_t>(Steinberg::kAlternateKey),
              "Unexpected Steinberg::KeyModifier::kAlternateKey value");
static_assert(kCommandKeyModifier == static_cast<int16_t>(Steinberg::kCommandKey),
              "Unexpected Steinberg::KeyModifier::kCommandKey value");
static_assert(kControlKeyModifier == static_cast<int16_t>(Steinberg::kControlKey),
              "Unexpected Steinberg::KeyModifier::kControlKey value");

} // namespace

int16_t VST3Host::queryKeyModifiers() const
{
    int16_t modifiers = 0;
    if ((::GetKeyState(VK_SHIFT) & 0x8000) != 0)
        modifiers |= kShiftKeyModifier;
    if ((::GetKeyState(VK_CONTROL) & 0x8000) != 0)
        modifiers |= kCommandKeyModifier;
    if ((::GetKeyState(VK_MENU) & 0x8000) != 0)
        modifiers |= kAlternateKeyModifier;
    if ((::GetKeyState(VK_LWIN) & 0x8000) != 0 || (::GetKeyState(VK_RWIN) & 0x8000) != 0)
        modifiers |= kControlKeyModifier;
    return modifiers;
}

char16_t VST3Host::translateVirtualKey(WPARAM wParam, LPARAM lParam) const
{
    wchar_t buffer[4] = {};
    BYTE keyboardState[256];
    if (!::GetKeyboardState(keyboardState))
        return 0;

    UINT virtualKey = static_cast<UINT>(wParam);
    UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xFF);
    int length = ::ToUnicode(virtualKey, scanCode, keyboardState, buffer, 4, 0);
    if (length > 0)
        return static_cast<char16_t>(buffer[0]);
    return 0;
}

bool VST3Host::handleKeyDown(WPARAM wParam, LPARAM lParam)
{
    if (!view_ || !viewAttached_)
        return false;

    Steinberg::char16 character = static_cast<Steinberg::char16>(translateVirtualKey(wParam, lParam));
    Steinberg::int16 keyCode = static_cast<Steinberg::int16>(wParam);
    Steinberg::int16 modifiers = static_cast<Steinberg::int16>(queryKeyModifiers());
    return view_->onKeyDown(character, keyCode, modifiers) == kResultTrue;
}

bool VST3Host::handleKeyUp(WPARAM wParam, LPARAM lParam)
{
    if (!view_ || !viewAttached_)
        return false;

    Steinberg::char16 character = static_cast<Steinberg::char16>(translateVirtualKey(wParam, lParam));
    Steinberg::int16 keyCode = static_cast<Steinberg::int16>(wParam);
    Steinberg::int16 modifiers = static_cast<Steinberg::int16>(queryKeyModifiers());
    return view_->onKeyUp(character, keyCode, modifiers) == kResultTrue;
}

LRESULT CALLBACK VST3Host::PluginEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        if (create && create->lpCreateParams)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        ::SetTimer(hwnd, 1, 16, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == 1)
        {
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (host && host->editorView_)
        {
            Steinberg::ViewRect rect {};
            rect.left = 0;
            rect.top = 0;
            rect.right = static_cast<Steinberg::int32>(LOWORD(lParam));
            rect.bottom = static_cast<Steinberg::int32>(HIWORD(lParam));
            if (host->plugFrame_)
                host->plugFrame_->setCachedRect(rect);
            host->editorView_->onSize(&rect);
        }
        return 0;
    case WM_DESTROY:
        ::KillTimer(hwnd, 1);
        if (host)
            host->cleanupEditorWindowResources();
        return 0;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
            host->onContainerCreated(hwnd);
        return 0;
    case WM_KJ_OPENEDITOR:
        if (host)
            host->onOpenEditorMessage(hwnd);
        return 0;
    case WM_TIMER:
        if (host && wParam == kIdleTimerId)
        {
            host->onIdleTimer();
            return 0;
        }
        break;
    case WM_SIZE:
        if (host)
            host->onContainerResized(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;
    case WM_CLOSE:
        if (host)
        {
            host->closeContainerWindow();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (host)
            host->onContainerDestroyed();
        return 0;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::PluginViewHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        if (create && create->lpCreateParams)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    // Forward the relevant input and focus messages to the plug-in view so it can react to user gestures.
    switch (msg)
    {
    case WM_CREATE:
        ::SetTimer(hwnd, kViewRepaintTimerId, kViewRepaintIntervalMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kViewRepaintTimerId)
        {
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_MOUSEWHEEL:
        if (host && host->view_ && host->viewAttached_)
        {
            float delta = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / static_cast<float>(WHEEL_DELTA);
            host->view_->onWheel(delta);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_SIZE:
        if (host && host->view_ && host->viewAttached_)
        {
            Steinberg::ViewRect resizeRect {};
            resizeRect.left = 0;
            resizeRect.top = 0;
            resizeRect.right = static_cast<Steinberg::int32>(LOWORD(lParam));
            resizeRect.bottom = static_cast<Steinberg::int32>(HIWORD(lParam));
            host->storeCurrentViewRect(resizeRect);
            host->view_->onSize(&resizeRect);
            return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_MBUTTONDOWN:
    case WM_RBUTTONDOWN:
        ::SetFocus(hwnd);
        break;
    case WM_SETFOCUS:
        if (host && host->view_ && host->viewAttached_)
            host->view_->onFocus(static_cast<Steinberg::TBool>(true));
        return 0;
    case WM_KILLFOCUS:
        if (host && host->view_ && host->viewAttached_)
            host->view_->onFocus(static_cast<Steinberg::TBool>(false));
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (host && host->handleKeyDown(wParam, lParam))
            return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (host && host->handleKeyUp(wParam, lParam))
            return 0;
        break;
    case WM_ERASEBKGND:
        return 1;
    case WM_DESTROY:
        ::KillTimer(hwnd, kViewRepaintTimerId);
        break;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::HeaderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
        {
            host->headerWindow_ = hwnd;
            HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;

            host->headerTitleStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                         8, 6, 200, 20, hwnd, nullptr, instance, nullptr);
            host->headerVendorStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                          8, 26, 200, 18, hwnd, nullptr, instance, nullptr);
            host->headerStatusStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                          8, 44, 200, 14, hwnd, nullptr, instance, nullptr);

            host->headerFallbackButton_ = ::CreateWindowExW(0, L"BUTTON", L"",
                                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                             0, 0, 120, 26, hwnd,
                                                             reinterpret_cast<HMENU>(kHeaderFallbackButtonId),
                                                             instance, nullptr);
            host->headerCloseButton_ = ::CreateWindowExW(0, L"BUTTON", L"Close",
                                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                          0, 0, 80, 26, hwnd,
                                                          reinterpret_cast<HMENU>(kHeaderCloseButtonId),
                                                          instance, nullptr);

            HFONT titleFont = host->headerTitleFont_ ? host->headerTitleFont_
                                                      : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            HFONT textFont = host->headerTextFont_ ? host->headerTextFont_
                                                   : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));

            if (host->headerTitleStatic_)
                ::SendMessageW(host->headerTitleStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
            if (host->headerVendorStatic_)
                ::SendMessageW(host->headerVendorStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerStatusStatic_)
                ::SendMessageW(host->headerStatusStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerFallbackButton_)
                ::SendMessageW(host->headerFallbackButton_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerCloseButton_)
                ::SendMessageW(host->headerCloseButton_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);

            host->updateHeaderTexts();
        }
        return 0;

    case WM_SIZE:
        if (host)
        {
            int width = static_cast<int>(LOWORD(lParam));
            int margin = 8;
            int buttonHeight = 26;
            int closeWidth = 80;
            int toggleWidth = 130;
            int buttonTop = (kHeaderHeight - buttonHeight) / 2;

            if (host->headerCloseButton_ && ::IsWindow(host->headerCloseButton_))
            {
                ::MoveWindow(host->headerCloseButton_, width - margin - closeWidth, buttonTop,
                             closeWidth, buttonHeight, TRUE);
            }

            if (host->headerFallbackButton_ && ::IsWindow(host->headerFallbackButton_))
            {
                int fallbackLeft = width - margin - closeWidth - margin - toggleWidth;
                ::MoveWindow(host->headerFallbackButton_, fallbackLeft, buttonTop,
                             toggleWidth, buttonHeight, TRUE);
            }

            int textRight = width - margin - closeWidth - margin - toggleWidth - margin;
            if (textRight < margin + 10)
                textRight = margin + 10;

            int textWidth = textRight - margin;
            if (host->headerTitleStatic_ && ::IsWindow(host->headerTitleStatic_))
                ::MoveWindow(host->headerTitleStatic_, margin, 6, textWidth, 18, TRUE);
            if (host->headerVendorStatic_ && ::IsWindow(host->headerVendorStatic_))
                ::MoveWindow(host->headerVendorStatic_, margin, 26, textWidth, 16, TRUE);
            if (host->headerStatusStatic_ && ::IsWindow(host->headerStatusStatic_))
                ::MoveWindow(host->headerStatusStatic_, margin, 44, textWidth, 14, TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (host)
            host->handleHeaderCommand(LOWORD(wParam));
        return 0;

    case WM_DESTROY:
        if (host)
        {
            host->headerWindow_ = nullptr;
            host->headerTitleStatic_ = nullptr;
            host->headerVendorStatic_ = nullptr;
            host->headerStatusStatic_ = nullptr;
            host->headerFallbackButton_ = nullptr;
            host->headerCloseButton_ = nullptr;
        }
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
LRESULT CALLBACK VST3Host::FallbackWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
        {
            host->fallbackWindow_ = hwnd;
            HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;

            host->fallbackListView_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                                         0, 0, 0, 0, hwnd,
                                                         reinterpret_cast<HMENU>(kFallbackListViewId),
                                                         instance, nullptr);
            if (host->fallbackListView_)
            {
                ::SendMessageW(host->fallbackListView_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

                LVCOLUMNW column {};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.cx = 220;
                column.pszText = const_cast<wchar_t*>(L"Parameter");
                ::SendMessageW(host->fallbackListView_, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
                column.cx = 160;
                column.pszText = const_cast<wchar_t*>(L"Value");
                ::SendMessageW(host->fallbackListView_, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
            }

            host->fallbackSlider_ = ::CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                                       WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                                       0, 0, 0, 0, hwnd,
                                                       reinterpret_cast<HMENU>(kFallbackSliderId),
                                                       instance, nullptr);
            host->fallbackValueStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                            0, 0, 0, 0, hwnd, nullptr, instance, nullptr);

            HFONT textFont = host->headerTextFont_ ? host->headerTextFont_
                                                   : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            if (host->fallbackListView_)
                ::SendMessageW(host->fallbackListView_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->fallbackSlider_)
                ::SendMessageW(host->fallbackSlider_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->fallbackValueStatic_)
                ::SendMessageW(host->fallbackValueStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);

            host->refreshFallbackParameters();
            host->updateFallbackSlider(true);
            host->updateFallbackValueLabel();
        }
        return 0;

    case WM_SIZE:
        if (host)
        {
            int width = static_cast<int>(LOWORD(lParam));
            int height = static_cast<int>(HIWORD(lParam));
            int listHeight = std::max(0, height - 80);
            int sliderTop = listHeight + 8;

            if (host->fallbackListView_ && ::IsWindow(host->fallbackListView_))
                ::MoveWindow(host->fallbackListView_, 8, 8, width - 16, listHeight, TRUE);
            if (host->fallbackSlider_ && ::IsWindow(host->fallbackSlider_))
                ::MoveWindow(host->fallbackSlider_, 8, sliderTop, width - 16, 24, TRUE);
            if (host->fallbackValueStatic_ && ::IsWindow(host->fallbackValueStatic_))
                ::MoveWindow(host->fallbackValueStatic_, 8, sliderTop + 30, width - 16, 18, TRUE);
        }
        return 0;

    case WM_NOTIFY:
        if (host)
        {
            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header && header->idFrom == kFallbackListViewId && header->code == LVN_ITEMCHANGED)
            {
                auto* info = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if ((info->uChanged & LVIF_STATE) != 0 && (info->uNewState & LVIS_SELECTED) != 0)
                    host->onFallbackParameterSelected(info->iItem);
            }
        }
        return 0;

    case WM_HSCROLL:
        if (host && host->fallbackSlider_ && reinterpret_cast<HWND>(lParam) == host->fallbackSlider_)
        {
            const int scrollCode = LOWORD(wParam);
            bool finalChange = (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION);
            host->applyFallbackSliderChange(finalChange);
            return 0;
        }
        break;

    case kFallbackRefreshMessage:
        if (host)
        {
            host->refreshFallbackParameters();
            host->updateFallbackSlider(false);
            host->updateFallbackValueLabel();
        }
        return 0;

    case WM_DESTROY:
        if (host)
        {
            host->fallbackWindow_ = nullptr;
            host->fallbackListView_ = nullptr;
            host->fallbackSlider_ = nullptr;
            host->fallbackValueStatic_ = nullptr;
        }
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::StandaloneEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        if (create && create->lpCreateParams)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        ::SetTimer(hwnd, kViewRepaintTimerId, kViewRepaintIntervalMs, nullptr);
        return 0;
    case WM_TIMER:
        if (wParam == kViewRepaintTimerId)
        {
            ::InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }
        break;
    case WM_PAINT: {
        PAINTSTRUCT ps {};
        ::BeginPaint(hwnd, &ps);
        ::EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        ::KillTimer(hwnd, kViewRepaintTimerId);
        if (host)
            host->standaloneEditorThreadShouldExit_.store(true);
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

void VST3Host::destroyPluginUI()
{
    ClosePluginEditor();
    resetFallbackEditState();
    if (editorWindow_ && ::IsWindow(editorWindow_))
    {
        ::DestroyWindow(editorWindow_);
    }
    else
    {
        cleanupEditorWindowResources();
    }

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        ::DestroyWindow(containerWindow_);
    }
    else
    {
        onContainerDestroyed();
    }

    fallbackVisible_ = false;
}

#endif // _WIN32

} // namespace kj

#ifdef _WIN32
// -----------------------------------------------------------------------------
// Dedicated VST Editor Window WndProc
// -----------------------------------------------------------------------------
static LRESULT CALLBACK VSTEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    static VSTEditorState state;

    switch (msg)
    {
    case WM_CREATE:
    {
        auto cs = reinterpret_cast<LPCREATESTRUCT>(lParam);
        state.host = reinterpret_cast<kj::VST3Host*>(cs->lpCreateParams);

        if (!state.host)
            return -1;

        state.view = state.host->getView();
        state.parent = hwnd;

        if (state.view) {
            Steinberg::ViewRect rect = {0, 0, 800, 600};
            state.view->attached(state.parent, Steinberg::kPlatformTypeHWND);
            state.view->onSize(&rect);
            state.attached = true;
        }
        return 0;
    }

    case WM_SIZE:
        if (state.view && state.attached) {
            RECT rc;
            GetClientRect(hwnd, &rc);
            Steinberg::ViewRect r{rc.left, rc.top, rc.right, rc.bottom};
            state.view->onSize(&r);
        }
        return 0;

    case WM_CLOSE:
        if (state.view && state.attached) {
            state.view->removed();
            state.attached = false;
        }
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif // _WIN32
