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
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
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

#ifdef _WIN32
class PlugFrame;
#endif

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
    void renderAudio(float** out, int numChannels, int numSamples);

    struct HostTransportState {
        double samplePosition = 0.0;
        double tempo = 120.0;
        Steinberg::int32 timeSigNum = 4;
        Steinberg::int32 timeSigDen = 4;
        bool playing = false;
    };

    void setTransportState(const HostTransportState& state);

    void queueEvent(const Steinberg::Vst::Event& ev);
    void queueNoteEvent(const Steinberg::Vst::Event& ev);

    void setOwningTrackId(int trackId) { owningTrackId_.store(trackId, std::memory_order_release); }

    bool saveState(std::vector<uint8_t>& outState) const;
    bool loadState(const uint8_t* data, size_t size);

    void openEditor(void* nativeWindowHandle);
    void asyncLoadPluginEditor(void* parentWindowHandle);

    bool isPluginLoaded() const;
    bool isPluginReady() const;
    bool isPluginLoading() const;
    bool waitUntilReady();
    bool waitForPluginReady();

#ifdef _WIN32
    PlugFrame* getPlugFrame() const { return plugFrame_; }
    void setPlugFrame(PlugFrame* frame);
    Steinberg::IPlugView* getView() const { return view_.get(); }
#endif

private:
    template <typename T>
    class SpscRingBuffer
    {
    public:
        SpscRingBuffer() = default;
        explicit SpscRingBuffer(size_t capacity) { reset(capacity); }

        void reset(size_t capacity)
        {
            buffer_.assign(capacity > 0 ? capacity + 1 : 0, {});
            head_.store(0, std::memory_order_release);
            tail_.store(0, std::memory_order_release);
            capacity_ = buffer_.size();
        }

        [[nodiscard]] size_t capacity() const { return capacity_ > 0 ? capacity_ - 1 : 0; }

        void clear()
        {
            head_.store(0, std::memory_order_release);
            tail_.store(0, std::memory_order_release);
        }

        bool push(const T& value)
        {
            return emplace(value);
        }

        bool push(T&& value)
        {
            return emplace(std::move(value));
        }

        template <typename U>
        bool emplace(U&& value)
        {
            if (capacity_ == 0)
                return false;

            const size_t head = head_.load(std::memory_order_relaxed);
            const size_t next = increment(head);
            if (next == tail_.load(std::memory_order_acquire))
                return false;

            buffer_[head] = std::forward<U>(value);
            head_.store(next, std::memory_order_release);
            return true;
        }

        template <typename U>
        void pushOverwrite(U&& value)
        {
            if (capacity_ == 0)
                return;

            size_t head = head_.load(std::memory_order_relaxed);
            size_t next = increment(head);
            if (next == tail_.load(std::memory_order_acquire))
            {
                tail_.store(increment(tail_.load(std::memory_order_relaxed)), std::memory_order_release);
            }

            buffer_[head] = std::forward<U>(value);
            head_.store(next, std::memory_order_release);
        }

        size_t popAll(std::vector<T>& out)
        {
            const size_t availableCapacity = capacity();
            if (out.capacity() < availableCapacity)
                out.reserve(availableCapacity);
            out.clear();

            size_t tail = tail_.load(std::memory_order_relaxed);
            const size_t head = head_.load(std::memory_order_acquire);
            size_t count = 0;
            while (tail != head)
            {
                out.push_back(buffer_[tail]);
                tail = increment(tail);
                ++count;
            }
            tail_.store(tail, std::memory_order_release);
            return count;
        }

    private:
        size_t increment(size_t index) const { return (index + 1) % capacity_; }

        std::vector<T> buffer_;
        size_t capacity_ = 0;
        std::atomic<size_t> head_ {0};
        std::atomic<size_t> tail_ {0};
    };

    struct PendingParameterChange {
        Steinberg::Vst::ParamID id {Steinberg::Vst::kNoParamId};
        Steinberg::Vst::ParamValue value {0.0};
    };

#ifdef _WIN32
    struct FallbackParameter {
        Steinberg::Vst::ParameterInfo info {};
        Steinberg::Vst::ParamValue normalizedValue = 0.0;
    };
#endif

#ifdef _WIN32
    friend class PlugFrame;
#endif

    class ComponentHandler;

    class NonRealtimeScope;

