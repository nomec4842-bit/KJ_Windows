#include "hosting/VST3Host.h"
#include <iostream>
#include <filesystem>

#define SMTG_OS_WINDOWS 1
#define SMTG_PLATFORM_WINDOWS 1
#define SMTG_EXPORT_MODULE_ENTRY 1

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

namespace kj {

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

            return true;
        }
    }

    std::cerr << "No valid audio effect found in " << pluginPath << std::endl;
    return false;
}

void VST3Host::showPluginUI()
{
    std::cout << "[KJ] Displaying plugin GUI window...\n";
    // TODO: integrate IPlugView / VSTGUI later
}

} // namespace kj
