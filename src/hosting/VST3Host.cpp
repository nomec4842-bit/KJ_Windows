#include "hosting/VST3Host.h"
#include <iostream>
#include <filesystem>
#include <algorithm>

#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/gui/iplugview.h>

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

            FUnknownPtr<IAudioProcessor> processor(component);
            FUnknownPtr<IEditController> controller(component);
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
    if (!view_)
        return;

    view_->attached(hwnd, "HWND");

    ViewRect rect{};
    if (view_->getSize(&rect) == kResultTrue)
    {
        view_->onSize(&rect);
    }

    HWND hPlugin = static_cast<HWND>(hwnd);
    ShowWindow(hPlugin, SW_SHOW);
    UpdateWindow(hPlugin);
    SetFocus(hPlugin);
}

void VST3Host::showPluginUI()
{
    std::cout << "[KJ] Displaying plugin GUI window...\n";
    // TODO: integrate IPlugView / VSTGUI later
}

} // namespace kj
