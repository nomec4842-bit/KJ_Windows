#pragma once

// Steinberg base + VST interfaces (you'll likely use these in the .cpp later)
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "base/source/fobject.h" // for Steinberg::IPtr

// VST3 hosting layer (your module.h snippet)
#include "public.sdk/source/vst/hosting/module.h"

#include <memory>
#include <string>
#include <iostream>

namespace kj {

class VST3Host {
public:
    bool load(const std::string& path);
    void unload();
    ~VST3Host();

private:
    // If you want to keep a raw component later, you can add:
    // Steinberg::IPtr<Steinberg::Vst::IComponent> component_{nullptr};

    // This is the **correct** type for the Module in your SDK:
    VST3::Hosting::Module::Ptr module_;
};

} // namespace kj
