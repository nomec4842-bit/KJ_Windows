#pragma once

#include <memory>
#include <string>

namespace Steinberg
{
template <class T>
class IPtr;

namespace Vst
{
class Module;
class IComponent;
} // namespace Vst
} // namespace Steinberg

namespace kj
{
class VST3Host
{
public:
    VST3Host() = default;
    ~VST3Host();

    bool load(const std::string& path);
    void unload();

private:
    std::shared_ptr<Steinberg::Vst::Module> module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
};
} // namespace kj

