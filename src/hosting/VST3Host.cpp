#include "hosting/VST3Host.h"

#include <algorithm>
#include <iostream>
#include <vector>

namespace kj
{

namespace
{
bool categoryMatches(const VST3::Hosting::ClassInfo& info)
{
    const auto& category = info.category();
    const auto& sub = info.subCategoriesString();

    auto contains = [](const std::string& haystack, const char* needle) {
        return haystack.find(needle) != std::string::npos;
    };

    if (contains(category, "Audio Module Class") || contains(category, "Fx"))
        return true;
    if (contains(sub, "Fx") || contains(sub, "Instrument"))
        return true;
    return false;
}
} // namespace

VST3Host::~VST3Host()
{
    unload();
}

bool VST3Host::load(const std::string& path)
{
    unload();

    std::cout << "[VST3Host] Loading module from " << path << std::endl;

    std::string error;
    module_ = VST3::Hosting::Module::create(path, error);
    if (!module_)
    {
        std::cerr << "[VST3Host] Failed to load module: " << error << std::endl;
        return false;
    }

    const auto& factory = module_->getFactory();
    auto classInfos = factory.classInfos();
    if (classInfos.empty())
    {
        std::cerr << "[VST3Host] No classes exported by module." << std::endl;
        unload();
        return false;
    }

    const VST3::Hosting::ClassInfo* chosenInfo = nullptr;
    for (const auto& info : classInfos)
    {
        if (categoryMatches(info))
        {
            chosenInfo = &info;
            break;
        }
    }
    if (!chosenInfo)
    {
        chosenInfo = &classInfos.front();
        std::cout << "[VST3Host] Using first available class: " << chosenInfo->name() << std::endl;
    }
    else
    {
        std::cout << "[VST3Host] Selected class: " << chosenInfo->name() << std::endl;
    }

    component_ = factory.createInstance<Steinberg::Vst::IComponent>(chosenInfo->ID());
    if (!component_)
    {
        std::cerr << "[VST3Host] Failed to create component instance." << std::endl;
        unload();
        return false;
    }

    if (component_->initialize(nullptr) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] Component initialization failed." << std::endl;
        unload();
        return false;
    }

    Steinberg::Vst::IAudioProcessor* processor = nullptr;
    if (component_->queryInterface(Steinberg::Vst::IAudioProcessor::iid,
                                   reinterpret_cast<void**>(&processor)) != Steinberg::kResultOk || !processor)
    {
        std::cerr << "[VST3Host] Component does not expose IAudioProcessor." << std::endl;
        unload();
        return false;
    }
    processor_ = Steinberg::owned(processor);

    Steinberg::Vst::IEditController* controller = nullptr;
    if (component_->queryInterface(Steinberg::Vst::IEditController::iid,
                                   reinterpret_cast<void**>(&controller)) == Steinberg::kResultOk && controller)
    {
        controller_ = Steinberg::owned(controller);
    }
    else
    {
        std::cout << "[VST3Host] No edit controller provided." << std::endl;
    }

    preparedSampleRate_ = 0.0;
    preparedMaxBlockSize_ = 0;
    processingActive_ = false;

    std::cout << "[VST3Host] Module loaded successfully." << std::endl;
    return true;
}

void VST3Host::unload()
{
    if (view_)
    {
        view_->removed();
        view_ = nullptr;
    }

    if (controller_)
    {
        controller_ = nullptr;
    }

    if (processor_)
    {
        processor_->setProcessing(false);
        processor_ = nullptr;
    }

    if (component_)
    {
        component_->setActive(false);
        component_->terminate();
        component_ = nullptr;
    }

    if (module_)
    {
        std::cout << "[VST3Host] Unloading module." << std::endl;
        module_.reset();
    }

    preparedSampleRate_ = 0.0;
    preparedMaxBlockSize_ = 0;
    processingActive_ = false;
}

