#pragma once

#ifdef _WIN32

#include <memory>
#include <mutex>
#include <string>
#include <windows.h>

#include "pluginterfaces/gui/iplugview.h"
#include "hosting/VST3PlugFrame.h"

namespace kj {

class VST3Host;
class VSTGuiThread;

class VSTEditorWindow : public std::enable_shared_from_this<VSTEditorWindow>
{
public:
    static std::shared_ptr<VSTEditorWindow> create(const std::shared_ptr<VST3Host>& host);

    struct Deleter
    {
        void operator()(VSTEditorWindow* window) const { delete window; }
    };

    VSTEditorWindow(const VSTEditorWindow&) = delete;
    VSTEditorWindow& operator=(const VSTEditorWindow&) = delete;

    void show();
    void close();

private:
    explicit VSTEditorWindow(const std::shared_ptr<VST3Host>& host);
    ~VSTEditorWindow() = default;

    void showOnGuiThread();
    void destroyOnGuiThread();
    bool createWindowInternal();
    bool createWindow();
    void Show();
    void detachView();
    void onResize(UINT width, UINT height);
    void focus();

    static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static constexpr const wchar_t* kWindowClass = L"KJ_VST3_EDITOR";

    std::weak_ptr<VST3Host> host_;
    std::wstring title_;

    Steinberg::IPtr<Steinberg::IPlugView> view_;
    Steinberg::IPtr<PlugFrame> plugFrame_;
    std::string platformType_;
    Steinberg::ViewRect lastRect_ {};

    HWND hwnd_ = nullptr;
    bool attached_ = false;
};

} // namespace kj

#endif // _WIN32

