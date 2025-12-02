#pragma once

#ifdef _WIN32

#include <atomic>
#include <windows.h>

#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"

namespace kj {

class VST3Host;

class PlugFrame : public Steinberg::IPlugFrame
{
public:
    explicit PlugFrame(VST3Host& host);

    void setHostWindow(HWND window);
    void setActiveView(Steinberg::IPlugView* view);
    void setCachedRect(const Steinberg::ViewRect& rect);
    void clearCachedRect();
    void setRunLoop(Steinberg::IPtr<Steinberg::FUnknown> runLoop);

    // IPlugFrame
    Steinberg::tresult PLUGIN_API queryInterface(const Steinberg::TUID iid, void** obj) override;
    Steinberg::uint32 PLUGIN_API addRef() override;
    Steinberg::uint32 PLUGIN_API release() override;
    Steinberg::tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override;

private:
    std::atomic<Steinberg::uint32> refCount_ {1};
    VST3Host& host_;
    HWND hostWindow_ = nullptr;
    Steinberg::IPlugView* activeView_ = nullptr;
    Steinberg::ViewRect cachedRect_ {};
    bool hasCachedRect_ = false;
    Steinberg::IPtr<Steinberg::FUnknown> runLoop_;
};

} // namespace kj

#endif // _WIN32

