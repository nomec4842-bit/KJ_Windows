#ifdef _WIN32

#include "hosting/VSTEditorWindow.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>
#include <future>

#include "hosting/VST3Host.h"
#include "hosting/VSTGuiThread.h"

namespace kj {

using namespace kj;
std::shared_ptr<VSTEditorWindow> VSTEditorWindow::create(const std::shared_ptr<VST3Host>& host)
{
    return std::shared_ptr<VSTEditorWindow>(new VSTEditorWindow(host), Deleter{});
}

VSTEditorWindow::VSTEditorWindow(const std::shared_ptr<VST3Host>& host) : host_(host)
{
    if (host)
        title_ = host->getPluginDisplayName();
}

void VSTEditorWindow::show()
{
    auto self = shared_from_this();
    auto task = [self]() { self->showOnGuiThread(); };
    auto& gui = VSTGuiThread::instance();
    if (gui.isGuiThread())
        task();
    else
        gui.post(task);
}

void VSTEditorWindow::close()
{
    auto self = shared_from_this();
    auto task = [self]() { self->destroyOnGuiThread(); };
    auto& gui = VSTGuiThread::instance();
    if (gui.isGuiThread())
        task();
    else
        gui.post(task);
}

void VSTEditorWindow::showOnGuiThread()
{
    if (!hwnd_ || !::IsWindow(hwnd_))
    {
        if (!createWindow())
        {
            std::cerr << "[VST] Failed to create VST3 editor window." << std::endl;
            return;
        }
    }

    Show();
}

void VSTEditorWindow::Show()
{
    if (!hwnd_ || !::IsWindow(hwnd_))
        return;

    ::ShowWindow(hwnd_, SW_SHOWNORMAL);
    ::UpdateWindow(hwnd_);

    if (!view_ || attached_)
    {
        focus();
        return;
    }

    auto host = host_.lock();
    if (!host)
        return;

    plugFrame_->setActiveView(view_);
    plugFrame_->setCachedRect(lastRect_);

    if (view_->setFrame(plugFrame_) != Steinberg::kResultOk)
    {
        detachView();
        return;
    }

    // --------------------------------------------------------------
    // ASYNC ATTACH PIPELINE (fixes UI deadlock + host freeze)
    // --------------------------------------------------------------
    auto self = shared_from_this();
    auto attachTask = [self]() {
        auto host2 = self->host_.lock();
        if (!host2)
            return;

        if (!self->view_ || !self->hwnd_ || !::IsWindow(self->hwnd_))
            return;

        // Ensure window is visible before attach
        if (!::IsWindowVisible(self->hwnd_))
            ::ShowWindow(self->hwnd_, SW_SHOWNORMAL);

        // Perform attach on GUI thread
        if (self->view_->attached(reinterpret_cast<void*>(self->hwnd_), self->platformType_.c_str()) == Steinberg::kResultOk)
        {
            self->attached_ = true;
            self->view_->onSize(&self->lastRect_);
            host2->storeCurrentViewRect(self->lastRect_);

            if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(self->view_))
            {
                scaleSupport->setContentScaleFactor(1.0f);
            }

            // Focus after successful attach
            ::SetForegroundWindow(self->hwnd_);
            ::SetFocus(self->hwnd_);
        }
        else
        {
            self->detachView();
        }
    };

    // Post async task to GUI thread to keep message pump alive
    auto& guiThread = VSTGuiThread::instance();
    if (guiThread.isGuiThread())
        attachTask();
    else
        guiThread.post(attachTask);

    // Do NOT block GUI thread anymore.
    // focus() removed – now done inside async attach
}

