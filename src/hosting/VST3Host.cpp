#include "hosting/VST3Host.h"
#include <iostream>

using namespace VST3::Hosting;
using namespace Steinberg;

namespace kj
{

bool VST3Host::load(const std::string& path)
{
    unload(); // always start clean

    std::string error;
    module_ = Module::create(path, error);
    if (!module_)
    {
        std::cerr << "[VST3Host] Failed to load module: " << error << std::endl;
        return false;
    }

    std::cout << "[VST3Host] Loaded VST3 module: " << module_->getName() << std::endl;
    return true;
}

void VST3Host::unload()
{
    if (module_)
    {
        module_.reset(); // shared_ptr reset unloads the DLL
        std::cout << "[VST3Host] Module unloaded." << std::endl;
    }
}

VST3Host::~VST3Host()
{
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
