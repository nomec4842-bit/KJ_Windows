#include "hosting/VSTGuiThread.h"

#include <iostream>

int main()
{
#if defined(_WIN32)
    auto& guiThread = kj::VSTGuiThread::instance();

    auto falseFuture = guiThread.post([]() { return false; });
    falseFuture.wait();
    if (falseFuture.get())
    {
        std::cerr << "[Test] Expected posted task result to propagate false." << std::endl;
        return 1;
    }

    auto trueFuture = guiThread.post([]() { return true; });
    trueFuture.wait();
    if (!trueFuture.get())
    {
        std::cerr << "[Test] Expected posted task result to propagate true." << std::endl;
        return 1;
    }

    guiThread.shutdown();
    std::cout << "[Test] VSTGuiThread post propagation checks passed." << std::endl;
#endif
    return 0;
}
