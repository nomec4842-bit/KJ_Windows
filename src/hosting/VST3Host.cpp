#include "hosting/VST3Host.h"
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <cmath>
#include <iomanip>
#include <sstream>

#include <pluginterfaces/vst/ivstprocesscontext.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/base/ipersistent.h>
#include <public.sdk/source/vst/vstcomponent.h>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#ifdef _WIN32
#pragma comment(lib, "Comctl32.lib")
#endif

#define SMTG_OS_WINDOWS 1
#define SMTG_PLATFORM_WINDOWS 1
#define SMTG_EXPORT_MODULE_ENTRY 1

using namespace VST3::Hosting;
using namespace Steinberg;
using namespace Steinberg::Vst;

#ifdef _WIN32
namespace {
constexpr wchar_t kContainerWindowClassName[] = L"KJ_VST3_CONTAINER";
constexpr wchar_t kHeaderWindowClassName[] = L"KJ_VST3_HEADER";
constexpr wchar_t kFallbackWindowClassName[] = L"KJ_VST3_FALLBACK";
constexpr int kHeaderHeight = 56;
constexpr UINT_PTR kHeaderFallbackButtonId = 1001;
constexpr UINT_PTR kHeaderCloseButtonId = 1002;
constexpr UINT_PTR kFallbackListViewId = 2001;
constexpr UINT_PTR kFallbackSliderId = 2002;
constexpr int kFallbackSliderRange = 1000;

std::wstring Utf8ToWide(const std::string& value)
{
    if (value.empty())
        return {};

    int required = ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, nullptr, 0);
    if (required <= 0)
        return std::wstring(value.begin(), value.end());

    std::wstring result(static_cast<size_t>(required - 1), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, value.c_str(), -1, result.data(), required);
    return result;
}

std::wstring String128ToWide(const Steinberg::Vst::String128& value)
{
    std::wstring result;
    result.reserve(Steinberg::Vst::kString128Size);
    for (int i = 0; i < Steinberg::Vst::kString128Size; ++i)
    {
        Steinberg::Vst::TChar character = value[i];
        if (character == 0)
            break;
        result.push_back(static_cast<wchar_t>(character));
    }
    return result;
}
}
#endif

namespace kj {

#ifdef _WIN32
struct VST3Host::FallbackParameter
{
    Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
    Steinberg::Vst::ParameterInfo info {};
    double normalizedValue = 0.0;
};

class VST3Host::PlugFrame : public Steinberg::IPlugFrame
{
public:
    explicit PlugFrame(VST3Host& host) : host_(host) {}

    tresult PLUGIN_API queryInterface(const TUID iid, void** obj) override
    {
        if (!obj)
            return kInvalidArgument;

        *obj = nullptr;

        if (std::memcmp(iid, IPlugFrame::iid, sizeof(TUID)) == 0 || std::memcmp(iid, FUnknown::iid, sizeof(TUID)) == 0)
        {
            *obj = static_cast<IPlugFrame*>(this);
            addRef();
            return kResultOk;
        }

        return kNoInterface;
    }

    uint32 PLUGIN_API addRef() override
    {
        return ++refCount_;
    }

    uint32 PLUGIN_API release() override
    {
        uint32 newCount = --refCount_;
        if (newCount == 0)
            delete this;
        return newCount;
    }

    tresult PLUGIN_API resizeView(Steinberg::IPlugView* view, Steinberg::ViewRect* newSize) override
    {
        if (!newSize)
            return kInvalidArgument;

        if (!host_.applyViewRect(*newSize))
            return kResultFalse;

        Steinberg::IPlugView* targetView = view ? view : host_.view_.get();
        if (targetView)
        {
            Steinberg::ViewRect currentSize;
            bool sizeChanged = true;
            if (targetView->getSize(&currentSize) == kResultTrue)
            {
                sizeChanged = currentSize.left != newSize->left || currentSize.top != newSize->top ||
                              currentSize.right != newSize->right || currentSize.bottom != newSize->bottom;
            }

            if (sizeChanged)
            {
                Steinberg::ViewRect newRect = *newSize;
                targetView->onSize(&newRect);
            }
        }

        return kResultOk;
    }

private:
    std::atomic<uint32> refCount_ {1};
    VST3Host& host_;
};

bool VST3Host::ensureWindowClasses()
{
    if (windowClassesRegistered_)
        return true;

    HINSTANCE instance = ::GetModuleHandleW(nullptr);

    WNDCLASSEXW containerClass {};
    containerClass.cbSize = sizeof(containerClass);
    containerClass.style = CS_DBLCLKS;
    containerClass.lpfnWndProc = &VST3Host::ContainerWndProc;
    containerClass.cbClsExtra = 0;
    containerClass.cbWndExtra = sizeof(LONG_PTR);
    containerClass.hInstance = instance;
    containerClass.hCursor = ::LoadCursorW(nullptr, IDC_ARROW);
    containerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    containerClass.lpszClassName = kContainerWindowClassName;
    containerClass.hIcon = ::LoadIconW(nullptr, IDI_APPLICATION);
    containerClass.hIconSm = containerClass.hIcon;

    if (!::RegisterClassExW(&containerClass))
        return false;

    WNDCLASSEXW headerClass = containerClass;
    headerClass.lpfnWndProc = &VST3Host::HeaderWndProc;
    headerClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
    headerClass.lpszClassName = kHeaderWindowClassName;
    headerClass.hIcon = nullptr;
    headerClass.hIconSm = nullptr;
    if (!::RegisterClassExW(&headerClass))
        return false;

    WNDCLASSEXW fallbackClass = containerClass;
    fallbackClass.lpfnWndProc = &VST3Host::FallbackWndProc;
    fallbackClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    fallbackClass.lpszClassName = kFallbackWindowClassName;
    fallbackClass.hIcon = nullptr;
    fallbackClass.hIconSm = nullptr;
    if (!::RegisterClassExW(&fallbackClass))
        return false;

    windowClassesRegistered_ = true;
    return true;
}

bool VST3Host::ensureCommonControls()
{
    INITCOMMONCONTROLSEX initControls {};
    initControls.dwSize = sizeof(initControls);
    initControls.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    return ::InitCommonControlsEx(&initControls) != FALSE;
}

bool VST3Host::createContainerWindow(HWND parentWindow)
{
    if (!ensureWindowClasses() || !ensureCommonControls())
        return false;

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        if (parentWindow)
            ::SetWindowLongPtrW(containerWindow_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(parentWindow));
        return true;
    }

