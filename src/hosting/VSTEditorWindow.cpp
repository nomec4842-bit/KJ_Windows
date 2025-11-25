#ifdef _WIN32

#include "hosting/VSTEditorWindow.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include "hosting/VST3Host.h"
#include "hosting/VSTGuiThread.h"

namespace kj {

using namespace kj;

static void waitForGuiAttachReady(kj::VST3Host* host)
{
    // Wait up to 2 seconds for DSP thread + host to finish initializing
    using namespace std::chrono_literals;
    const auto timeout = std::chrono::steady_clock::now() + 2s;

    while (!host->guiAttachReady_.load(std::memory_order_acquire)) {
        if (std::chrono::steady_clock::now() > timeout)
            break;
        std::this_thread::sleep_for(10ms);
    }
}

namespace {
RECT computeWindowRect(const Steinberg::ViewRect& rect, DWORD style, DWORD exStyle)
{
    RECT desired {rect.left, rect.top, rect.right, rect.bottom};
    ::AdjustWindowRectEx(&desired, style, FALSE, exStyle);
    return desired;
}
}

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

    // Wait until VST3Host is ready for GUI attach
    waitForGuiAttachReady(host.get());

    if (!::IsWindowVisible(hwnd_))
        return;

    if (view_->attached(reinterpret_cast<void*>(hwnd_), platformType_.c_str()) != Steinberg::kResultOk)
    {
        detachView();
        return;
    }

    attached_ = true;
    view_->onSize(&lastRect_);
    host->storeCurrentViewRect(lastRect_);

    if (auto scaleSupport = Steinberg::FUnknownPtr<Steinberg::IPlugViewContentScaleSupport>(view_))
    {
        scaleSupport->setContentScaleFactor(1.0f);
    }

    focus();
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
    auto host = host_.lock();
    if (!host)
        return false;

    if (!host->waitForPluginReady())
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
    std::call_once(classFlag, []() {
        WNDCLASSEXW wc {};
        wc.cbSize = sizeof(WNDCLASSEXW);
        wc.style = CS_DBLCLKS;
        wc.lpfnWndProc = &VSTEditorWindow::WndProc;
        wc.hInstance = ::GetModuleHandleW(nullptr);
        wc.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kWindowClass;
        ::RegisterClassExW(&wc);
    });

    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD exStyle = WS_EX_APPWINDOW;
    RECT desired = computeWindowRect(initialRect, style, exStyle);

    hwnd_ = ::CreateWindowExW(exStyle, kWindowClass, title_.c_str(), style, CW_USEDEFAULT, CW_USEDEFAULT,
                              desired.right - desired.left, desired.bottom - desired.top, nullptr, nullptr,
                              ::GetModuleHandleW(nullptr), this);
    if (!hwnd_)
        return false;

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

