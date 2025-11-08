#include "hosting/VST3Host.h"

#include <iostream>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/base/ipluginbase.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"

namespace kj
{
namespace
{
// Helper function to safely downcast a class info to an IComponent instance.
Steinberg::IPtr<Steinberg::Vst::IComponent> createFirstComponent(Steinberg::FUnknown* factoryUnknown)
{
    if (!factoryUnknown)
        return {};

    // Wrap the raw factory pointer in the Steinberg smart pointer. This increments the
    // underlying COM reference count and guarantees release() is called when the IPtr
    // falls out of scope.
    Steinberg::IPtr<Steinberg::IPluginFactory> factory(factoryUnknown);
    if (!factory)
        return {};

    Steinberg::int32 classCount = factory->countClasses();
    for (Steinberg::int32 index = 0; index < classCount; ++index)
    {
        Steinberg::PClassInfo classInfo{};
        if (factory->getClassInfo(index, &classInfo) != Steinberg::kResultOk)
            continue;

        Steinberg::IPtr<Steinberg::Vst::IComponent> component;
        if (factory->createInstance(classInfo.cid, Steinberg::Vst::IComponent::iid,
                                    reinterpret_cast<void**>(&component)) == Steinberg::kResultOk && component)
        {
            // createInstance hands back an AddRef'ed interface pointer. Assigning it to
            // the IPtr retains that reference until we explicitly reset it, keeping the
            // component alive for the host lifetime.
            return component;
        }
    }

    return {};
}
} // namespace

bool VST3Host::load(const std::string& path)
{
    // Always start from a clean state before attempting a new load.
    unload();

    std::cout << "[VST3Host] Loading VST3 module: " << path << std::endl;

    // Module::create returns a shared_ptr that manages the plugin DLL lifetime.
    auto module = Steinberg::Vst::Module::create(path);
    if (!module)
    {
        std::cout << "[VST3Host] Failed to load VST3 plugin." << std::endl;
        return false;
    }

    // The module exposes its factory through the IPluginFactory interface. The returned
    // pointer follows COM rules and must be wrapped in an IPtr to manage lifetime.
    Steinberg::FUnknown* factoryUnknown = module->getFactory();
    Steinberg::IPtr<Steinberg::Vst::IComponent> component = createFirstComponent(factoryUnknown);

    if (!component)
    {
        std::cout << "[VST3Host] Failed to instantiate component from VST3 factory." << std::endl;
        module->unload();
        return false;
    }

    // Store the instantiated component and module so their reference counts stay alive.
    component_ = component;
    module_ = std::move(module);

    std::cout << "[VST3Host] VST3 plugin loaded successfully." << std::endl;
    return true;
}

void VST3Host::unload()
{
    if (component_)
    {
        // Releasing the IPtr drops the COM reference count for the component. Once the
        // count reaches zero the plug-in component is destroyed by the SDK.
        component_ = nullptr;
    }

    if (module_)
    {
        // Resetting the shared_ptr unloads the module and releases all classes. Module::unload
        // ensures the DLL is detached before the shared_ptr clears its final reference.
        module_->unload();
        module_.reset();
    }
}

VST3Host::~VST3Host()
{
    // Ensure we always release the module before destruction to avoid leaks.
    unload();
}

} // namespace kj

#ifdef KJ_VST3HOST_TEST
int main()
{
    kj::VST3Host host;
    host.load("plugin.vst3");
    return 0;
}
#endif