    HINSTANCE instance = ::GetModuleHandleW(nullptr);
    DWORD style = WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
    DWORD exStyle = WS_EX_TOOLWINDOW;

    int defaultWidth = 520;
    int defaultHeight = 420;
    RECT desired {0, 0, defaultWidth, defaultHeight};
    ::AdjustWindowRectEx(&desired, style, FALSE, exStyle);

    std::wstring title = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;

    containerWindow_ = ::CreateWindowExW(exStyle, kContainerWindowClassName, title.c_str(), style, CW_USEDEFAULT,
                                         CW_USEDEFAULT, desired.right - desired.left, desired.bottom - desired.top,
                                         parentWindow, nullptr, instance, this);

    return containerWindow_ != nullptr;
}

void VST3Host::closeContainerWindow()
{
    if (containerWindow_ && ::IsWindow(containerWindow_))
        ::ShowWindow(containerWindow_, SW_HIDE);
}

void VST3Host::onContainerCreated(HWND hwnd)
{
    containerWindow_ = hwnd;

    if (!headerFontsCreated_)
    {
        NONCLIENTMETRICSW metrics {sizeof(metrics)};
        if (::SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0))
        {
            headerTextFont_ = ::CreateFontIndirectW(&metrics.lfMessageFont);
            LOGFONTW titleFont = metrics.lfMessageFont;
            titleFont.lfWeight = FW_BOLD;
            headerTitleFont_ = ::CreateFontIndirectW(&titleFont);
            headerFontsCreated_ = true;
        }
        else
        {
            headerTextFont_ = reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            headerTitleFont_ = headerTextFont_;
            headerFontsCreated_ = false;
        }
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(::GetWindowLongPtrW(hwnd, GWLP_HINSTANCE));

    headerWindow_ = ::CreateWindowExW(0, kHeaderWindowClassName, L"", WS_CHILD | WS_VISIBLE, 0, 0, 0, kHeaderHeight,
                                      hwnd, nullptr, instance, this);

    contentWindow_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                       0, kHeaderHeight, 0, 0, hwnd, nullptr, instance, nullptr);
    ::SetWindowLongPtrW(contentWindow_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    ensurePluginViewHost();
    ensureFallbackWindow();
    updateHeaderTexts();
}

void VST3Host::onContainerResized(int width, int height)
{
    if (headerWindow_ && ::IsWindow(headerWindow_))
        ::MoveWindow(headerWindow_, 0, 0, width, kHeaderHeight, TRUE);

    int contentHeight = std::max(0, height - kHeaderHeight);
    if (contentWindow_ && ::IsWindow(contentWindow_))
        ::MoveWindow(contentWindow_, 0, kHeaderHeight, width, contentHeight, TRUE);

    if (pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        ::MoveWindow(pluginViewWindow_, 0, 0, width, contentHeight, TRUE);

    if (fallbackWindow_ && ::IsWindow(fallbackWindow_))
        ::MoveWindow(fallbackWindow_, 0, 0, width, contentHeight, TRUE);
}

void VST3Host::onContainerDestroyed()
{
    containerWindow_ = nullptr;
    headerWindow_ = nullptr;
    headerTitleStatic_ = nullptr;
    headerVendorStatic_ = nullptr;
    headerStatusStatic_ = nullptr;
    headerFallbackButton_ = nullptr;
    headerCloseButton_ = nullptr;
    contentWindow_ = nullptr;
    pluginViewWindow_ = nullptr;
    fallbackWindow_ = nullptr;
    fallbackListView_ = nullptr;
    fallbackSlider_ = nullptr;
    fallbackValueStatic_ = nullptr;

    if (headerFontsCreated_)
    {
        if (headerTitleFont_)
            ::DeleteObject(headerTitleFont_);
        if (headerTextFont_)
            ::DeleteObject(headerTextFont_);
        headerTitleFont_ = nullptr;
        headerTextFont_ = nullptr;
        headerFontsCreated_ = false;
    }

    fallbackVisible_ = false;
    fallbackSelectedIndex_ = -1;
    fallbackEditing_ = false;
    fallbackEditingParamId_ = Steinberg::Vst::kNoParamId;
}

void VST3Host::ensurePluginViewHost()
{
    if (!contentWindow_ || !::IsWindow(contentWindow_))
        return;

    if (!pluginViewWindow_)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        pluginViewWindow_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
                                             0, 0, 0, 0, contentWindow_, nullptr, instance, nullptr);
    }
}

