#ifdef _WIN32

#include "hosting/VSTEditorWindow.h"

#include <algorithm>
#include <chrono>
#include <atomic>
#include <iostream>
#include <string>
#include <thread>
#include <future>
#include <mutex>
#include <array>
#include <cwctype>

#include "hosting/VST3Host.h"
#include "hosting/VSTGuiThread.h"
#include "pluginterfaces/base/keycodes.h"

namespace
{
Steinberg::int16 translateVirtualKeyFromVK(WPARAM vk)
{
    using Steinberg::CharToVirtualKeyCode;

    switch (vk)
    {
    case VK_BACK: return Steinberg::KEY_BACK;
    case VK_TAB: return Steinberg::KEY_TAB;
    case VK_CLEAR: return Steinberg::KEY_CLEAR;
    case VK_RETURN: return Steinberg::KEY_RETURN;
    case VK_PAUSE: return Steinberg::KEY_PAUSE;
    case VK_ESCAPE: return Steinberg::KEY_ESCAPE;
    case VK_SPACE: return Steinberg::KEY_SPACE;
    case VK_PRIOR: return Steinberg::KEY_PAGEUP;
    case VK_NEXT: return Steinberg::KEY_PAGEDOWN;
    case VK_END: return Steinberg::KEY_END;
    case VK_HOME: return Steinberg::KEY_HOME;
    case VK_LEFT: return Steinberg::KEY_LEFT;
    case VK_UP: return Steinberg::KEY_UP;
    case VK_RIGHT: return Steinberg::KEY_RIGHT;
    case VK_DOWN: return Steinberg::KEY_DOWN;
    case VK_SNAPSHOT: return Steinberg::KEY_SNAPSHOT;
    case VK_INSERT: return Steinberg::KEY_INSERT;
    case VK_DELETE: return Steinberg::KEY_DELETE;
    case VK_HELP: return Steinberg::KEY_HELP;
    case VK_NUMPAD0: return Steinberg::KEY_NUMPAD0;
    case VK_NUMPAD1: return Steinberg::KEY_NUMPAD1;
    case VK_NUMPAD2: return Steinberg::KEY_NUMPAD2;
    case VK_NUMPAD3: return Steinberg::KEY_NUMPAD3;
    case VK_NUMPAD4: return Steinberg::KEY_NUMPAD4;
    case VK_NUMPAD5: return Steinberg::KEY_NUMPAD5;
    case VK_NUMPAD6: return Steinberg::KEY_NUMPAD6;
    case VK_NUMPAD7: return Steinberg::KEY_NUMPAD7;
    case VK_NUMPAD8: return Steinberg::KEY_NUMPAD8;
    case VK_NUMPAD9: return Steinberg::KEY_NUMPAD9;
    case VK_MULTIPLY: return Steinberg::KEY_MULTIPLY;
    case VK_ADD: return Steinberg::KEY_ADD;
    case VK_SEPARATOR: return Steinberg::KEY_SEPARATOR;
    case VK_SUBTRACT: return Steinberg::KEY_SUBTRACT;
    case VK_DECIMAL: return Steinberg::KEY_DECIMAL;
    case VK_DIVIDE: return Steinberg::KEY_DIVIDE;
    case VK_F1: return Steinberg::KEY_F1;
    case VK_F2: return Steinberg::KEY_F2;
    case VK_F3: return Steinberg::KEY_F3;
    case VK_F4: return Steinberg::KEY_F4;
    case VK_F5: return Steinberg::KEY_F5;
    case VK_F6: return Steinberg::KEY_F6;
    case VK_F7: return Steinberg::KEY_F7;
    case VK_F8: return Steinberg::KEY_F8;
    case VK_F9: return Steinberg::KEY_F9;
    case VK_F10: return Steinberg::KEY_F10;
    case VK_F11: return Steinberg::KEY_F11;
    case VK_F12: return Steinberg::KEY_F12;
    case VK_F13: return Steinberg::KEY_F13;
    case VK_F14: return Steinberg::KEY_F14;
    case VK_F15: return Steinberg::KEY_F15;
    case VK_F16: return Steinberg::KEY_F16;
    case VK_F17: return Steinberg::KEY_F17;
    case VK_F18: return Steinberg::KEY_F18;
    case VK_F19: return Steinberg::KEY_F19;
    case VK_F20: return Steinberg::KEY_F20;
    case VK_F21: return Steinberg::KEY_F21;
    case VK_F22: return Steinberg::KEY_F22;
    case VK_F23: return Steinberg::KEY_F23;
    case VK_F24: return Steinberg::KEY_F24;
    case VK_NUMLOCK: return Steinberg::KEY_NUMLOCK;
    case VK_SCROLL: return Steinberg::KEY_SCROLL;
    case VK_SHIFT: return Steinberg::KEY_SHIFT;
    case VK_CONTROL: return Steinberg::KEY_CONTROL;
    case VK_MENU: return Steinberg::KEY_ALT;
    case VK_APPS: return Steinberg::KEY_CONTEXTMENU;
    case VK_LWIN:
    case VK_RWIN: return Steinberg::KEY_SUPER;
    default:
        break;
    }

    if (vk >= 'A' && vk <= 'Z')
        return CharToVirtualKeyCode(static_cast<Steinberg::tchar>(vk));

    if (vk >= 'a' && vk <= 'z')
        return CharToVirtualKeyCode(static_cast<Steinberg::tchar>(std::towupper(static_cast<wchar_t>(vk))));

    if (vk >= '0' && vk <= '9')
        return CharToVirtualKeyCode(static_cast<Steinberg::tchar>(vk));

    return 0;
}

Steinberg::int16 currentKeyModifiers()
{
    Steinberg::int16 modifiers = 0;
    if (::GetKeyState(VK_SHIFT) & 0x8000)
        modifiers |= Steinberg::kShiftKey;
    if (::GetKeyState(VK_MENU) & 0x8000)
        modifiers |= Steinberg::kAlternateKey;
    if (::GetKeyState(VK_CONTROL) & 0x8000)
        modifiers |= Steinberg::kCommandKey;
    if ((::GetKeyState(VK_LWIN) & 0x8000) || (::GetKeyState(VK_RWIN) & 0x8000))
        modifiers |= Steinberg::kControlKey;
    return modifiers;
}

Steinberg::char16 translateCharacter(UINT message, WPARAM wParam, LPARAM lParam)
{
    if (message == WM_CHAR)
        return static_cast<Steinberg::char16>(wParam);

    std::array<BYTE, 256> keyState {};
    if (!::GetKeyboardState(keyState.data()))
        return 0;

    WCHAR chars[4] = {0};
    UINT scanCode = static_cast<UINT>((lParam >> 16) & 0xff);
    int result = ::ToUnicode(static_cast<UINT>(wParam), scanCode, keyState.data(), chars, 4, 0);
    if (result > 0)
        return static_cast<Steinberg::char16>(chars[0]);

    return 0;
}

} // namespace

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

        bool created = false;
        try
        {
            created = createWindow().get();
        }
        catch (const std::exception& ex)
        {
            std::cerr << "[VST] Exception while creating VST3 editor window: " << ex.what() << std::endl;
        }
        catch (...)
        {
            std::cerr << "[VST] Unknown exception while creating VST3 editor window." << std::endl;
        }

        if (!created)
        {
            std::cerr << "[VST] Failed to create VST3 editor window." << std::endl;
            return;
        }

        Show();
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
        std::cerr << "[VST] Failed to set plug-in view frame before attachment." << std::endl;
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
            std::cerr << "[VST] Plug-in view attachment failed for platform '" << self->platformType_ << "'." << std::endl;
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
    auto& guiThreadInstance = VSTGuiThread::instance();
    if (!guiThreadInstance.isGuiThread())
    {
        std::cerr << "[VST] A VST window must be created by the thread that runs the message loop." << std::endl;
        return false;
    }

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
    {
        std::cerr << "[VST] Failed to register VST3 editor window class." << std::endl;
        return false;
    }

    // If the host does not provide a valid window handle, create a standalone popup window so the
    // editor can still appear. The popup path bypasses cross-thread parent checks and avoids the
    // previous desktop fallback, which could prevent the editor from opening when no host window
    // is available.
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
            safeParent = guiThreadInstance.ensureSafeParentWindow();
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
        safeParent = guiThreadInstance.ensureSafeParentWindow();
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
    {
        std::cerr << "[VST] CreateWindowEx failed for VST3 editor window." << std::endl;
        return false;
    }

    if (ownerForPopup)
        ::SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(ownerForPopup));

    view_ = view;
    plugFrame_ = Steinberg::IPtr<PlugFrame>(new PlugFrame(*host));
    plugFrame_->setRunLoop(host->getRunLoop());
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
    const auto viewReady = [&](const VSTEditorWindow* target) {
        return target && target->view_ && target->attached_;
    };

    const auto forwardKeyEvent = [&](UINT message, bool isKeyDown) {
        if (!viewReady(window))
            return false;

        const Steinberg::char16 character = translateCharacter(message, wParam, lParam);
        const Steinberg::int16 virtualKey =
            message == WM_CHAR ? Steinberg::CharToVirtualKeyCode(static_cast<Steinberg::tchar>(wParam))
                               : translateVirtualKeyFromVK(wParam);
        const Steinberg::int16 modifiers = currentKeyModifiers();

        const auto result = isKeyDown ? window->view_->onKeyDown(character, virtualKey, modifiers)
                                      : window->view_->onKeyUp(character, virtualKey, modifiers);

        return result == Steinberg::kResultOk;
    };

    switch (msg)
    {
    case WM_SIZE:
        if (window)
            window->onResize(LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_SETFOCUS:
        if (viewReady(window))
            window->view_->onFocus(static_cast<Steinberg::TBool>(true));
        return 0;
    case WM_KILLFOCUS:
        if (viewReady(window))
            window->view_->onFocus(static_cast<Steinberg::TBool>(false));
        return 0;
    case WM_DESTROY:
        if (window)
            window->detachView();
        return 0;
    case WM_MOUSEWHEEL:
        if (viewReady(window))
        {
            const auto result = window->view_->onWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) /
                                                       static_cast<float>(WHEEL_DELTA));
            if (result == Steinberg::kResultOk)
                return 0;
        }
        break;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN:
        if (window)
            ::SetFocus(hwnd);
        break;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
        if (forwardKeyEvent(msg, true))
            return 0;
        break;
    case WM_KEYUP:
    case WM_SYSKEYUP:
        if (forwardKeyEvent(msg, false))
            return 0;
        break;
    case WM_CHAR:
        if (forwardKeyEvent(msg, true))
            return 0;
        break;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace kj

#endif // _WIN32

