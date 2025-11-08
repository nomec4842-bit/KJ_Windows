#pragma once

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "base/source/fobject.h" // for Steinberg::IPtr
#include "public.sdk/source/vst/hosting/module.h" // defines Steinberg::Vst::Module
#include "public.sdk/source/vst/hosting/hostclasses.h"

#include <memory>
#include <iostream>

namespace kj {
class VST3Host {
public:
    bool load(const std::string& path);
    void unload();
    ~VST3Host();

private:
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_ = nullptr;
    std::shared_ptr<Steinberg::Vst::Module> module_; // works now
};
} // namespace kj

