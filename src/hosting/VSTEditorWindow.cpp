#ifdef _WIN32

#include "hosting/VSTEditorWindow.h"

#include <algorithm>
#include <chrono>
#include <atomic>
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
        auto self = shared_from_this();
        auto creationFuture = createWindow();

        std::thread([self, future = std::move(creationFuture)]() mutable {
            bool created = false;
            try
            {
                created = future.get();
            }
            catch (const std::exception& ex)
            {
                std::cerr << "[VST] Exception while creating VST3 editor window: " << ex.what() << std::endl;
            }
            catch (...)
            {
                std::cerr << "[VST] Unknown exception while creating VST3 editor window." << std::endl;
            }

            auto& gui = VSTGuiThread::instance();
            auto onReady = [self, created]() {
                if (!created)
                {
                    std::cerr << "[VST] Failed to create VST3 editor window." << std::endl;
                    return;
                }
                self->Show();
            };

            if (gui.isGuiThread())
                onReady();
            else
                gui.post(onReady);
        }).detach();

        return;
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

std::future<bool> VSTEditorWindow::createWindow()
{
    auto promise = std::make_shared<std::promise<bool>>();
    auto future = promise->get_future();

    auto& guiThread = VSTGuiThread::instance();
    if (!guiThread.isGuiThread())
    {
        auto self = shared_from_this();
        auto postFuture = guiThread.post([self, promise]() {
            if (!self)
            {
                promise->set_value(false);
                return;
            }

            try
            {
                promise->set_value(self->createWindowInternal());
            }
            catch (const std::exception& ex)
            {
                std::cerr << "[VST] Exception during window creation: " << ex.what() << std::endl;
                promise->set_value(false);
            }
            catch (...)
            {
                std::cerr << "[VST] Unknown exception during window creation." << std::endl;
                promise->set_value(false);
            }
        });

        if (postFuture.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            try
            {
                if (!postFuture.get())
                    promise->set_value(false);
            }
            catch (const std::exception& ex)
            {
                std::cerr << "[VST] Posting window creation task failed: " << ex.what() << std::endl;
                promise->set_value(false);
            }
            catch (...)
            {
                std::cerr << "[VST] Posting window creation task failed with unknown error." << std::endl;
                promise->set_value(false);
            }
        }
        else
        {
            std::thread([promise, postFuture = std::move(postFuture)]() mutable {
                try
                {
                    if (!postFuture.get())
                        promise->set_value(false);
                }
                catch (const std::exception& ex)
                {
                    std::cerr << "[VST] Posting window creation task failed: " << ex.what() << std::endl;
                    promise->set_value(false);
                }
                catch (...)
                {
                    std::cerr << "[VST] Posting window creation task failed with unknown error." << std::endl;
                    promise->set_value(false);
                }
            }).detach();
        }

        return future;
    }

    try
    {
        promise->set_value(createWindowInternal());
    }
    catch (const std::exception& ex)
    {
        std::cerr << "[VST] Exception during window creation: " << ex.what() << std::endl;
        promise->set_value(false);
    }
    catch (...)
    {
        std::cerr << "[VST] Unknown exception during window creation." << std::endl;
        promise->set_value(false);
    }

    return future;
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

    // If the host does not provide a valid window handle, create a standalone popup window so the
    // editor can still appear. The popup path bypasses cross-thread parent checks and avoids the
    // previous desktop fallback, which could prevent the editor from opening when no host window
    // is available.
    auto& guiThread = VSTGuiThread::instance();

    HWND parent = host->getParentWindowForEditor();
    bool parentValid = ::IsWindow(parent) != FALSE;
    const DWORD currentThread = ::GetCurrentThreadId();

    DWORD style = WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
    DWORD exStyle = 0;
    HWND windowParent = parent;
    HWND ownerForPopup = nullptr;
    HWND safeParent = nullptr;
    bool usingSafeParent = false;

    static std::atomic<bool> crossThreadWarningShown {false};

    if (parentValid)
    {
        const DWORD parentThread = ::GetWindowThreadProcessId(parent, nullptr);
        if (parentThread != 0 && parentThread != currentThread)
        {
            safeParent = guiThread.ensureSafeParentWindow();
            usingSafeParent = safeParent && ::IsWindow(safeParent);
            parentValid = usingSafeParent;
            windowParent = usingSafeParent ? safeParent : nullptr;

            if (!usingSafeParent)
            {
                bool expected = false;
                if (crossThreadWarningShown.compare_exchange_strong(expected, true))
                {
                    std::cerr << "[VST] Parent window belongs to a different thread; creating popup editor window instead." << std::endl;
                }
                style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
                exStyle = WS_EX_TOOLWINDOW;

                windowParent = nullptr;
                ownerForPopup = parent;
            }
        }
    }
    else
    {
        safeParent = guiThread.ensureSafeParentWindow();
        usingSafeParent = safeParent && ::IsWindow(safeParent);
        parentValid = usingSafeParent;
        windowParent = usingSafeParent ? safeParent : nullptr;

        if (!usingSafeParent)
        {
            // When the host window is invalid, fall back to a standalone popup so the editor can still show.
            style = WS_POPUP | WS_VISIBLE | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
            exStyle = WS_EX_TOOLWINDOW;
        }
    }

    const int width = std::max<int>(1, initialRect.getWidth() > 0 ? initialRect.getWidth() : 800);
    const int height = std::max<int>(1, initialRect.getHeight() > 0 ? initialRect.getHeight() : 600);

    if (usingSafeParent && windowParent)
    {
        RECT parentRect {0, 0, width, height};
        DWORD parentStyle = WS_OVERLAPPEDWINDOW | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;
        DWORD parentExStyle = WS_EX_TOOLWINDOW;
        ::AdjustWindowRectEx(&parentRect, parentStyle, FALSE, parentExStyle);

        ::SetWindowLongPtrW(windowParent, GWL_STYLE, static_cast<LONG_PTR>(parentStyle));
        ::SetWindowLongPtrW(windowParent, GWL_EXSTYLE, static_cast<LONG_PTR>(parentExStyle));
        ::SetWindowTextW(windowParent, title_.c_str());
        ::SetWindowPos(windowParent, nullptr, 0, 0, parentRect.right - parentRect.left,
                       parentRect.bottom - parentRect.top,
                       SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
        ::ShowWindow(windowParent, SW_SHOWNORMAL);
        ::UpdateWindow(windowParent);
    }

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

