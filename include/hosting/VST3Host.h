#pragma once

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

// Steinberg base + VST interfaces
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "base/source/fobject.h" // for Steinberg::IPtr

// VST3 hosting layer (module helper)
#include "public.sdk/source/vst/hosting/module.h"

#include <memory>
#include <string>
#include <vector>

namespace kj {

class VST3Host {
public:
    VST3Host() = default;
    ~VST3Host();

    bool load(const std::string& path);
    void showPluginUI(void* parentWindowHandle);
    void unload();

    bool prepare(double sampleRate, int maxBlockSize);
    void process(float** outputs, int numChannels, int numSamples);

    void openEditor(void* nativeWindowHandle);

    bool isPluginLoaded() const;

private:
#ifdef _WIN32
    struct FallbackParameter {
        std::string name;
        Steinberg::Vst::ParamID id;
        double value = 0.0;
        bool isBoolean = false;
        Steinberg::Vst::ParamValue defaultValue = 0.0;
    };
    class PlugFrame;
    void destroyPluginUI();
    bool ensureWindowClasses();
    bool ensureCommonControls();
    bool createContainerWindow(HWND parentWindow);
    void closeContainerWindow();
    void onContainerCreated(HWND hwnd);
    void onContainerResized(int width, int height);
    void onContainerDestroyed();
    void ensurePluginViewHost();
    bool applyViewRect(const Steinberg::ViewRect& rect);
    void updateWindowSizeForContent(int contentWidth, int contentHeight);
    void updateHeaderTexts();
    void handleHeaderCommand(UINT commandId);
    void showFallbackControls(bool show);
    void ensureFallbackWindow();
    void refreshFallbackParameters();
    void onFallbackParameterSelected(int index);
    void updateFallbackSlider(bool resetSelection);
    void applyFallbackSliderChange(bool finalChange);
    void updateFallbackValueLabel();
    void resetFallbackEditState();
    std::wstring getFallbackDisplayString(const FallbackParameter& param) const;
    std::wstring getParameterName(const FallbackParameter& param) const;
    static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HeaderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK FallbackWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_;
    Steinberg::IPtr<Steinberg::IPlugView> view_;

#ifdef _WIN32
    PlugFrame* plugFrame_ = nullptr;
    HWND containerWindow_ = nullptr;
    HWND headerWindow_ = nullptr;
    HWND headerTitleStatic_ = nullptr;
    HWND headerVendorStatic_ = nullptr;
    HWND headerStatusStatic_ = nullptr;
    HWND headerFallbackButton_ = nullptr;
    HWND headerCloseButton_ = nullptr;
    HWND contentWindow_ = nullptr;
    HWND pluginViewWindow_ = nullptr;
    HWND fallbackWindow_ = nullptr;
    HWND fallbackListView_ = nullptr;
    HWND fallbackSlider_ = nullptr;
    HWND fallbackValueStatic_ = nullptr;
    HFONT headerTitleFont_ = nullptr;
    HFONT headerTextFont_ = nullptr;
    bool headerFontsCreated_ = false;
    bool windowClassesRegistered_ = false;
    bool frameAttached_ = false;
    bool viewAttached_ = false;
    bool fallbackVisible_ = false;
    int fallbackSelectedIndex_ = -1;
    bool fallbackEditing_ = false;
    Steinberg::Vst::ParamID fallbackEditingParamId_ = Steinberg::Vst::kNoParamId;
#endif

    double preparedSampleRate_ = 0.0;
    int preparedMaxBlockSize_ = 0;
    bool processingActive_ = false;

#ifdef _WIN32
    std::wstring pluginNameW_;
    std::wstring pluginVendorW_;
    std::vector<FallbackParameter> fallbackParameters_;
#endif
};

} // namespace kj
