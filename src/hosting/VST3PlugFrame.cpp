#ifdef _WIN32

#include "hosting/VST3PlugFrame.h"

#include <cstring>

#include "hosting/VST3Host.h"

namespace kj {

PlugFrame::PlugFrame(VST3Host& host) : host_(host) {}

void PlugFrame::setHostWindow(HWND window)
{
    hostWindow_ = window;
}

void PlugFrame::setActiveView(Steinberg::IPlugView* view)
{
    activeView_ = view;
    if (!view)
        clearCachedRect();
}

void PlugFrame::setCachedRect(const Steinberg::ViewRect& rect)
{
    cachedRect_ = rect;
    hasCachedRect_ = true;
}

void PlugFrame::clearCachedRect()
{
    cachedRect_ = {};
    hasCachedRect_ = false;
}

tresult PLUGIN_API PlugFrame::queryInterface(const Steinberg::TUID iid, void** obj)
{
    if (!obj)
        return kInvalidArgument;

    *obj = nullptr;

    if (std::memcmp(iid, Steinberg::IPlugFrame::iid, sizeof(Steinberg::TUID)) == 0 ||
        std::memcmp(iid, Steinberg::FUnknown::iid, sizeof(Steinberg::TUID)) == 0)
    {
        *obj = static_cast<Steinberg::IPlugFrame*>(this);
        addRef();
        return kResultOk;
    }

    return kNoInterface;
}

Steinberg::uint32 PLUGIN_API PlugFrame::addRef()
{
    return ++refCount_;
}

Steinberg::uint32 PLUGIN_API PlugFrame::release()
{
    auto newCount = --refCount_;
    if (newCount == 0)
        delete this;
    return newCount;
}

tresult PLUGIN_API PlugFrame::resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize)
{
    if (!newSize)
        return kInvalidArgument;

    if (view && view != activeView_)
        activeView_ = view;

    const Steinberg::ViewRect requestedRect = *newSize;
    const bool sizeChanged = !hasCachedRect_ || cachedRect_.left != requestedRect.left ||
                             cachedRect_.top != requestedRect.top || cachedRect_.right != requestedRect.right ||
                             cachedRect_.bottom != requestedRect.bottom;

    if (!sizeChanged)
        return kResultOk;

    Steinberg::IPlugView* targetView = activeView_;
    const bool resized = host_.resizePluginViewWindow(hostWindow_, requestedRect, true);
    if (resized && targetView)
    {
        Steinberg::ViewRect notifyRect = requestedRect;
        targetView->onSize(&notifyRect);
    }

    cachedRect_ = requestedRect;
    hasCachedRect_ = true;
    return kResultOk;
}

} // namespace kj

#endif // _WIN32

