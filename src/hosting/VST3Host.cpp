#include "hosting/VST3Host.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstring>

#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/base/ipersistent.h>
#include <public.sdk/source/vst/vstcomponent.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#define SMTG_OS_WINDOWS 1
#define SMTG_PLATFORM_WINDOWS 1
#define SMTG_EXPORT_MODULE_ENTRY 1

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

#ifdef _WIN32
namespace {
constexpr wchar_t kPluginChildWindowClassName[] = L"STATIC";
constexpr DWORD kPluginChildWindowStyle = WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
constexpr DWORD kPluginChildWindowExStyle = WS_EX_NOPARENTNOTIFY;
}
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

        if (!host_.childWindow_ || !::IsWindow(host_.childWindow_))
            return kResultFalse;

        const int width = std::max<int>(1, newSize->getWidth());
        const int height = std::max<int>(1, newSize->getHeight());

        BOOL moved = ::MoveWindow(host_.childWindow_, newSize->left, newSize->top, width, height, TRUE);
        if (!moved)
        {
            moved = ::SetWindowPos(host_.childWindow_, nullptr, newSize->left, newSize->top, width, height,
                                   SWP_NOZORDER | SWP_NOACTIVATE);
        }

        if (!moved)
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

VST3Host::~VST3Host()
{
    unload();
}

bool VST3Host::load(const std::string& pluginPath)
{
    std::string error;
    auto module = Module::create(pluginPath, error);
    if (!module)
    {
        std::cerr << "Failed to load plugin: " << error << std::endl;
        return false;
    }

    auto factory = module->getFactory();
    auto classes = factory.classInfos();
    const VST3::Hosting::ClassInfo* componentClass = nullptr;
    const VST3::Hosting::ClassInfo* controllerClass = nullptr;

    for (const auto& c : classes)
    {
        if (c.category() == kVstAudioEffectClass)
            componentClass = &c;
        else if (c.category() == kVstComponentControllerClass)
            controllerClass = &c;
    }

    if (!componentClass)
    {
        std::cerr << "No valid audio effect found in " << pluginPath << std::endl;
        return false;
    }

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

    FUnknownPtr<IAudioProcessor> processor(component);
    if (processor)
        std::cout << "AudioProcessor loaded successfully\n";
    if (controller)
        std::cout << "EditController loaded successfully\n";

    #ifdef _WIN32
    destroyPluginUI();
    #endif

    module_ = module;
    component_ = component;
    processor_ = processor;
    controller_ = controller;
    view_ = nullptr;

    if (!controller_)
        std::cerr << "[KJ] Plugin has no controller class, skipping GUI.\n";

    return true;
}

bool VST3Host::prepare(double sampleRate, int blockSize)
{
    if (!processor_)
        return false;

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;

    auto result = processor_->setupProcessing(setup);
    if (result == kResultOk)
    {
        preparedSampleRate_ = sampleRate;
        preparedMaxBlockSize_ = blockSize;
        processingActive_ = true;
        return true;
    }

    return false;
}

void VST3Host::process(float** outputs, int numChannels, int numFrames)
{
    if (!processor_ || !processingActive_ || !outputs)
        return;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        if (outputs[channel])
            std::fill(outputs[channel], outputs[channel] + numFrames, 0.0f);
    }

    AudioBusBuffers outputBus{};
    outputBus.numChannels = numChannels;
    outputBus.channelBuffers32 = outputs;

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numFrames;
    data.numOutputs = 1;
    data.outputs = &outputBus;

    processor_->process(data);
}

void VST3Host::unload()
{
    #ifdef _WIN32
    destroyPluginUI();
    #endif

    view_ = nullptr;

    controller_ = nullptr;
    processor_ = nullptr;
    component_ = nullptr;
    module_ = nullptr;

    preparedSampleRate_ = 0.0;
    preparedMaxBlockSize_ = 0;
    processingActive_ = false;
}

bool VST3Host::isPluginLoaded() const
{
    return component_ != nullptr;
}

