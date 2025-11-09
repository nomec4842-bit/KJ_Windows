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

#ifdef _WIN32
struct HWND__;
using HWND = HWND__*;
#endif

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
#ifdef _WIN32
    class PlugFrame;
    void destroyPluginUI();
#endif

    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::IPlugView> view_;

#ifdef _WIN32
    PlugFrame* plugFrame_ = nullptr;
    HWND childWindow_ = nullptr;
    bool frameAttached_ = false;
    bool viewAttached_ = false;
#endif

    double preparedSampleRate_ = 0.0;
    int preparedMaxBlockSize_ = 0;
    bool processingActive_ = false;
};

} // namespace kj