bool VST3Host::applyViewRect(const Steinberg::ViewRect& rect)
{
    if (!pluginViewWindow_ || !::IsWindow(pluginViewWindow_))
        return false;

    const int width = std::max<int>(1, rect.getWidth());
    const int height = std::max<int>(1, rect.getHeight());

    BOOL moved = ::MoveWindow(pluginViewWindow_, 0, 0, width, height, TRUE);
    if (!moved)
    {
        moved = ::SetWindowPos(pluginViewWindow_, nullptr, 0, 0, width, height,
                               SWP_NOZORDER | SWP_NOACTIVATE);
    }

    if (!moved)
        return false;

    updateWindowSizeForContent(width, height);
    return true;
}

void VST3Host::updateWindowSizeForContent(int contentWidth, int contentHeight)
{
    if (!containerWindow_ || !::IsWindow(containerWindow_))
        return;

    int width = std::max(contentWidth, 200);
    int height = std::max(contentHeight, 150);

    RECT clientRect {0, 0, width, height + kHeaderHeight};
    DWORD style = static_cast<DWORD>(::GetWindowLongW(containerWindow_, GWL_STYLE));
    DWORD exStyle = static_cast<DWORD>(::GetWindowLongW(containerWindow_, GWL_EXSTYLE));
    ::AdjustWindowRectEx(&clientRect, style, FALSE, exStyle);

    ::SetWindowPos(containerWindow_, nullptr, 0, 0, clientRect.right - clientRect.left,
                   clientRect.bottom - clientRect.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
}

void VST3Host::updateHeaderTexts()
{
    std::wstring title = pluginNameW_.empty() ? std::wstring(L"VST3 Plug-in") : pluginNameW_;
    if (containerWindow_ && ::IsWindow(containerWindow_))
        ::SetWindowTextW(containerWindow_, title.c_str());

    if (headerTitleStatic_ && ::IsWindow(headerTitleStatic_))
        ::SetWindowTextW(headerTitleStatic_, title.c_str());

    std::wstring vendorText = pluginVendorW_.empty() ? std::wstring(L"Vendor: Unknown")
                                                    : std::wstring(L"Vendor: ") + pluginVendorW_;
    if (headerVendorStatic_ && ::IsWindow(headerVendorStatic_))
        ::SetWindowTextW(headerVendorStatic_, vendorText.c_str());

    std::wstring status;
    if (!view_)
        status = L"Fallback controls (no custom editor)";
    else if (fallbackVisible_)
        status = L"Fallback controls active";
    else
        status = L"Custom editor active";

    if (headerStatusStatic_ && ::IsWindow(headerStatusStatic_))
        ::SetWindowTextW(headerStatusStatic_, status.c_str());

    if (headerFallbackButton_ && ::IsWindow(headerFallbackButton_))
    {
        if (view_)
        {
            std::wstring buttonText = fallbackVisible_ ? std::wstring(L"Show Editor")
                                                       : std::wstring(L"Show Fallback");
            ::SetWindowTextW(headerFallbackButton_, buttonText.c_str());
            ::EnableWindow(headerFallbackButton_, TRUE);
        }
        else
        {
            ::SetWindowTextW(headerFallbackButton_, L"Fallback Only");
            ::EnableWindow(headerFallbackButton_, FALSE);
        }
    }
}

void VST3Host::handleHeaderCommand(UINT commandId)
{
    switch (commandId)
    {
    case kHeaderFallbackButtonId:
        if (view_)
        {
            showFallbackControls(!fallbackVisible_);
        }
        break;
    case kHeaderCloseButtonId:
        closeContainerWindow();
        break;
    default:
        break;
    }
}

void VST3Host::showFallbackControls(bool show)
{
    ensureFallbackWindow();
    ensurePluginViewHost();

    bool shouldShowFallback = show || !view_;
    if (shouldShowFallback != fallbackVisible_)
        resetFallbackEditState();

    fallbackVisible_ = shouldShowFallback;

    if (fallbackWindow_ && ::IsWindow(fallbackWindow_))
        ::ShowWindow(fallbackWindow_, fallbackVisible_ ? SW_SHOW : SW_HIDE);

    if (pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        ::ShowWindow(pluginViewWindow_, (!fallbackVisible_ && view_) ? SW_SHOW : SW_HIDE);

    if (fallbackVisible_)
    {
        refreshFallbackParameters();
        updateFallbackSlider(false);
        updateFallbackValueLabel();
    }

    if (view_ && viewAttached_ && !fallbackVisible_)
    {
        ViewRect rect {};
        if (view_->getSize(&rect) == kResultTrue)
            applyViewRect(rect);
    }
    else if (fallbackVisible_)
    {
        updateWindowSizeForContent(480, 360);
    }

    updateHeaderTexts();
}

void VST3Host::ensureFallbackWindow()
{
    if (!contentWindow_ || !::IsWindow(contentWindow_))
        return;

    if (!fallbackWindow_)
    {
        HINSTANCE instance = ::GetModuleHandleW(nullptr);
        fallbackWindow_ = ::CreateWindowExW(0, kFallbackWindowClassName, L"", WS_CHILD,
                                            0, 0, 0, 0, contentWindow_, nullptr, instance, this);
    }
}

void VST3Host::refreshFallbackParameters()
{
    fallbackParameters_.clear();

    if (!controller_)
    {
        if (fallbackListView_ && ::IsWindow(fallbackListView_))
            ::SendMessageW(fallbackListView_, LVM_DELETEALLITEMS, 0, 0);
        return;
    }

    const int32 parameterCount = controller_->getParameterCount();
    fallbackParameters_.reserve(parameterCount);

    for (int32 index = 0; index < parameterCount; ++index)
    {
        ParameterInfo info {};
        if (controller_->getParameterInfo(index, info) != kResultOk)
            continue;

        if ((info.flags & ParameterInfo::kIsReadOnly) != 0)
            continue;

        FallbackParameter parameter {};
        parameter.id = info.id;
        parameter.info = info;
        parameter.normalizedValue = controller_->getParamNormalized(info.id);
        fallbackParameters_.push_back(parameter);
    }

    if (!fallbackListView_ || !::IsWindow(fallbackListView_))
        return;

    ::SendMessageW(fallbackListView_, LVM_DELETEALLITEMS, 0, 0);

    LVITEMW item {};
    item.mask = LVIF_TEXT;

    int row = 0;
    for (const auto& parameter : fallbackParameters_)
    {
        std::wstring name = getParameterName(parameter);
        item.iItem = row;
        item.pszText = name.empty() ? const_cast<wchar_t*>(L"Parameter")
                                    : const_cast<wchar_t*>(name.c_str());
        int inserted = static_cast<int>(::SendMessageW(fallbackListView_, LVM_INSERTITEMW, 0,
                                                       reinterpret_cast<LPARAM>(&item)));
        if (inserted >= 0)
        {
            std::wstring value = getFallbackDisplayString(parameter);
            ::ListView_SetItemTextW(fallbackListView_, inserted, 1,
                                    value.empty() ? const_cast<wchar_t*>(L"")
                                                  : const_cast<wchar_t*>(value.c_str()));
        }
        ++row;
    }
}

void VST3Host::onFallbackParameterSelected(int index)
{
    if (index < 0 || index >= static_cast<int>(fallbackParameters_.size()))
    {
        fallbackSelectedIndex_ = -1;
        if (fallbackSlider_ && ::IsWindow(fallbackSlider_))
            ::EnableWindow(fallbackSlider_, FALSE);
        updateFallbackValueLabel();
        resetFallbackEditState();
        return;
    }

    fallbackSelectedIndex_ = index;
    resetFallbackEditState();

    if (fallbackSlider_ && ::IsWindow(fallbackSlider_))
    {
        ::EnableWindow(fallbackSlider_, TRUE);
        ::SendMessageW(fallbackSlider_, TBM_SETRANGE, TRUE, MAKELPARAM(0, kFallbackSliderRange));
        double normalized = std::clamp(fallbackParameters_[index].normalizedValue, 0.0, 1.0);
        LRESULT sliderPos = static_cast<LRESULT>(std::lround(normalized * kFallbackSliderRange));
        ::SendMessageW(fallbackSlider_, TBM_SETPOS, TRUE, sliderPos);
    }

    updateFallbackValueLabel();
}

void VST3Host::updateFallbackSlider(bool resetSelection)
{
    if (!fallbackListView_ || !::IsWindow(fallbackListView_))
        return;

    if (fallbackParameters_.empty())
    {
        ::EnableWindow(fallbackSlider_, FALSE);
        fallbackSelectedIndex_ = -1;
        updateFallbackValueLabel();
        return;
    }

    if (resetSelection || fallbackSelectedIndex_ < 0 ||
        fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
    {
        fallbackSelectedIndex_ = 0;
        ::ListView_SetItemState(fallbackListView_, fallbackSelectedIndex_, LVIS_SELECTED, LVIS_SELECTED);
        ::ListView_EnsureVisible(fallbackListView_, fallbackSelectedIndex_, FALSE);
    }

    onFallbackParameterSelected(fallbackSelectedIndex_);
}

void VST3Host::applyFallbackSliderChange(bool finalChange)
{
    if (!fallbackSlider_ || !::IsWindow(fallbackSlider_) || !controller_)
        return;

    if (fallbackSelectedIndex_ < 0 || fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
        return;

    auto& parameter = fallbackParameters_[fallbackSelectedIndex_];

    int sliderPosition = static_cast<int>(::SendMessageW(fallbackSlider_, TBM_GETPOS, 0, 0));
    double normalized = std::clamp(static_cast<double>(sliderPosition) / kFallbackSliderRange, 0.0, 1.0);

    if (!fallbackEditing_)
    {
        controller_->beginEdit(parameter.id);
        fallbackEditing_ = true;
        fallbackEditingParamId_ = parameter.id;
    }

    controller_->setParamNormalized(parameter.id, normalized);
    controller_->performEdit(parameter.id, normalized);
    if (component_)
        component_->setParamNormalized(parameter.id, normalized);

    parameter.normalizedValue = normalized;

    if (fallbackListView_ && ::IsWindow(fallbackListView_))
    {
        std::wstring value = getFallbackDisplayString(parameter);
        ::ListView_SetItemTextW(fallbackListView_, fallbackSelectedIndex_, 1,
                                value.empty() ? const_cast<wchar_t*>(L"")
                                              : const_cast<wchar_t*>(value.c_str()));
    }

    updateFallbackValueLabel();

    if (finalChange && fallbackEditing_)
    {
        controller_->endEdit(parameter.id);
        fallbackEditing_ = false;
        fallbackEditingParamId_ = Steinberg::Vst::kNoParamId;
    }
}

void VST3Host::updateFallbackValueLabel()
{
    if (!fallbackValueStatic_ || !::IsWindow(fallbackValueStatic_))
        return;

    if (fallbackSelectedIndex_ < 0 || fallbackSelectedIndex_ >= static_cast<int>(fallbackParameters_.size()))
    {
        ::SetWindowTextW(fallbackValueStatic_, L"No editable parameters");
        return;
    }

    const auto& parameter = fallbackParameters_[fallbackSelectedIndex_];
    std::wstring label = getParameterName(parameter);
    std::wstring value = getFallbackDisplayString(parameter);
    std::wstring units = String128ToWide(parameter.info.units);
    if (!value.empty())
    {
        label += L": ";
        label += value;
        if (!units.empty())
        {
            label += L" ";
            label += units;
        }
    }
    ::SetWindowTextW(fallbackValueStatic_, label.c_str());
}

void VST3Host::resetFallbackEditState()
{
    if (fallbackEditing_ && controller_ && fallbackEditingParamId_ != Steinberg::Vst::kNoParamId)
    {
        controller_->endEdit(fallbackEditingParamId_);
    }
    fallbackEditing_ = false;
    fallbackEditingParamId_ = Steinberg::Vst::kNoParamId;
}

std::wstring VST3Host::getFallbackDisplayString(const FallbackParameter& param) const
{
    if (!controller_)
        return {};

    Steinberg::Vst::String128 buffer {};
    if (controller_->normalizedParamToPlain(param.id, param.normalizedValue, buffer) == kResultOk)
    {
        std::wstring text = String128ToWide(buffer);
        if (!text.empty())
            return text;
    }

    std::wstringstream stream;
    stream << std::fixed << std::setprecision(3) << param.normalizedValue;
    return stream.str();
}

std::wstring VST3Host::getParameterName(const FallbackParameter& param) const
{
    std::wstring name = String128ToWide(param.info.title);
    if (!name.empty())
        return name;

    name = String128ToWide(param.info.shortTitle);
    if (!name.empty())
        return name;

    std::wstringstream stream;
    stream << L"Param " << param.id;
    return stream.str();
}

LRESULT CALLBACK VST3Host::ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
            host->onContainerCreated(hwnd);
        return 0;
    case WM_SIZE:
        if (host)
            host->onContainerResized(static_cast<int>(LOWORD(lParam)), static_cast<int>(HIWORD(lParam)));
        return 0;
    case WM_CLOSE:
        if (host)
        {
            host->closeContainerWindow();
            return 0;
        }
        break;
    case WM_DESTROY:
        if (host)
            host->onContainerDestroyed();
        return 0;
    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::HeaderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
        {
            host->headerWindow_ = hwnd;
            HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;

            host->headerTitleStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                         8, 6, 200, 20, hwnd, nullptr, instance, nullptr);
            host->headerVendorStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                          8, 26, 200, 18, hwnd, nullptr, instance, nullptr);
            host->headerStatusStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                          8, 44, 200, 14, hwnd, nullptr, instance, nullptr);

            host->headerFallbackButton_ = ::CreateWindowExW(0, L"BUTTON", L"",
                                                             WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                             0, 0, 120, 26, hwnd,
                                                             reinterpret_cast<HMENU>(kHeaderFallbackButtonId),
                                                             instance, nullptr);
            host->headerCloseButton_ = ::CreateWindowExW(0, L"BUTTON", L"Close",
                                                          WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                          0, 0, 80, 26, hwnd,
                                                          reinterpret_cast<HMENU>(kHeaderCloseButtonId),
                                                          instance, nullptr);

            HFONT titleFont = host->headerTitleFont_ ? host->headerTitleFont_
                                                      : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            HFONT textFont = host->headerTextFont_ ? host->headerTextFont_
                                                   : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));

            if (host->headerTitleStatic_)
                ::SendMessageW(host->headerTitleStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(titleFont), TRUE);
            if (host->headerVendorStatic_)
                ::SendMessageW(host->headerVendorStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerStatusStatic_)
                ::SendMessageW(host->headerStatusStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerFallbackButton_)
                ::SendMessageW(host->headerFallbackButton_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->headerCloseButton_)
                ::SendMessageW(host->headerCloseButton_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);

            host->updateHeaderTexts();
        }
        return 0;

    case WM_SIZE:
        if (host)
        {
            int width = static_cast<int>(LOWORD(lParam));
            int margin = 8;
            int buttonHeight = 26;
            int closeWidth = 80;
            int toggleWidth = 130;
            int buttonTop = (kHeaderHeight - buttonHeight) / 2;

            if (host->headerCloseButton_ && ::IsWindow(host->headerCloseButton_))
            {
                ::MoveWindow(host->headerCloseButton_, width - margin - closeWidth, buttonTop,
                             closeWidth, buttonHeight, TRUE);
            }

            if (host->headerFallbackButton_ && ::IsWindow(host->headerFallbackButton_))
            {
                int fallbackLeft = width - margin - closeWidth - margin - toggleWidth;
                ::MoveWindow(host->headerFallbackButton_, fallbackLeft, buttonTop,
                             toggleWidth, buttonHeight, TRUE);
            }

            int textRight = width - margin - closeWidth - margin - toggleWidth - margin;
            if (textRight < margin + 10)
                textRight = margin + 10;

            int textWidth = textRight - margin;
            if (host->headerTitleStatic_ && ::IsWindow(host->headerTitleStatic_))
                ::MoveWindow(host->headerTitleStatic_, margin, 6, textWidth, 18, TRUE);
            if (host->headerVendorStatic_ && ::IsWindow(host->headerVendorStatic_))
                ::MoveWindow(host->headerVendorStatic_, margin, 26, textWidth, 16, TRUE);
            if (host->headerStatusStatic_ && ::IsWindow(host->headerStatusStatic_))
                ::MoveWindow(host->headerStatusStatic_, margin, 44, textWidth, 14, TRUE);
        }
        return 0;

    case WM_COMMAND:
        if (host)
            host->handleHeaderCommand(LOWORD(wParam));
        return 0;

    case WM_DESTROY:
        if (host)
        {
            host->headerWindow_ = nullptr;
            host->headerTitleStatic_ = nullptr;
            host->headerVendorStatic_ = nullptr;
            host->headerStatusStatic_ = nullptr;
            host->headerFallbackButton_ = nullptr;
            host->headerCloseButton_ = nullptr;
        }
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK VST3Host::FallbackWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_NCCREATE)
    {
        auto* create = reinterpret_cast<LPCREATESTRUCTW>(lParam);
        auto* host = reinterpret_cast<VST3Host*>(create->lpCreateParams);
        ::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(host));
    }

    auto* host = reinterpret_cast<VST3Host*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg)
    {
    case WM_CREATE:
        if (host)
        {
            host->fallbackWindow_ = hwnd;
            HINSTANCE instance = reinterpret_cast<LPCREATESTRUCTW>(lParam)->hInstance;

            host->fallbackListView_ = ::CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                                                         WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                                         0, 0, 0, 0, hwnd,
                                                         reinterpret_cast<HMENU>(kFallbackListViewId),
                                                         instance, nullptr);
            if (host->fallbackListView_)
            {
                ::SendMessageW(host->fallbackListView_, LVM_SETEXTENDEDLISTVIEWSTYLE, 0,
                               LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_GRIDLINES);

                LVCOLUMNW column {};
                column.mask = LVCF_TEXT | LVCF_WIDTH;
                column.cx = 220;
                column.pszText = const_cast<wchar_t*>(L"Parameter");
                ::SendMessageW(host->fallbackListView_, LVM_INSERTCOLUMNW, 0, reinterpret_cast<LPARAM>(&column));
                column.cx = 160;
                column.pszText = const_cast<wchar_t*>(L"Value");
                ::SendMessageW(host->fallbackListView_, LVM_INSERTCOLUMNW, 1, reinterpret_cast<LPARAM>(&column));
            }

            host->fallbackSlider_ = ::CreateWindowExW(0, TRACKBAR_CLASSW, L"",
                                                       WS_CHILD | WS_VISIBLE | TBS_HORZ,
                                                       0, 0, 0, 0, hwnd,
                                                       reinterpret_cast<HMENU>(kFallbackSliderId),
                                                       instance, nullptr);
            host->fallbackValueStatic_ = ::CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE,
                                                            0, 0, 0, 0, hwnd, nullptr, instance, nullptr);

            HFONT textFont = host->headerTextFont_ ? host->headerTextFont_
                                                   : reinterpret_cast<HFONT>(::GetStockObject(DEFAULT_GUI_FONT));
            if (host->fallbackListView_)
                ::SendMessageW(host->fallbackListView_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->fallbackSlider_)
                ::SendMessageW(host->fallbackSlider_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);
            if (host->fallbackValueStatic_)
                ::SendMessageW(host->fallbackValueStatic_, WM_SETFONT, reinterpret_cast<WPARAM>(textFont), TRUE);

            host->refreshFallbackParameters();
            host->updateFallbackSlider(true);
            host->updateFallbackValueLabel();
        }
        return 0;

    case WM_SIZE:
        if (host)
        {
            int width = static_cast<int>(LOWORD(lParam));
            int height = static_cast<int>(HIWORD(lParam));
            int margin = 10;
            int sliderHeight = 30;
            int valueHeight = 22;
            int listHeight = std::max(0, height - (margin * 3) - sliderHeight - valueHeight);

            if (host->fallbackListView_ && ::IsWindow(host->fallbackListView_))
                ::MoveWindow(host->fallbackListView_, margin, margin, width - 2 * margin, listHeight, TRUE);

            int sliderTop = margin + listHeight + margin;
            if (host->fallbackSlider_ && ::IsWindow(host->fallbackSlider_))
                ::MoveWindow(host->fallbackSlider_, margin, sliderTop, width - 2 * margin, sliderHeight, TRUE);

            int valueTop = sliderTop + sliderHeight + 4;
            if (host->fallbackValueStatic_ && ::IsWindow(host->fallbackValueStatic_))
                ::MoveWindow(host->fallbackValueStatic_, margin, valueTop, width - 2 * margin, valueHeight, TRUE);
        }
        return 0;

    case WM_NOTIFY:
        if (host)
        {
            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header && header->idFrom == kFallbackListViewId && header->code == LVN_ITEMCHANGED)
            {
                auto* info = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if ((info->uChanged & LVIF_STATE) != 0 && (info->uNewState & LVIS_SELECTED) != 0)
                    host->onFallbackParameterSelected(info->iItem);
            }
        }
        return 0;

    case WM_HSCROLL:
        if (host && host->fallbackSlider_ && reinterpret_cast<HWND>(lParam) == host->fallbackSlider_)
        {
            const int scrollCode = LOWORD(wParam);
            bool finalChange = (scrollCode == TB_ENDTRACK || scrollCode == TB_THUMBPOSITION);
            host->applyFallbackSliderChange(finalChange);
            return 0;
        }
        break;

    case WM_DESTROY:
        if (host)
        {
            host->fallbackWindow_ = nullptr;
            host->fallbackListView_ = nullptr;
            host->fallbackSlider_ = nullptr;
            host->fallbackValueStatic_ = nullptr;
        }
        return 0;

    default:
        break;
    }

    return ::DefWindowProcW(hwnd, msg, wParam, lParam);
}
#endif

