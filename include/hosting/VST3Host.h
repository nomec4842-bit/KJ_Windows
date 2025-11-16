#pragma once

#ifdef _WIN32
#ifndef _WIN32_IE
#define _WIN32_IE 0x0600
#endif
#include <windows.h>
#include <commctrl.h>
#endif

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <cstdint>

#include "pluginterfaces/base/fplatform.h"
#include "pluginterfaces/base/funknown.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstcomponent.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/vstspeaker.h"
#include "base/source/fobject.h"

#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/eventlist.h"

namespace kj {

constexpr size_t VST3_STRING128_SIZE = 128;
using String128 = Steinberg::Vst::TChar[VST3_STRING128_SIZE];

class VST3Host {
public:
    VST3Host() = default;
    ~VST3Host();

    bool load(const std::string& path);
    void showPluginUI(void* parentWindowHandle);
    bool ShowPluginEditor();
    void unload();

    bool prepare(double sampleRate, int maxBlockSize);
    void process(float** inputs, int numInputChannels, float** outputs, int numOutputChannels, int numSamples);
    void process(float** outputs, int numChannels, int numSamples);

    struct HostTransportState {
        double samplePosition = 0.0;
        double tempo = 120.0;
        Steinberg::int32 timeSigNum = 4;
        Steinberg::int32 timeSigDen = 4;
        bool playing = false;
    };

    void setTransportState(const HostTransportState& state);

    void queueNoteEvent(const Steinberg::Vst::Event& ev);

    bool saveState(std::vector<uint8_t>& outState) const;
    bool loadState(const uint8_t* data, size_t size);

    void openEditor(void* nativeWindowHandle);

    bool isPluginLoaded() const;

private:
    struct PendingParameterChange {
        Steinberg::Vst::ParamID id {Steinberg::Vst::kNoParamId};
        Steinberg::Vst::ParamValue value {0.0};
    };

#ifdef _WIN32
    struct FallbackParameter {
        Steinberg::Vst::ParameterInfo info {};
        Steinberg::Vst::ParamValue normalizedValue = 0.0;
    };

    class PlugFrame;
#endif

    class ComponentHandler;

#ifdef _WIN32
    void ClosePluginEditor();
    void destroyPluginUI();
    bool ensureWindowClasses();
    bool ensureCommonControls();
    bool createContainerWindow(HWND parentWindow);
    void closeContainerWindow();
    void onContainerCreated(HWND hwnd);
    void onContainerResized(int width, int height);
    void onContainerDestroyed();
    void ensurePluginViewHost();
    bool AttachView(Steinberg::IPlugView* view, HWND parentWindow);
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
    void syncFallbackParameterValue(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue value);
    bool resizePluginViewWindow(HWND window, const Steinberg::ViewRect& rect, bool adjustContainer);
    void storeCurrentViewRect(const Steinberg::ViewRect& rect);
    void clearCurrentViewRect();
    bool handleKeyDown(WPARAM wParam, LPARAM lParam);
    bool handleKeyUp(WPARAM wParam, LPARAM lParam);
    char16_t translateVirtualKey(WPARAM wParam, LPARAM lParam) const;
    int16_t queryKeyModifiers() const;
    void startViewIdleTimer();
    void stopViewIdleTimer();
    static LRESULT CALLBACK ContainerWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK HeaderWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK FallbackWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK PluginViewHostWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
    static LRESULT CALLBACK StandaloneEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
#endif

    void queueParameterChange(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue value, bool notifyController = true);
    void onControllerParameterChanged(Steinberg::Vst::ParamID paramId, Steinberg::Vst::ParamValue value);
    void onRestartComponent(Steinberg::int32 flags);
    void onComponentRequestOpenEditor(const char* viewType);
    bool ensureViewForRequestedType();

    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_ = nullptr;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    ComponentHandler* componentHandler_ = nullptr;

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
    Steinberg::Vst::ParamID fallbackEditingParamId_ = 0;
    std::atomic<bool> standaloneEditorThreadRunning_ {false};
    std::atomic<bool> standaloneEditorThreadShouldExit_ {false};
    std::thread standaloneEditorThread_;
    Steinberg::IPtr<Steinberg::IPlugView> standaloneEditorView_;
    HWND standaloneEditorWindow_ = nullptr;
    mutable std::mutex standaloneEditorMutex_;
    bool viewIdleTimerActive_ = false;
#endif

    double preparedSampleRate_ = 0.0;
    int preparedMaxBlockSize_ = 0;
    bool processingActive_ = false;
    Steinberg::int32 mainInputBusIndex_ = -1;
    Steinberg::int32 mainOutputBusIndex_ = -1;
    Steinberg::Vst::SpeakerArrangement inputArrangement_ = Steinberg::Vst::SpeakerArr::kEmpty;
    Steinberg::Vst::SpeakerArrangement outputArrangement_ = Steinberg::Vst::SpeakerArr::kEmpty;

    Steinberg::Vst::ParameterChanges inputParameterChanges_;
    std::vector<PendingParameterChange> pendingParameterChanges_;
    mutable std::mutex parameterMutex_;
    Steinberg::Vst::EventList inputEvents_;
    std::vector<Steinberg::Vst::Event> pendingEvents_;
    mutable std::mutex eventMutex_;
    Steinberg::Vst::ProcessContext processContext_ {};

    std::string requestedViewType_ {Steinberg::Vst::ViewType::kEditor};
    std::string currentViewType_;

#ifdef _WIN32
    std::wstring pluginNameW_;
    std::wstring pluginVendorW_;
    std::vector<FallbackParameter> fallbackParameters_;
    HWND lastParentWindow_ = nullptr;
    Steinberg::ViewRect currentViewRect_ {};
    bool hasCurrentViewRect_ = false;
#endif
};
} // namespace kj
