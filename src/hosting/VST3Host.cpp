#include "hosting/VST3Host.h"
#include <iostream>
#include <filesystem>
#include <algorithm>

#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/base/ipersistent.h>
#include <public.sdk/source/vst/vstcomponent.h>

#include <windows.h>

#define SMTG_OS_WINDOWS 1
#define SMTG_PLATFORM_WINDOWS 1
#define SMTG_EXPORT_MODULE_ENTRY 1

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace kj {

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
    auto infos = factory.classInfos();

    for (auto& info : infos)
    {
        if (info.category() == kVstAudioEffectClass)
        {
            auto component = factory.createInstance<IComponent>(info.ID());
            if (!component)
            {
                std::cerr << "Failed to instantiate component: " << info.name() << std::endl;
                return false;
            }

            auto controller = factory.createInstance<IEditController>(info.ID());
            if (!controller)
            {
                std::cerr << "[KJ] Failed to create IEditController.\n";
                return false;
            }

            Steinberg::FObject hostContext;
            if (component->initialize(&hostContext) != kResultOk)
            {
                std::cerr << "[KJ] Component initialization failed.\n";
                return false;
            }

            if (controller->initialize(&hostContext) != kResultOk)
            {
                std::cerr << "[KJ] Controller initialization failed.\n";
                return false;
            }

            Steinberg::FUID controllerClassId;
            if (auto persistent = Steinberg::FUnknownPtr<Steinberg::IPersistent>(controller))
            {
                Steinberg::FUID::String classIdString {};
                if (persistent->getClassID(classIdString) == kResultOk)
                {
                    controllerClassId.fromString(classIdString);
                }
            }

            if (!controllerClassId.isValid())
            {
                controllerClassId = Steinberg::FUID::fromTUID(info.ID().data());
            }

            if (auto componentImpl = Steinberg::FObject::fromUnknown<Steinberg::Vst::Component>(component))
            {
                componentImpl->setControllerClass(controllerClassId);
            }

            FUnknownPtr<IAudioProcessor> processor(component);
            if (processor)
                std::cout << "AudioProcessor loaded successfully\n";
            if (controller)
                std::cout << "EditController loaded successfully\n";

            module_ = module;
            component_ = component;
            processor_ = processor;
            controller_ = controller;
            view_ = nullptr;

            return true;
        }
    }

    std::cerr << "No valid audio effect found in " << pluginPath << std::endl;
    return false;
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
    if (view_)
    {
        view_->removed();
        view_ = nullptr;
    }

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
    if (!controller_)
    {
        std::cerr << "[KJ] No controller available for plugin GUI.\n";
        return;
    }

    if (!view_)
    {
        view_ = controller_->createView(ViewType::kEditor);
        if (!view_)
        {
            std::cerr << "[KJ] Plugin has no GUI view.\n";
            return;
        }
    }

    HWND parentWindow = reinterpret_cast<HWND>(parentHWND);
    view_->attached(parentWindow, "HWND");

    ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue)
    {
        view_->onSize(&rect);
    }

    HWND hPlugin = static_cast<HWND>(parentWindow);
    ShowWindow(hPlugin, SW_SHOW);
    UpdateWindow(hPlugin);
    SetFocus(hPlugin);

    std::cout << "[KJ] Plugin GUI displayed.\n";
}

} // namespace kj