VST3Host::~VST3Host()
{
    unload();
}

bool VST3Host::load(const std::string& pluginPath)
{
    std::string error;
#ifdef _WIN32
    destroyPluginUI();
    pluginNameW_.clear();
    pluginVendorW_.clear();
    fallbackParameters_.clear();
    fallbackVisible_ = false;
    fallbackSelectedIndex_ = -1;
    resetFallbackEditState();
#endif

    auto module = Module::create(pluginPath, error);
    if (!module)
    {
        std::cerr << "Failed to load plugin: " << error << std::endl;
        return false;
    }

    auto factory = module->getFactory();
    auto classes = factory.classInfos();
    const VST3::Hosting::ClassInfo* componentClass = nullptr;
    const VST3::Hosting::ClassInfo* controllerClass = nullptr;

    for (const auto& c : classes)
    {
        if (c.category() == kVstAudioEffectClass)
            componentClass = &c;
        else if (c.category() == kVstComponentControllerClass)
            controllerClass = &c;
    }

    if (!componentClass)
    {
        std::cerr << "No valid audio effect found in " << pluginPath << std::endl;
        return false;
    }

#ifdef _WIN32
    pluginNameW_ = Utf8ToWide(componentClass->name());
    pluginVendorW_ = Utf8ToWide(componentClass->vendor());
    updateHeaderTexts();
#endif

    auto component = factory.createInstance<IComponent>(componentClass->ID());
    if (!component)
    {
        std::cerr << "Failed to instantiate component: " << componentClass->name() << std::endl;
        return false;
    }

    Steinberg::FObject hostContext;
    if (component->initialize(&hostContext) != kResultOk)
    {
        std::cerr << "[KJ] Component initialization failed.\n";
        return false;
    }

    auto controller = controllerClass ? factory.createInstance<IEditController>(controllerClass->ID()) : nullptr;
    if (controller)
    {
        if (controller->initialize(&hostContext) != kResultOk)
        {
            std::cerr << "[KJ] Controller initialization failed.\n";
            return false;
        }
    }

    Steinberg::FUID controllerClassId;
    if (controller)
    {
        if (auto persistent = Steinberg::FUnknownPtr<Steinberg::IPersistent>(controller))
        {
            Steinberg::FUID::String classIdString {};
            if (persistent->getClassID(classIdString) == kResultOk)
            {
                controllerClassId.fromString(classIdString);
            }
        }
    }

    if (!controllerClassId.isValid() && controllerClass)
    {
        controllerClassId = Steinberg::FUID::fromTUID(controllerClass->ID().data());
    }

    if (auto componentImpl = Steinberg::FObject::fromUnknown<Steinberg::Vst::Component>(component))
    {
        if (controllerClassId.isValid())
            componentImpl->setControllerClass(controllerClassId);
    }

    FUnknownPtr<IAudioProcessor> processor(component);
    if (processor)
        std::cout << "AudioProcessor loaded successfully\n";
    if (controller)
        std::cout << "EditController loaded successfully\n";

    module_ = module;
    component_ = component;
    processor_ = processor;
    controller_ = controller;
    view_ = nullptr;

#ifdef _WIN32
    refreshFallbackParameters();
    updateHeaderTexts();
#endif

    if (!controller_)
        std::cerr << "[KJ] Plugin has no controller class, skipping GUI.\n";

    return true;
}

