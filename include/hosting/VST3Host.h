#pragma once

#include <memory>
#include <string>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
// Required for Steinberg::IPtr template definition. Without this, MSVC reports IPtr as undefined.
#include "base/source/fobject.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

namespace kj
{
class VST3Host
{
public:
    bool load(const std::string& path);
    void unload();
    ~VST3Host();

private:
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_ = nullptr;
    std::shared_ptr<Steinberg::Vst::Module> module_;
};
} // namespace kj

