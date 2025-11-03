#include "gui/compressor_window.h"

#include "core/tracks.h"
#include "gui/gui_main.h"

#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

void notifyEffectsWindowTrackValuesChanged(int trackId);

namespace
{

constexpr wchar_t kCompressorWindowClassName[] = L"KJCompressorWindow";
constexpr int kDefaultWindowWidth = 360;
constexpr int kDefaultWindowHeight = 320;

constexpr int kThresholdSliderMin = -60;
constexpr int kThresholdSliderMax = 0;
constexpr int kRatioSliderMin = 10;   // Represents 1.0
constexpr int kRatioSliderMax = 200;  // Represents 20.0
constexpr int kAttackSliderMin = 1;   // 1 ms
constexpr int kAttackSliderMax = 1000; // 1000 ms
constexpr int kReleaseSliderMin = 10;  // 10 ms
constexpr int kReleaseSliderMax = 4000; // 4000 ms

HWND gCompressorWindow = nullptr;
bool gCompressorWindowClassRegistered = false;

struct CompressorWindowState
{
    int trackId = 0;
    HWND trackLabel = nullptr;
    HWND enableCheckbox = nullptr;
    HWND thresholdLabel = nullptr;
    HWND thresholdSlider = nullptr;
    HWND thresholdValueLabel = nullptr;
    HWND ratioLabel = nullptr;
    HWND ratioSlider = nullptr;
    HWND ratioValueLabel = nullptr;
    HWND attackLabel = nullptr;
    HWND attackSlider = nullptr;
    HWND attackValueLabel = nullptr;
    HWND releaseLabel = nullptr;
    HWND releaseSlider = nullptr;
    HWND releaseValueLabel = nullptr;
};

const Track* findTrackById(const std::vector<Track>& tracks, int trackId)
{
    for (const auto& track : tracks)
    {
        if (track.id == trackId)
            return &track;
    }
    return nullptr;
}

std::wstring toWide(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

void setControlFont(HWND control, HFONT font)
{
    if (control)
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

void compressorWindowApplyFont(const CompressorWindowState& state, HFONT font)
{
    const HWND controls[] = {
        state.trackLabel,
        state.enableCheckbox,
        state.thresholdLabel,
        state.thresholdSlider,
        state.thresholdValueLabel,
        state.ratioLabel,
        state.ratioSlider,
        state.ratioValueLabel,
        state.attackLabel,
        state.attackSlider,
        state.attackValueLabel,
        state.releaseLabel,
        state.releaseSlider,
        state.releaseValueLabel,
    };

    for (HWND control : controls)
        setControlFont(control, font);
}

void compressorWindowLayout(HWND hwnd, CompressorWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int labelHeight = 20;
    const int checkboxHeight = 22;
    const int sliderHeight = 32;
    const int valueLabelWidth = 80;
    const int sliderSpacing = 8;
    const int sectionSpacing = 14;

    int contentWidth = std::max(width - padding * 2, 120);
    int currentY = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, padding, currentY, contentWidth, labelHeight, TRUE);
        currentY += labelHeight + sliderSpacing;
    }

    if (state->enableCheckbox)
    {
        MoveWindow(state->enableCheckbox, padding, currentY, contentWidth, checkboxHeight, TRUE);
        currentY += checkboxHeight + sectionSpacing;
    }

    auto layoutSlider = [&](HWND label, HWND slider, HWND valueLabel)
    {
        if (!label || !valueLabel || !slider)
            return;

        MoveWindow(label, padding, currentY, contentWidth - valueLabelWidth, labelHeight, TRUE);
        MoveWindow(valueLabel, padding + contentWidth - valueLabelWidth, currentY, valueLabelWidth, labelHeight, TRUE);
        currentY += labelHeight + sliderSpacing;
        MoveWindow(slider, padding, currentY, contentWidth, sliderHeight, TRUE);
        currentY += sliderHeight + sectionSpacing;
    };

    layoutSlider(state->thresholdLabel, state->thresholdSlider, state->thresholdValueLabel);
    layoutSlider(state->ratioLabel, state->ratioSlider, state->ratioValueLabel);
    layoutSlider(state->attackLabel, state->attackSlider, state->attackValueLabel);
    layoutSlider(state->releaseLabel, state->releaseSlider, state->releaseValueLabel);

    InvalidateRect(hwnd, nullptr, TRUE);
}

void setValueLabel(HWND control, const std::wstring& text)
{
    if (!control)
        return;
    SetWindowTextW(control, text.c_str());
}

void disableSliderGroup(HWND slider, HWND valueLabel)
{
    if (slider)
    {
        EnableWindow(slider, FALSE);
        SendMessageW(slider, TBM_SETPOS, TRUE, 0);
    }
    if (valueLabel)
        SetWindowTextW(valueLabel, L"-");
}

void compressorWindowSyncControls(HWND hwnd, CompressorWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"Select a track from the Effects window.");
        if (state->enableCheckbox)
            EnableWindow(state->enableCheckbox, FALSE);
        disableSliderGroup(state->thresholdSlider, state->thresholdValueLabel);
        disableSliderGroup(state->ratioSlider, state->ratioValueLabel);
        disableSliderGroup(state->attackSlider, state->attackValueLabel);
        disableSliderGroup(state->releaseSlider, state->releaseValueLabel);
        return;
    }

    auto tracks = getTracks();
    const Track* track = findTrackById(tracks, trackId);
    if (!track)
    {
        state->trackId = 0;
        compressorWindowSyncControls(hwnd, state);
        return;
    }

    std::wstring trackName = toWide(track->name);
    if (trackName.empty())
        trackName = L"Unnamed Track";
    if (state->trackLabel)
        SetWindowTextW(state->trackLabel, trackName.c_str());

    bool enabled = trackGetCompressorEnabled(trackId);
    float threshold = trackGetCompressorThresholdDb(trackId);
    float ratio = trackGetCompressorRatio(trackId);
    float attackSeconds = trackGetCompressorAttack(trackId);
    float releaseSeconds = trackGetCompressorRelease(trackId);

    if (state->enableCheckbox)
    {
        EnableWindow(state->enableCheckbox, TRUE);
        SendMessageW(state->enableCheckbox, BM_SETCHECK, enabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    auto clampInt = [](int value, int min, int max) { return std::clamp(value, min, max); };

    if (state->thresholdSlider)
    {
        EnableWindow(state->thresholdSlider, TRUE);
        int pos = clampInt(static_cast<int>(std::lround(threshold)), kThresholdSliderMin, kThresholdSliderMax);
        SendMessageW(state->thresholdSlider, TBM_SETPOS, TRUE, pos);
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(0) << threshold << L" dB";
        setValueLabel(state->thresholdValueLabel, stream.str());
    }

    if (state->ratioSlider)
    {
        EnableWindow(state->ratioSlider, TRUE);
        int pos = clampInt(static_cast<int>(std::lround(ratio * 10.0f)), kRatioSliderMin, kRatioSliderMax);
        SendMessageW(state->ratioSlider, TBM_SETPOS, TRUE, pos);
        std::wostringstream stream;
        stream << std::fixed << std::setprecision(1) << ratio << L":1";
        setValueLabel(state->ratioValueLabel, stream.str());
    }

    if (state->attackSlider)
    {
        EnableWindow(state->attackSlider, TRUE);
        int pos = clampInt(static_cast<int>(std::lround(attackSeconds * 1000.0f)), kAttackSliderMin, kAttackSliderMax);
        SendMessageW(state->attackSlider, TBM_SETPOS, TRUE, pos);
        std::wostringstream stream;
        stream << pos << L" ms";
        setValueLabel(state->attackValueLabel, stream.str());
    }

    if (state->releaseSlider)
    {
        EnableWindow(state->releaseSlider, TRUE);
        int pos = clampInt(static_cast<int>(std::lround(releaseSeconds * 1000.0f)), kReleaseSliderMin, kReleaseSliderMax);
        SendMessageW(state->releaseSlider, TBM_SETPOS, TRUE, pos);
        std::wostringstream stream;
        stream << pos << L" ms";
        setValueLabel(state->releaseValueLabel, stream.str());
    }
}

CompressorWindowState* getCompressorWindowState(HWND hwnd)
{
    return reinterpret_cast<CompressorWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void createSliderWithLabels(HWND parent,
                            HINSTANCE instance,
                            const wchar_t* labelText,
                            HWND& label,
                            HWND& slider,
                            HWND& valueLabel,
                            int rangeMin,
                            int rangeMax,
                            int ticFreq)
{
    label = CreateWindowExW(0,
                            L"STATIC",
                            labelText,
                            WS_CHILD | WS_VISIBLE,
                            0,
                            0,
                            100,
                            20,
                            parent,
                            nullptr,
                            instance,
                            nullptr);

    valueLabel = CreateWindowExW(0,
                                 L"STATIC",
                                 L"-",
                                 WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                 0,
                                 0,
                                 80,
                                 20,
                                 parent,
                                 nullptr,
                                 instance,
                                 nullptr);

    slider = CreateWindowExW(0,
                             TRACKBAR_CLASSW,
                             L"",
                             WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                             0,
                             0,
                             100,
                             30,
                             parent,
                             nullptr,
                             instance,
                             nullptr);
    if (slider)
    {
        SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
        if (ticFreq > 0)
            SendMessageW(slider, TBM_SETTICFREQ, ticFreq, 0);
    }
}

LRESULT CALLBACK CompressorWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    CompressorWindowState* state = getCompressorWindowState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* newState = new CompressorWindowState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));
        state = newState;

        HINSTANCE instance = reinterpret_cast<LPCREATESTRUCT>(lParam)->hInstance;

        state->trackLabel = CreateWindowExW(0,
                                            L"STATIC",
                                            L"-",
                                            WS_CHILD | WS_VISIBLE,
                                            0,
                                            0,
                                            100,
                                            20,
                                            hwnd,
                                            nullptr,
                                            instance,
                                            nullptr);

        state->enableCheckbox = CreateWindowExW(0,
                                                L"BUTTON",
                                                L"Enable compressor",
                                                WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX,
                                                0,
                                                0,
                                                140,
                                                20,
                                                hwnd,
                                                nullptr,
                                                instance,
                                                nullptr);

        createSliderWithLabels(hwnd,
                               instance,
                               L"Threshold",
                               state->thresholdLabel,
                               state->thresholdSlider,
                               state->thresholdValueLabel,
                               kThresholdSliderMin,
                               kThresholdSliderMax,
                               5);

        createSliderWithLabels(hwnd,
                               instance,
                               L"Ratio",
                               state->ratioLabel,
                               state->ratioSlider,
                               state->ratioValueLabel,
                               kRatioSliderMin,
                               kRatioSliderMax,
                               10);

        createSliderWithLabels(hwnd,
                               instance,
                               L"Attack",
                               state->attackLabel,
                               state->attackSlider,
                               state->attackValueLabel,
                               kAttackSliderMin,
                               kAttackSliderMax,
                               100);

        createSliderWithLabels(hwnd,
                               instance,
                               L"Release",
                               state->releaseLabel,
                               state->releaseSlider,
                               state->releaseValueLabel,
                               kReleaseSliderMin,
                               kReleaseSliderMax,
                               250);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        compressorWindowApplyFont(*state, font);

        RECT client {0, 0, 0, 0};
        GetClientRect(hwnd, &client);
        compressorWindowLayout(hwnd, state, client.right - client.left, client.bottom - client.top);
        compressorWindowSyncControls(hwnd, state);
        return 0;
    }
    case WM_SIZE:
        if (state)
            compressorWindowLayout(hwnd, state, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (state && reinterpret_cast<HWND>(lParam) == state->enableCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            int trackId = state->trackId;
            if (trackId > 0)
            {
                bool enabled = SendMessageW(state->enableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                trackSetCompressorEnabled(trackId, enabled);
                compressorWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
            }
            return 0;
        }
        break;
    case WM_HSCROLL:
        if (state)
        {
            int trackId = state->trackId;
            if (trackId <= 0)
                return 0;

            HWND control = reinterpret_cast<HWND>(lParam);
            if (!control)
                control = GetFocus();

            if (control == state->thresholdSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float value = std::clamp(static_cast<float>(pos), static_cast<float>(kThresholdSliderMin),
                                         static_cast<float>(kThresholdSliderMax));
                trackSetCompressorThresholdDb(trackId, value);
                compressorWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                return 0;
            }

            if (control == state->ratioSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float ratio = std::clamp(static_cast<float>(pos) / 10.0f, 1.0f, 20.0f);
                trackSetCompressorRatio(trackId, ratio);
                compressorWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                return 0;
            }

            if (control == state->attackSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float attack = std::clamp(static_cast<float>(pos) / 1000.0f, 0.001f, 1.0f);
                trackSetCompressorAttack(trackId, attack);
                compressorWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                return 0;
            }

            if (control == state->releaseSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float release = std::clamp(static_cast<float>(pos) / 1000.0f, 0.01f, 4.0f);
                trackSetCompressorRelease(trackId, release);
                compressorWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                return 0;
            }
        }
        return 0;
    case WM_COMPRESSOR_SET_TRACK:
        if (state)
        {
            state->trackId = static_cast<int>(wParam);
            compressorWindowSyncControls(hwnd, state);
        }
        return 0;
    case WM_COMPRESSOR_REFRESH_VALUES:
        if (state)
            compressorWindowSyncControls(hwnd, state);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
    {
        if (state)
        {
            delete state;
            SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        }
        if (hwnd == gCompressorWindow)
        {
            gCompressorWindow = nullptr;
            requestMainMenuRefresh();
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void registerCompressorWindowClass()
{
    if (gCompressorWindowClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = CompressorWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kCompressorWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
        gCompressorWindowClassRegistered = true;
}

} // namespace

void openCompressorWindow(HWND parent, int trackId)
{
    registerCompressorWindowClass();
    if (!gCompressorWindowClassRegistered)
        return;

    if (gCompressorWindow && IsWindow(gCompressorWindow))
    {
        ShowWindow(gCompressorWindow, SW_SHOWNORMAL);
        SetForegroundWindow(gCompressorWindow);
        PostMessageW(gCompressorWindow, WM_COMPRESSOR_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        return;
    }

    RECT parentRect {0, 0, 0, 0};
    if (parent && IsWindow(parent))
        GetWindowRect(parent, &parentRect);

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parentRect.right > parentRect.left && parentRect.bottom > parentRect.top)
    {
        x = parentRect.left + 60;
        y = parentRect.top + 60;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kCompressorWindowClassName,
                                L"Compressor",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kDefaultWindowWidth,
                                kDefaultWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gCompressorWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_COMPRESSOR_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        requestMainMenuRefresh();
    }
}

void notifyCompressorWindowTrackChanged(int trackId)
{
    if (gCompressorWindow && IsWindow(gCompressorWindow))
        PostMessageW(gCompressorWindow, WM_COMPRESSOR_SET_TRACK, static_cast<WPARAM>(trackId), 0);
}

void notifyCompressorWindowValuesChanged(int trackId)
{
    if (gCompressorWindow && IsWindow(gCompressorWindow))
    {
        CompressorWindowState* state = getCompressorWindowState(gCompressorWindow);
        if (!state || trackId == 0 || state->trackId == trackId)
            PostMessageW(gCompressorWindow, WM_COMPRESSOR_REFRESH_VALUES, 0, 0);
    }
}

void closeCompressorWindow()
{
    if (gCompressorWindow && IsWindow(gCompressorWindow))
        DestroyWindow(gCompressorWindow);
}