bool VST3Host::prepare(double sampleRate, int maxBlockSize)
{
    if (!processor_)
    {
        std::cerr << "[VST3Host] Cannot prepare: no processor." << std::endl;
        return false;
    }

    if (sampleRate <= 0.0 || maxBlockSize <= 0)
    {
        std::cerr << "[VST3Host] Invalid prepare arguments." << std::endl;
        return false;
    }

    if (processingActive_ && sampleRate == preparedSampleRate_ && maxBlockSize == preparedMaxBlockSize_)
        return true;

    Steinberg::Vst::ProcessSetup setup{};
    setup.processMode = Steinberg::Vst::kRealtime;
    setup.sampleRate = sampleRate;
    setup.maxSamplesPerBlock = maxBlockSize;
    setup.symbolicSampleSize = Steinberg::Vst::kSample32;

    if (component_ && component_->setActive(true) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] Failed to activate component." << std::endl;
    }

    if (processor_->setupProcessing(setup) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] setupProcessing failed." << std::endl;
        return false;
    }

    if (processor_->setProcessing(true) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] setProcessing(true) failed." << std::endl;
        return false;
    }

    preparedSampleRate_ = sampleRate;
    preparedMaxBlockSize_ = maxBlockSize;
    processingActive_ = true;
    std::cout << "[VST3Host] Prepared processor (sr=" << sampleRate
              << ", block=" << maxBlockSize << ")." << std::endl;
    return true;
}

void VST3Host::process(float** outputs, int numChannels, int numSamples)
{
    if (!processor_ || !processingActive_)
        return;

    if (!outputs || numChannels <= 0 || numSamples <= 0)
        return;

    std::vector<Steinberg::Vst::Sample32*> channelPointers;
    channelPointers.resize(static_cast<std::size_t>(numChannels), nullptr);

    for (int ch = 0; ch < numChannels; ++ch)
    {
        if (outputs[ch])
        {
            std::fill(outputs[ch], outputs[ch] + numSamples, 0.0f);
            channelPointers[static_cast<std::size_t>(ch)] = outputs[ch];
        }
    }

    Steinberg::Vst::AudioBusBuffers outputBus{};
    outputBus.numChannels = numChannels;
    outputBus.silenceFlags = 0;
    outputBus.channelBuffers32 = channelPointers.data();

    Steinberg::Vst::ProcessData processData{};
    processData.processMode = Steinberg::Vst::kRealtime;
    processData.symbolicSampleSize = Steinberg::Vst::kSample32;
    processData.numSamples = numSamples;
    processData.numInputs = 0;
    processData.numOutputs = 1;
    processData.outputs = &outputBus;

    if (processor_->process(processData) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] process() failed." << std::endl;
    }
}

bool VST3Host::isPluginLoaded() const
{
    return static_cast<bool>(module_);
}

void VST3Host::openEditor(void* nativeWindowHandle)
{
    if (!controller_ || !nativeWindowHandle)
    {
        std::cerr << "[VST3Host] Cannot open editor: controller or window handle missing." << std::endl;
        return;
    }

    if (view_)
    {
        view_->removed();
        view_ = nullptr;
    }

    Steinberg::IPlugView* rawView = controller_->createView(Steinberg::Vst::ViewType::kEditor);
    if (!rawView)
    {
        std::cerr << "[VST3Host] Plug-in did not provide an editor view." << std::endl;
        return;
    }

    view_ = Steinberg::owned(rawView);

    if (view_->isPlatformTypeSupported(Steinberg::kPlatformTypeHWND) != Steinberg::kResultTrue)
    {
        std::cerr << "[VST3Host] Editor view does not support HWND platform." << std::endl;
        view_->removed();
        view_ = nullptr;
        return;
    }

    if (view_->attached(nativeWindowHandle, Steinberg::kPlatformTypeHWND) != Steinberg::kResultOk)
    {
        std::cerr << "[VST3Host] Failed to attach editor view." << std::endl;
        view_->removed();
        view_ = nullptr;
        return;
    }

    Steinberg::ViewRect rect;
    if (view_->getSize(&rect) == Steinberg::kResultOk)
    {
        view_->onSize(&rect);
    }

    std::cout << "[VST3Host] Editor opened." << std::endl;
}

} // namespace kj
