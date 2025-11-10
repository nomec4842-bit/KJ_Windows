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

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include <chrono>

#include "pluginterfaces/base/ipersistent.h"
#include "pluginterfaces/base/ustring.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"
#include "public.sdk/source/vst/vstcomponent.h"

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace {

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
constexpr wchar_t kStandaloneEditorWindowClassName[] = L"KJ_VST3_STANDALONE_EDITOR";
constexpr int kHeaderHeight = 56;
constexpr UINT_PTR kHeaderFallbackButtonId = 1001;
constexpr UINT_PTR kHeaderCloseButtonId = 1002;
constexpr UINT_PTR kFallbackListViewId = 2001;
constexpr UINT_PTR kFallbackSliderId = 2002;
constexpr int kFallbackSliderRange = 1000;
constexpr UINT kFallbackRefreshMessage = WM_APP + 101;

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

#ifdef _WIN32
class VST3Host::PlugFrame : public Steinberg::IPlugFrame
{
public:
    explicit PlugFrame(VST3Host& host) : host_(host) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;

        *obj = nullptr;

        if (std::memcmp(iid, IPlugFrame::iid, sizeof(TUID)) == 0 || std::memcmp(iid, FUnknown::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<IPlugFrame*>(this);
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

    tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override
    {
        if (!newSize)
            return kInvalidArgument;

        if (!host_.applyViewRect(*newSize))
            return kResultFalse;

        Steinberg::IPlugView* targetView = view ? view : host_.view_.get();
        if (targetView)
        {
            Steinberg::ViewRect currentSize;
            bool sizeChanged = true;
            if (targetView->getSize(&currentSize) == kResultTrue)
            {
                sizeChanged = currentSize.left != newSize->left || currentSize.top != newSize->top ||
                              currentSize.right != newSize->right || currentSize.bottom != newSize->bottom;
            }

            if (sizeChanged)
            {
                Steinberg::ViewRect newRect = *newSize;
                targetView->onSize(&newRect);
            }
        }

        return kResultOk;
    }

private:
    std::atomic<uint32> refCount_ {1};
    VST3Host& host_;
};
#endif

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

VST3Host::~VST3Host()
{
    unload();
}

bool VST3Host::load(const std::string& pluginPath)
{
    unload();

    std::string error;

#ifdef _WIN32
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
        return false;
    }

    auto factory = module->getFactory();
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
        return false;
    }

#ifdef _WIN32
    pluginNameW_ = Utf8ToWide(componentClass->name());
    pluginVendorW_ = Utf8ToWide(componentClass->vendor());
#endif

    auto component = factory.createInstance<IComponent>(componentClass->ID());
    if (!component)
    {
        std::cerr << "Failed to instantiate component: " << componentClass->name() << std::endl;
        return false;
    }

    Steinberg::FObject hostContext;
    if (component->initialize(&hostContext) != kResultOk)
    {
        std::cerr << "[KJ] Component initialization failed.\n";
        return false;
    }

    auto controller = controllerClass ? factory.createInstance<IEditController>(controllerClass->ID()) : nullptr;
    if (controller)
    {
        if (controller->initialize(&hostContext) != kResultOk)
        {
            std::cerr << "[KJ] Controller initialization failed.\n";
            return false;
        }
    }

    Steinberg::FUID controllerClassId;
    if (controller)
    {
        if (auto persistent = Steinberg::FUnknownPtr<Steinberg::IPersistent>(controller))
        {
            Steinberg::FUID::String classIdString {};
            if (persistent->getClassID(classIdString) == kResultOk)
            {
                controllerClassId.fromString(classIdString);
            }
        }
    }

    if (!controllerClassId.isValid() && controllerClass)
    {
        controllerClassId = Steinberg::FUID::fromTUID(controllerClass->ID().data());
    }

    if (auto componentImpl = Steinberg::FObject::fromUnknown<Steinberg::Vst::Component>(component))
    {
        if (controllerClassId.isValid())
            componentImpl->setControllerClass(controllerClassId);
    }

    auto processor = Steinberg::FUnknownPtr<IAudioProcessor>(component);
    if (!processor)
    {
        std::cerr << "Component does not implement IAudioProcessor.\n";
        return false;
    }

    module_ = module;
    component_ = component;
    processor_ = processor;
    controller_ = controller;
    view_ = nullptr;
    currentViewType_.clear();

    if (controller_)
    {
        componentHandler_ = new ComponentHandler(*this);
        controller_->setComponentHandler(static_cast<Steinberg::Vst::IComponentHandler*>(componentHandler_));
        inputParameterChanges_.setMaxParameters(controller_->getParameterCount());

        // Request the plug-in's editor view immediately so we know if a native GUI is available.
        if (!ensureViewForRequestedType())
            std::cerr << "[KJ] Plug-in does not provide a usable editor view; fallback UI will be used." << std::endl;
    }
    else
    {
        inputParameterChanges_.setMaxParameters(0);
    }

    {
        std::lock_guard<std::mutex> lock(parameterMutex_);
        pendingParameterChanges_.clear();
    }

#ifdef _WIN32
    refreshFallbackParameters();
    updateHeaderTexts();
#endif

    return true;
}

bool VST3Host::prepare(double sampleRate, int blockSize)
{
    if (!processor_)
        return false;

    if (component_)
        component_->setActive(true);

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
    processingActive_ = true;
    return true;
}

void VST3Host::queueParameterChange(ParamID paramId, ParamValue value)
{
    if (paramId == kNoParamId || IsInvalidNormalizedValue(value))
        return;

    ParamValue clamped = std::clamp(value, 0.0, 1.0);

    std::lock_guard<std::mutex> lock(parameterMutex_);
    auto it = std::find_if(pendingParameterChanges_.begin(), pendingParameterChanges_.end(),
                           [paramId](const PendingParameterChange& change) { return change.id == paramId; });
    if (it != pendingParameterChanges_.end())
    {
        it->value = clamped;
    }
    else
    {
        pendingParameterChanges_.push_back({paramId, clamped});
    }
}

void VST3Host::onControllerParameterChanged(ParamID paramId, ParamValue value)
{
    queueParameterChange(paramId, value);
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

    showPluginUI(targetParent);
#else
    (void)requested;
#endif
}

bool VST3Host::ensureViewForRequestedType()
{
    if (!controller_)
        return false;

    // Ask the controller for its native editor view using the requested type (defaulting to kEditor).
    const char* preferredType = requestedViewType_.empty() ? Steinberg::Vst::ViewType::kEditor
                                                           : requestedViewType_.c_str();
    const char* fallbackType = Steinberg::Vst::ViewType::kEditor;
    const char* usedType = preferredType;

    if (view_ && !currentViewType_.empty() && currentViewType_ == usedType)
        return true;

    if (view_ && viewAttached_)
        return true;

    view_ = nullptr;
    currentViewType_.clear();

    Steinberg::IPtr<Steinberg::IPlugView> newView = controller_->createView(preferredType);
    if (!newView && std::strcmp(preferredType, fallbackType) != 0)
    {
        // Fall back to the standard editor type if the plug-in rejected the requested view.
        newView = controller_->createView(fallbackType);
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

    view_ = newView;
    currentViewType_ = usedType;
    return true;
}

void VST3Host::process(float** outputs, int numChannels, int numSamples)
{
    if (!processor_ || !processingActive_ || !outputs)
        return;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        if (outputs[channel])
            std::fill(outputs[channel], outputs[channel] + numSamples, 0.0f);
    }

    std::vector<PendingParameterChange> changes;
    {
        std::lock_guard<std::mutex> lock(parameterMutex_);
        changes = pendingParameterChanges_;
        pendingParameterChanges_.clear();
    }

    inputParameterChanges_.clearQueue();
    for (const auto& change : changes)
    {
        if (change.id == kNoParamId)
            continue;

        int32 index = 0;
        if (auto* queue = inputParameterChanges_.addParameterData(change.id, index))
        {
            queue->addPoint(0, change.value, index);
        }
    }

    AudioBusBuffers outputBus {};
    outputBus.numChannels = numChannels;
    outputBus.channelBuffers32 = outputs;

    ProcessData data {};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numSamples;
    data.numOutputs = 1;
    data.outputs = &outputBus;
    if (inputParameterChanges_.getParameterCount() > 0)
        data.inputParameterChanges = &inputParameterChanges_;

    processor_->process(data);
}

void VST3Host::unload()
{
#ifdef _WIN32
    destroyPluginUI();
#endif

    if (processor_)
        processor_->setProcessing(false);
    if (component_)
        component_->setActive(false);

    processingActive_ = false;

    if (controller_ && componentHandler_)
        controller_->setComponentHandler(nullptr);

    view_ = nullptr;
    currentViewType_.clear();

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

    inputParameterChanges_.setMaxParameters(0);
    inputParameterChanges_.clearQueue();

    requestedViewType_ = Steinberg::Vst::ViewType::kEditor;

#ifdef _WIN32
    lastParentWindow_ = nullptr;
#endif

    std::lock_guard<std::mutex> lock(parameterMutex_);
    pendingParameterChanges_.clear();
}

bool VST3Host::isPluginLoaded() const
{
    return component_ != nullptr;
}

bool VST3Host::ShowPluginEditor()
{
#ifdef _WIN32
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
        Steinberg::IPtr<Steinberg::Vst::IEditController> controllerCopy = controller_;
        std::wstring windowTitle = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;

        standaloneEditorThreadShouldExit_.store(false);
        standaloneEditorThreadRunning_.store(true);

        standaloneEditorThread_ = std::thread([this, promise = std::move(startPromise), controllerCopy, windowTitle]() mutable {
            Steinberg::IPtr<Steinberg::IPlugView> localView;
            HWND hwnd = nullptr;
            bool attached = false;
            bool promiseSatisfied = false;

            auto cleanup = [this, &localView, &hwnd, &attached]() {
                if (attached && localView)
                    localView->removed();

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
                if (!controllerCopy)
                {
                    fail("[KJ] Plug-in controller is unavailable; cannot create editor view.");
                    return;
                }

                localView = controllerCopy->createView(Steinberg::Vst::ViewType::kEditor);
                if (!localView)
                {
                    fail("[KJ] Plug-in did not provide an editor view.");
                    return;
                }

                if (localView->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != kResultTrue)
                {
                    fail("[KJ] Plug-in editor does not support HWND platform windows.");
                    return;
                }

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

                if (localView->attached(reinterpret_cast<void*>(hwnd), Steinberg::kPlatformTypeHWND) != kResultOk)
                {
                    fail("[KJ] Failed to attach plug-in editor view to HWND.");
                    return;
                }

                attached = true;
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
    showPluginUI(hwnd);
}
void VST3Host::showPluginUI(void* parentHWND)
{
#ifdef _WIN32
    if (!component_ || !controller_)
    {
        std::cerr << "[KJ] Cannot show GUI before plugin is fully loaded.\n";
        return;
    }

    HWND parentWindow = reinterpret_cast<HWND>(parentHWND);
    if (parentWindow)
    {
        lastParentWindow_ = parentWindow;
    }
    else if (lastParentWindow_ && ::IsWindow(lastParentWindow_))
    {
        parentWindow = lastParentWindow_;
    }
    else
    {
        lastParentWindow_ = nullptr;
    }

    if (!createContainerWindow(parentWindow))
    {
        std::cerr << "[KJ] Failed to create container window for plug-in GUI.\n";
        return;
    }

    if (!ensureViewForRequestedType())
    {
        std::cerr << "[KJ] Plug-in has no usable editor view. Showing fallback controls.\n";
    }

    if (view_)
    {
        ensurePluginViewHost();

        if (!plugFrame_)
            plugFrame_ = new PlugFrame(*this);

        if (!pluginViewWindow_ || !::IsWindow(pluginViewWindow_))
        {
            // If we failed to allocate a container window we must fall back to the generic controls.
            std::cerr << "[KJ] Failed to create host window for VST3 editor. Showing fallback controls.\n";
            view_ = nullptr;
            frameAttached_ = false;
            viewAttached_ = false;
        }
        else
        {
            // Query the plug-in for its preferred editor dimensions and size our host window accordingly.
            Steinberg::ViewRect targetRect {};
            if (view_->getSize(&targetRect) != kResultTrue)
            {
                targetRect.left = 0;
                targetRect.top = 0;
                targetRect.right = 400;
                targetRect.bottom = 300;
            }

            applyViewRect(targetRect);

            if (!frameAttached_)
            {
                // Attach our IPlugFrame implementation before handing over the native window handle.
                plugFrame_->addRef();
                if (view_->setFrame(plugFrame_) != kResultOk)
                {
                    std::cerr << "[KJ] Failed to set VST3 plug frame. Falling back to generic controls.\n";
                    plugFrame_->release();
                    plugFrame_ = nullptr;
                    frameAttached_ = false;
                    view_ = nullptr;
                }
                else
                {
                    frameAttached_ = true;
                }
            }

            if (view_ && frameAttached_ && !viewAttached_ && pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
            {
                void* nativeHandle = reinterpret_cast<void*>(pluginViewWindow_);
                if (view_->attached(nativeHandle, Steinberg::kPlatformTypeHWND) != kResultOk)
                {
                    std::cerr << "[KJ] Failed to attach VST3 editor view. Falling back to generic controls.\n";
                    view_->setFrame(nullptr);
                    frameAttached_ = false;
                    if (plugFrame_)
                    {
                        plugFrame_->release();
                        plugFrame_ = nullptr;
                    }
                    view_ = nullptr;
                }
                else
                {
                    viewAttached_ = true;
                    // Notify the plug-in that the window is visible and provide the final size.
                    view_->onSize(&targetRect);

                    if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(view_))
                    {
                        float scale = GetContentScaleForWindow(pluginViewWindow_);
                        scaleSupport->setContentScaleFactor(scale);
                    }
                }
            }
        }
    }

    refreshFallbackParameters();
    showFallbackControls(fallbackVisible_);
    updateHeaderTexts();

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        ::ShowWindow(containerWindow_, SW_SHOWNORMAL);
        ::SetWindowPos(containerWindow_, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        ::UpdateWindow(containerWindow_);
        ::SetForegroundWindow(containerWindow_);
    }
    std::cout << "[KJ] Plugin GUI displayed.\n";
#else
    (void)parentHWND;
    std::cerr << "[KJ] Plugin UI is only supported on Windows.\\n";
#endif
}

#ifdef _WIN32

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

bool VST3Host::ensureWindowClasses()
{
    if (windowClassesRegistered_)
        return true;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);

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

    windowClassesRegistered_ = true;
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

    return containerWindow_ != nullptr;
}

void VST3Host::closeContainerWindow()
{
    if (containerWindow_ && ::IsWindow(containerWindow_))
        ::ShowWindow(containerWindow_, SW_HIDE);
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
}

void VST3Host::onContainerResized(int width, int height)
{
    if (headerWindow_ && ::IsWindow(headerWindow_))
        ::MoveWindow(headerWindow_, 0, 0, width, kHeaderHeight, TRUE);

    int contentHeight = std::max(0, height - kHeaderHeight);
    if (contentWindow_ && ::IsWindow(contentWindow_))
        ::MoveWindow(contentWindow_, 0, kHeaderHeight, width, contentHeight, TRUE);

    if (pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        ::MoveWindow(pluginViewWindow_, 0, 0, width, contentHeight, TRUE);

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
    pluginViewWindow_ = nullptr;
    fallbackWindow_ = nullptr;
    fallbackListView_ = nullptr;
    fallbackSlider_ = nullptr;
    fallbackValueStatic_ = nullptr;

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
void VST3Host::ensurePluginViewHost()
{
    if (!contentWindow_ || !::IsWindow(contentWindow_))
        return;

    if (!pluginViewWindow_)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        pluginViewWindow_ =
            ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                              0, 0, 0, 0, contentWindow_, nullptr, instance, nullptr);
    }

    if (pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        ::ShowWindow(pluginViewWindow_, SW_SHOWNORMAL);
}

bool VST3Host::applyViewRect(const Steinberg::ViewRect& rect)
{
    if (!pluginViewWindow_ || !::IsWindow(pluginViewWindow_))
        return false;

    const int width = std::max<int>(1, rect.getWidth());
    const int height = std::max<int>(1, rect.getHeight());

    BOOL moved = ::MoveWindow(pluginViewWindow_, 0, 0, width, height, TRUE);
    if (!moved)
    {
        moved = ::SetWindowPos(pluginViewWindow_, nullptr, 0, 0, width, height,
                               SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!moved)
        return false;

    updateWindowSizeForContent(width, height);
    return true;
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
    ensurePluginViewHost();

    bool shouldShowFallback = show || !view_;
    if (shouldShowFallback != fallbackVisible_)
        resetFallbackEditState();

    fallbackVisible_ = shouldShowFallback;

    if (fallbackWindow_ && ::IsWindow(fallbackWindow_))
        ::ShowWindow(fallbackWindow_, fallbackVisible_ ? SW_SHOW : SW_HIDE);

    if (pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        ::ShowWindow(pluginViewWindow_, (!fallbackVisible_ && view_) ? SW_SHOW : SW_HIDE);

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
            applyViewRect(rect);
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

    queueParameterChange(parameter.info.id, normalized);

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
    case WM_CLOSE:
        ::DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
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

    if (view_ && viewAttached_)
    {
        view_->removed();
        viewAttached_ = false;
    }

    if (view_ && frameAttached_)
    {
        view_->setFrame(nullptr);
        frameAttached_ = false;
    }

    if (plugFrame_)
    {
        plugFrame_->release();
        plugFrame_ = nullptr;
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