bool VST3Host::prepare(double sampleRate, int blockSize)
{
    if (!processor_)
        return false;

    ProcessSetup setup{};
    setup.processMode = kRealtime;
    setup.symbolicSampleSize = kSample32;
    setup.maxSamplesPerBlock = blockSize;
    setup.sampleRate = sampleRate;

    auto result = processor_->setupProcessing(setup);
    if (result == kResultOk)
    {
        preparedSampleRate_ = sampleRate;
        preparedMaxBlockSize_ = blockSize;
        processingActive_ = true;
        return true;
    }

    return false;
}

void VST3Host::process(float** outputs, int numChannels, int numFrames)
{
    if (!processor_ || !processingActive_ || !outputs)
        return;

    for (int channel = 0; channel < numChannels; ++channel)
    {
        if (outputs[channel])
            std::fill(outputs[channel], outputs[channel] + numFrames, 0.0f);
    }

    AudioBusBuffers outputBus{};
    outputBus.numChannels = numChannels;
    outputBus.channelBuffers32 = outputs;

    ProcessData data{};
    data.processMode = kRealtime;
    data.symbolicSampleSize = kSample32;
    data.numSamples = numFrames;
    data.numOutputs = 1;
    data.outputs = &outputBus;

    processor_->process(data);
}

void VST3Host::unload()
{
    #ifdef _WIN32
    destroyPluginUI();
    #endif

    view_ = nullptr;

    controller_ = nullptr;
    processor_ = nullptr;
    component_ = nullptr;
    module_ = nullptr;

    preparedSampleRate_ = 0.0;
    preparedMaxBlockSize_ = 0;
    processingActive_ = false;
}