void VST3Host::openEditor(void* hwnd)
{
    showPluginUI(hwnd);
}

void VST3Host::showPluginUI(void* parentHWND)
{
#ifndef _WIN32
    (void)parentHWND;
    std::cerr << "[KJ] Plugin UI is only supported on Windows.\\n";
    return;
#else
    if (!controller_)
    {
        std::cerr << "[KJ] No controller available for plugin GUI.\n";
        return;
    }

    if (!view_)
    {
        auto view = controller_->createView("editor");
        if (!view)
        {
            std::cerr << "[KJ] Plugin has no editor view.\n";
            return;
        }

        view_ = view;
    }

    if (!component_ || !controller_)
    {
        std::cerr << "[KJ] Cannot show GUI before plugin is fully loaded.\n";
        return;
    }

    HWND parentWindow = reinterpret_cast<HWND>(parentHWND);
    if (!parentWindow)
    {
        std::cerr << "[KJ] No parent window provided for plugin GUI.\n";
        return;
    }

    ViewRect initialRect {};
    if (view_->getSize(&initialRect) != kResultTrue)
    {
        initialRect.left = 0;
        initialRect.top = 0;
        initialRect.right = 400;
        initialRect.bottom = 300;
    }

    if (!plugFrame_)
    {
        plugFrame_ = new PlugFrame(*this);
    }

    if (!childWindow_ || !::IsWindow(childWindow_))
    {
        if (frameAttached_ || viewAttached_ || plugFrame_)
        {
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

            if (view_ && viewAttached_)
            {
                view_->removed();
                viewAttached_ = false;
            }
        }

        childWindow_ = ::CreateWindowExW(kPluginChildWindowExStyle, kPluginChildWindowClassName, L"",
                                         kPluginChildWindowStyle, initialRect.left, initialRect.top,
                                         std::max<int>(initialRect.getWidth(), 1), std::max<int>(initialRect.getHeight(), 1),
                                         parentWindow, nullptr, ::GetModuleHandleW(nullptr), nullptr);

        if (!childWindow_)
        {
            std::cerr << "[KJ] Failed to create child window for plugin GUI.\n";
            return;
        }
    }
    else
    {
        ::SetParent(childWindow_, parentWindow);
    }

    if (!frameAttached_)
    {
        plugFrame_->addRef();
        if (view_->setFrame(plugFrame_) != kResultOk)
        {
            std::cerr << "[KJ] Failed to set VST3 plug frame.\n";
            plugFrame_->release();
            return;
        }
        frameAttached_ = true;
    }

    if (!viewAttached_)
    {
        if (view_->attached(childWindow_, Steinberg::kPlatformTypeHWND) != kResultOk)
        {
            std::cerr << "[KJ] Failed to attach VST3 editor view.\n";
            view_->setFrame(nullptr);
            frameAttached_ = false;
            plugFrame_->release();
            plugFrame_ = nullptr;
            if (childWindow_ && ::IsWindow(childWindow_))
            {
                ::DestroyWindow(childWindow_);
            }
            childWindow_ = nullptr;
            return;
        }

        viewAttached_ = true;
    }

    ViewRect appliedRect = initialRect;
    if (view_->getSize(&appliedRect) != kResultTrue)
        appliedRect = initialRect;

    plugFrame_->resizeView(view_, &appliedRect);

    ::ShowWindow(childWindow_, SW_SHOW);
    ::UpdateWindow(childWindow_);
    ::SetFocus(childWindow_);

    std::cout << "[KJ] Plugin GUI displayed.\n";
#endif
}

#ifdef _WIN32
void VST3Host::destroyPluginUI()
{
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

    if (view_ && viewAttached_)
    {
        view_->removed();
        viewAttached_ = false;
    }

    if (childWindow_ && ::IsWindow(childWindow_))
    {
        ::DestroyWindow(childWindow_);
    }
    childWindow_ = nullptr;
}
#endif

} // namespace kj