#ifdef _WIN32
#define WM_KJ_OPENEDITOR (WM_USER + 0x200)
    void ClosePluginEditor();
    void destroyPluginUI();
    bool ensureEditorWindowClass();
    bool ensureWindowClasses();
    bool ensureCommonControls();
    bool createContainerWindow(HWND parentWindow);
    void closeContainerWindow();
    void onContainerCreated(HWND hwnd);
    void onContainerResized(int width, int height);
    void onContainerDestroyed();
    HWND ensurePluginViewHost();
    void onIdleTimer();
    bool AttachView(Steinberg::IPlugView* view, HWND parentWindow);
    void cleanupEditorWindowResources();
    bool applyViewRect(HWND hostWindow, const Steinberg::ViewRect& rect);
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
    void onOpenEditorMessage(HWND hwnd);
    static LRESULT CALLBACK PluginEditorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);
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
    bool createViewForRequestedType(const char* preferredType, Steinberg::IPtr<Steinberg::IPlugView>& outView,
                                    std::string& usedType,
                                    Steinberg::Vst::IEditController* controllerOverride = nullptr);
    void processInternal(float** inputs, int numInputChannels, float** outputs, int numOutputChannels, int numSamples,
                         const std::vector<PendingParameterChange>& changes,
                         const std::vector<Steinberg::Vst::Event>& events);
    void unloadLocked();
    void suspendProcessing();
    void resumeProcessing();
    void waitForProcessingToComplete();
    void markLoadStarted();
    void markLoadFinished(bool success);

    VST3::Hosting::Module::Ptr module_;
    Steinberg::IPtr<Steinberg::Vst::IComponent> component_ = nullptr;
    Steinberg::IPtr<Steinberg::Vst::IAudioProcessor> processor_;
    Steinberg::IPtr<Steinberg::Vst::IEditController> controller_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> view_;
    bool controllerInitialized_ = false;
    ComponentHandler* componentHandler_ = nullptr;

#ifdef _WIN32
    PlugFrame* plugFrame_ = nullptr;
    Steinberg::IPtr<Steinberg::IPlugView> editorView_;
    HWND containerWindow_ = nullptr;
    HWND headerWindow_ = nullptr;
    HWND headerTitleStatic_ = nullptr;
    HWND headerVendorStatic_ = nullptr;
    HWND headerStatusStatic_ = nullptr;
    HWND headerFallbackButton_ = nullptr;
    HWND headerCloseButton_ = nullptr;
    HWND contentWindow_ = nullptr;
    HWND viewHostWindow_ = nullptr;
    HWND fallbackWindow_ = nullptr;
    HWND fallbackListView_ = nullptr;
    HWND fallbackSlider_ = nullptr;
    HWND fallbackValueStatic_ = nullptr;
    HFONT headerTitleFont_ = nullptr;
    HFONT headerTextFont_ = nullptr;
    bool headerFontsCreated_ = false;
    bool frameAttached_ = false;
    bool viewAttached_ = false;
    UINT_PTR idleTimerId_ = 0;
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
#endif

    double preparedSampleRate_ = 0.0;
    int preparedMaxBlockSize_ = 0;
    bool processingActive_ = false;
    Steinberg::int32 mainInputBusIndex_ = -1;
    Steinberg::int32 mainOutputBusIndex_ = -1;
    Steinberg::Vst::SpeakerArrangement inputArrangement_ = Steinberg::Vst::SpeakerArr::kEmpty;
    Steinberg::Vst::SpeakerArrangement outputArrangement_ = Steinberg::Vst::SpeakerArr::kEmpty;
    mutable std::mutex processMutex_;
    std::atomic<bool> processingSuspended_ {false};
    std::atomic<uint32_t> activeProcessCount_ {0};

    Steinberg::Vst::ParameterChanges inputParameterChanges_;
    SpscRingBuffer<PendingParameterChange> parameterChangeQueue_ {512};
    std::vector<PendingParameterChange> processParameterChanges_;
    Steinberg::Vst::EventList inputEventList_;
    SpscRingBuffer<Steinberg::Vst::Event> eventQueue_ {512};
    std::vector<Steinberg::Vst::Event> processEvents_;
    Steinberg::Vst::ProcessContext processContext_ {};

    std::vector<std::vector<float>> internalIn_;
    std::vector<std::vector<float>> internalOut_;
    std::vector<float*> inputChannelPointers_;
    std::vector<float*> outputChannelPointers_;

    std::string requestedViewType_ {Steinberg::Vst::ViewType::kEditor};
    std::string currentViewType_;
    mutable std::mutex viewMutex_;

    mutable std::mutex loadingMutex_;
    std::condition_variable loadingCv_;
    bool loadingInProgress_ = false;
    bool pluginReady_ = false;
    std::atomic<int> owningTrackId_ {0};
    std::filesystem::path pluginPath_;

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