bool VST3Host::isPluginLoaded() const
{
    return component_ != nullptr;
}

void VST3Host::openEditor(void* hwnd)
{
    showPluginUI(hwnd);
}

void VST3Host::showPluginUI(void* parentHWND)
{
#ifndef _WIN32
    (void)parentHWND;
    std::cerr << "[KJ] Plugin UI is only supported on Windows.\\n";
    return;
#else
    if (!component_ || !controller_)
    {
        std::cerr << "[KJ] Cannot show GUI before plugin is fully loaded.\n";
        return;
    }

    HWND parentWindow = reinterpret_cast<HWND>(parentHWND);
    if (!createContainerWindow(parentWindow))
    {
        std::cerr << "[KJ] Failed to create container window for plug-in GUI.\n";
        return;
    }

    if (!view_ && controller_)
    {
        auto view = controller_->createView("editor");
        if (view)
        {
            view_ = view;
        }
        else
        {
            std::cerr << "[KJ] Plugin has no editor view. Using fallback controls.\n";
        }
    }

    if (view_)
    {
        ensurePluginViewHost();

        if (!plugFrame_)
            plugFrame_ = new PlugFrame(*this);

        if (!frameAttached_)
        {
            plugFrame_->addRef();
            if (view_->setFrame(plugFrame_) != kResultOk)
            {
                std::cerr << "[KJ] Failed to set VST3 plug frame. Falling back to generic controls.\n";
                plugFrame_->release();
                plugFrame_ = nullptr;
                frameAttached_ = false;
                view_ = nullptr;
            }
            else
            {
                frameAttached_ = true;
            }
        }

        if (view_ && frameAttached_ && !viewAttached_ && pluginViewWindow_ && ::IsWindow(pluginViewWindow_))
        {
            if (view_->attached(pluginViewWindow_, Steinberg::kPlatformTypeHWND) != kResultOk)
            {
                std::cerr << "[KJ] Failed to attach VST3 editor view. Falling back to generic controls.\n";
                view_->setFrame(nullptr);
                frameAttached_ = false;
                if (plugFrame_)
                {
                    plugFrame_->release();
                    plugFrame_ = nullptr;
                }
                view_ = nullptr;
            }
            else
            {
                viewAttached_ = true;
                ViewRect rect {};
                if (view_->getSize(&rect) != kResultTrue)
                {
                    rect.left = 0;
                    rect.top = 0;
                    rect.right = 400;
                    rect.bottom = 300;
                }
                applyViewRect(rect);
                view_->onSize(&rect);
            }
        }
    }

    refreshFallbackParameters();
    showFallbackControls(fallbackVisible_);

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        ::ShowWindow(containerWindow_, SW_SHOWNORMAL);
        ::SetWindowPos(containerWindow_, HWND_TOP, 0, 0, 0, 0, SWP_NOSIZE | SWP_NOMOVE);
        ::UpdateWindow(containerWindow_);
        ::SetForegroundWindow(containerWindow_);
    }
    std::cout << "[KJ] Plugin GUI displayed.\n";
#endif
}

#ifdef _WIN32
void VST3Host::destroyPluginUI()
{
    resetFallbackEditState();

    if (view_ && frameAttached_)
    {
        view_->setFrame(nullptr);
        frameAttached_ = false;
    }

    if (plugFrame_)
    {
        plugFrame_->release();
        plugFrame_ = nullptr;
    }

    if (view_ && viewAttached_)
    {
        view_->removed();
        viewAttached_ = false;
    }

    if (containerWindow_ && ::IsWindow(containerWindow_))
    {
        ::DestroyWindow(containerWindow_);
    }
    else
    {
        onContainerDestroyed();
    }

    fallbackVisible_ = false;
}
#endif

} // namespace kj
