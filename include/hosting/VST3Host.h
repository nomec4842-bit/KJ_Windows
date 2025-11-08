#pragma once

// Steinberg base + VST interfaces
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "base/source/fobject.h" // for Steinberg::IPtr

// VST3 hosting layer (module helper)
#include "public.sdk/source/vst/hosting/module.h"

#include <memory>
#include <string>

namespace kj {

class VST3Host {
public:
    VST3Host() = default;
    ~VST3Host();

    bool load(const std::string& path);
    void showPluginUI(void* parentWindowHandle);
    void unload();

    bool prepare(double sampleRate, int maxBlockSize);
    void process(float** outputs, int numChannels, int numSamples);

    void openEditor(void* nativeWindowHandle);

    bool isPluginLoaded() const;

private:
    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::IPlugView> view_;

    double preparedSampleRate_ = 0.0;
    int preparedMaxBlockSize_ = 0;
    bool processingActive_ = false;
};

} // namespace kj