void VSTEditorWindow::destroyOnGuiThread()
{
    detachView();
    if (hwnd_ && ::IsWindow(hwnd_))
    {
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
}

bool VSTEditorWindow::createWindow()
{
    auto& guiThread = VSTGuiThread::instance();
    if (!guiThread.isGuiThread())
    {
        bool created = false;
        auto self = shared_from_this();
        auto future = guiThread.post([self, &created]() {
            if (!self)
                return;
            created = self->createWindowInternal();
        });
        future.wait();
        return created;
    }

    return createWindowInternal();
}

bool VSTEditorWindow::createWindowInternal()
{
    auto host = host_.lock();
    if (!host)
        return false;

    if (!host->isPluginReady())
    {
        std::cerr << "[VST] Plug-in not ready; editor cannot be created." << std::endl;
        return false;
    }

    Steinberg::IPtr<Steinberg::IPlugView> view;
    Steinberg::ViewRect initialRect {};
    std::string platformType;
    if (!host->createEditorViewOnGui(view, initialRect, platformType))
    {
        std::cerr << "[VST] Plug-in did not provide a usable view." << std::endl;
        return false;
    }

    platformType_ = platformType;
    if (platformType_.empty())
    {
        std::cerr << "[VST] Plug-in view does not expose any supported platform types." << std::endl;
        return false;
    }

    static std::once_flag classFlag;
    static bool classRegistered = false;
    std::call_once(classFlag, []() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_HREDRAW | CS_VREDRAW | CS_OWNDC;
        wc.lpfnWndProc = &VSTEditorWindow::WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClass;
        classRegistered = ::RegisterClassExW(&wc) != 0;
    });

    if (!classRegistered)
        return false;

    HWND parent = host->getParentWindowForEditor();
    const bool parentValid = ::IsWindow(parent) != FALSE;
    if (!parentValid)
        parent = ::GetDesktopWindow();

    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    DWORD exStyle = 0;
    HWND windowParent = parent;
    HWND ownerForPopup = nullptr;

    if (parentValid)
    {
        const DWORD parentThread = ::GetWindowThreadProcessId(parent, nullptr);
        const DWORD currentThread = ::GetCurrentThreadId();
        if (parentThread != 0 && parentThread != currentThread)
        {
            std::cerr << "[VST] Parent window belongs to a different thread; creating popup editor window instead." << std::endl;
            style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            exStyle = WS_EX_TOOLWINDOW;

            // The parent belongs to another thread, so pass nullptr to CreateWindowExW
            // to avoid cross-thread parent/child creation failures. Re-attach as owner
            // after creation so focus/z-order still follow the host.
            windowParent = nullptr;
            ownerForPopup = parent;
        }
    }

    const int width = std::max<int>(1, initialRect.getWidth() > 0 ? initialRect.getWidth() : 800);
    const int height = std::max<int>(1, initialRect.getHeight() > 0 ? initialRect.getHeight() : 600);

    hwnd_ = ::CreateWindowExW(exStyle, kWindowClass, title_.c_str(), style, 0, 0, width, height, windowParent, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (!hwnd_)
        return false;

    if (ownerForPopup)
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerForPopup));

    view_ = view;
    plugFrame_ = Steinberg::IPtr<PlugFrame>(new PlugFrame(*host));
    plugFrame_->setHostWindow(hwnd_);
    plugFrame_->setActiveView(view_);
    plugFrame_->setCachedRect(initialRect);

    lastRect_ = initialRect;

    return true;
}

void VSTEditorWindow::detachView()
{
    if (view_ && attached_)
    {
        view_->removed();
        attached_ = false;
    }

    if (view_ && plugFrame_)
    {
        if (auto host = host_.lock())
        {
            view_->setFrame(nullptr);
        }
        else
        {
            view_->setFrame(nullptr);
        }
        plugFrame_->setActiveView(nullptr);
        plugFrame_->setHostWindow(nullptr);
    }

    plugFrame_ = nullptr;
    view_ = nullptr;
    platformType_.clear();
}

void VSTEditorWindow::onResize(UINT width, UINT height)
{
    lastRect_.left = 0;
    lastRect_.top = 0;
    lastRect_.right = static_cast<Steinberg::int32>(width);
    lastRect_.bottom = static_cast<Steinberg::int32>(height);

    if (plugFrame_)
        plugFrame_->setCachedRect(lastRect_);

    if (view_ && attached_)
    {
        view_->onSize(&lastRect_);
    }

    if (auto host = host_.lock())
        host->storeCurrentViewRect(lastRect_);
}

void VSTEditorWindow::focus()
{
    if (hwnd_ && ::IsWindow(hwnd_))
    {
        ::SetForegroundWindow(hwnd_);
        ::SetFocus(hwnd_);
    }
}

LRESULT CALLBACK VSTEditorWindow::WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        if (create && create->lpCreateParams)
            ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }

    auto* window = reinterpret_cast<VSTEditorWindow*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg)
    {
    case WM_SIZE:
        if (window)
            window->onResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SETFOCUS:
        if (window && window->view_)
        {
            if (auto host = window->host_.lock())
            {
                window->view_->onFocus(static_cast<Steinberg::TBool>(true));
            }
            else
            {
                window->view_->onFocus(static_cast<Steinberg::TBool>(true));
            }
        }
        return 0;
    case WM_KILLFOCUS:
        if (window && window->view_)
        {
            if (auto host = window->host_.lock())
            {
                window->view_->onFocus(static_cast<Steinberg::TBool>(false));
            }
            else
            {
                window->view_->onFocus(static_cast<Steinberg::TBool>(false));
            }
        }
        return 0;
    case WM_DESTROY:
        if (window)
            window->detachView();
        return 0;
    default:
        break;
    }

    if (window && window->view_)
    {
        switch (msg)
        {
        case WM_MOUSEWHEEL:
            if (auto host = window->host_.lock())
            {
                window->view_->onWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            }
            else
            {
                window->view_->onWheel(GET_WHEEL_DELTA_WPARAM(wParam));
            }
            return 0;
        default:
            break;
        }
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace kj

#endif // _WIN32

