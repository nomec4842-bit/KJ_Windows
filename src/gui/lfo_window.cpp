#include "gui/lfo_window.h"

#include "core/track_type_synth.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <commctrl.h>
#include <cwchar>
#include <string>
#include <vector>

namespace
{

constexpr wchar_t kLfoWindowClassName[] = L"KJLfoWindow";
constexpr int kDefaultWindowWidth = 400;
constexpr int kDefaultWindowHeight = 520;

constexpr int kRateSliderMin = 5;    // 0.05 Hz
constexpr int kRateSliderMax = 2000; // 20 Hz
constexpr int kDeformSliderMin = 0;
constexpr int kDeformSliderMax = 100;
constexpr float kUiMinLfoRateHz = 0.05f;
constexpr float kUiMaxLfoRateHz = 20.0f;

constexpr std::array<const wchar_t*, 4> kShapeLabels = {
    L"Sine",
    L"Triangle",
    L"Saw",
    L"Square",
};

HWND gLfoWindow = nullptr;
bool gLfoWindowClassRegistered = false;

struct LfoControls
{
    HWND headerLabel = nullptr;
    HWND rateLabel = nullptr;
    HWND rateValueLabel = nullptr;
    HWND rateSlider = nullptr;
    HWND shapeLabel = nullptr;
    HWND shapeCombo = nullptr;
    HWND deformLabel = nullptr;
    HWND deformValueLabel = nullptr;
    HWND deformSlider = nullptr;
};

struct LfoWindowState
{
    int trackId = 0;
    HWND trackLabel = nullptr;
    std::array<LfoControls, 3> lfos{};
};

const Track* findTrack(const std::vector<Track>& tracks, int trackId)
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

void applyFont(LfoWindowState* state, HFONT font)
{
    if (!state)
        return;

    if (state->trackLabel)
        setControlFont(state->trackLabel, font);

    for (auto& lfo : state->lfos)
    {
        setControlFont(lfo.headerLabel, font);
        setControlFont(lfo.rateLabel, font);
        setControlFont(lfo.rateValueLabel, font);
        setControlFont(lfo.rateSlider, font);
        setControlFont(lfo.shapeLabel, font);
        setControlFont(lfo.shapeCombo, font);
        setControlFont(lfo.deformLabel, font);
        setControlFont(lfo.deformValueLabel, font);
        setControlFont(lfo.deformSlider, font);
    }
}

int rateToSlider(float rate)
{
    float clamped = std::clamp(rate, kUiMinLfoRateHz, kUiMaxLfoRateHz);
    return static_cast<int>(std::lround(clamped * 100.0f));
}

float sliderToRate(int slider)
{
    int clamped = std::clamp(slider, kRateSliderMin, kRateSliderMax);
    return static_cast<float>(clamped) / 100.0f;
}

int deformToSlider(float deform)
{
    float clamped = std::clamp(deform, 0.0f, 1.0f);
    return static_cast<int>(std::lround(clamped * 100.0f));
}

float sliderToDeform(int slider)
{
    int clamped = std::clamp(slider, kDeformSliderMin, kDeformSliderMax);
    return static_cast<float>(clamped) / 100.0f;
}

void layoutWindow(HWND hwnd, LfoWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int labelHeight = 20;
    const int valueWidth = 80;
    const int sliderHeight = 32;
    const int comboHeight = 28;
    const int sectionSpacing = 14;
    const int controlSpacing = 6;

    int contentWidth = std::max(width - padding * 2, 120);
    int y = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, padding, y, contentWidth, labelHeight, TRUE);
        y += labelHeight + sectionSpacing;
    }

    for (size_t i = 0; i < state->lfos.size(); ++i)
    {
        auto& lfo = state->lfos[i];
        if (lfo.headerLabel)
        {
            MoveWindow(lfo.headerLabel, padding, y, contentWidth, labelHeight, TRUE);
            y += labelHeight + controlSpacing;
        }

        if (lfo.rateLabel && lfo.rateValueLabel)
        {
            MoveWindow(lfo.rateLabel, padding, y, contentWidth - valueWidth, labelHeight, TRUE);
            MoveWindow(lfo.rateValueLabel, padding + contentWidth - valueWidth, y, valueWidth, labelHeight, TRUE);
            y += labelHeight + controlSpacing;
        }
        if (lfo.rateSlider)
        {
            MoveWindow(lfo.rateSlider, padding, y, contentWidth, sliderHeight, TRUE);
            y += sliderHeight + controlSpacing;
        }

        if (lfo.shapeLabel && lfo.shapeCombo)
        {
            MoveWindow(lfo.shapeLabel, padding, y, contentWidth, labelHeight, TRUE);
            y += labelHeight + controlSpacing;
            MoveWindow(lfo.shapeCombo, padding, y, contentWidth, comboHeight, TRUE);
            y += comboHeight + controlSpacing;
        }

        if (lfo.deformLabel && lfo.deformValueLabel)
        {
            MoveWindow(lfo.deformLabel, padding, y, contentWidth - valueWidth, labelHeight, TRUE);
            MoveWindow(lfo.deformValueLabel, padding + contentWidth - valueWidth, y, valueWidth, labelHeight, TRUE);
            y += labelHeight + controlSpacing;
        }
        if (lfo.deformSlider)
        {
            MoveWindow(lfo.deformSlider, padding, y, contentWidth, sliderHeight, TRUE);
            y += sliderHeight + sectionSpacing;
        }
    }
}

void populateShapeCombo(HWND combo)
{
    if (!combo)
        return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < kShapeLabels.size(); ++i)
    {
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(kShapeLabels[i]));
        if (index >= 0)
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
}

void setShapeSelection(HWND combo, LfoShape shape)
{
    if (!combo)
        return;

    int target = static_cast<int>(shape);
    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        int data = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if (data == target)
        {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, 0, 0);
}

void updateValueLabels(LfoWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
        return;

    for (size_t i = 0; i < state->lfos.size(); ++i)
    {
        auto& controls = state->lfos[i];
        float rate = trackGetLfoRate(trackId, static_cast<int>(i));
        float deform = trackGetLfoDeform(trackId, static_cast<int>(i));
        if (controls.rateValueLabel)
        {
            wchar_t buffer[64];
            swprintf(buffer, 64, L"%.2f Hz", rate);
            SetWindowTextW(controls.rateValueLabel, buffer);
        }
        if (controls.deformValueLabel)
        {
            wchar_t buffer[64];
            swprintf(buffer, 64, L"%.0f%%", deform * 100.0f);
            SetWindowTextW(controls.deformValueLabel, buffer);
        }
    }
}

void syncControls(LfoWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"Select a track in the Mod Matrix.");
        for (auto& lfo : state->lfos)
        {
            EnableWindow(lfo.rateSlider, FALSE);
            EnableWindow(lfo.shapeCombo, FALSE);
            EnableWindow(lfo.deformSlider, FALSE);
            if (lfo.rateValueLabel)
                SetWindowTextW(lfo.rateValueLabel, L"-");
            if (lfo.deformValueLabel)
                SetWindowTextW(lfo.deformValueLabel, L"-");
        }
        return;
    }

    auto tracks = getTracks();
    const Track* track = findTrack(tracks, trackId);
    if (!track)
    {
        state->trackId = 0;
        syncControls(state);
        return;
    }

    std::wstring trackName = toWide(track->name);
    if (trackName.empty())
        trackName = L"Unnamed Track";
    if (state->trackLabel)
        SetWindowTextW(state->trackLabel, trackName.c_str());

    for (size_t i = 0; i < state->lfos.size(); ++i)
    {
        auto& controls = state->lfos[i];
        float rate = trackGetLfoRate(trackId, static_cast<int>(i));
        float deform = trackGetLfoDeform(trackId, static_cast<int>(i));
        LfoShape shape = trackGetLfoShape(trackId, static_cast<int>(i));

        if (controls.rateSlider)
        {
            SendMessageW(controls.rateSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kRateSliderMin, kRateSliderMax));
            SendMessageW(controls.rateSlider, TBM_SETPOS, TRUE, rateToSlider(rate));
            EnableWindow(controls.rateSlider, TRUE);
        }
        if (controls.shapeCombo)
        {
            populateShapeCombo(controls.shapeCombo);
            setShapeSelection(controls.shapeCombo, shape);
            EnableWindow(controls.shapeCombo, TRUE);
        }
        if (controls.deformSlider)
        {
            SendMessageW(controls.deformSlider, TBM_SETRANGE, TRUE, MAKELPARAM(kDeformSliderMin, kDeformSliderMax));
            SendMessageW(controls.deformSlider, TBM_SETPOS, TRUE, deformToSlider(deform));
            EnableWindow(controls.deformSlider, TRUE);
        }
    }

    updateValueLabels(state);
}

void ensureWindowClass()
{
    if (gLfoWindowClassRegistered)
        return;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        auto* state = reinterpret_cast<LfoWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

        switch (msg)
        {
        case WM_CREATE:
        {
            auto* createStruct = reinterpret_cast<LPCREATESTRUCTW>(lParam);
            auto* newState = new LfoWindowState();
            if (createStruct)
                newState->trackId = static_cast<int>(reinterpret_cast<intptr_t>(createStruct->lpCreateParams));
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

            newState->trackLabel = CreateWindowExW(0,
                                                   L"STATIC",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   hwnd,
                                                   nullptr,
                                                   createStruct->hInstance,
                                                   nullptr);

            for (size_t i = 0; i < newState->lfos.size(); ++i)
            {
                auto& lfo = newState->lfos[i];
                wchar_t header[32];
                swprintf(header, 32, L"LFO %zu", i + 1);
                lfo.headerLabel = CreateWindowExW(0,
                                                  L"STATIC",
                                                  header,
                                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  hwnd,
                                                  nullptr,
                                                  createStruct->hInstance,
                                                  nullptr);
                lfo.rateLabel = CreateWindowExW(0,
                                               L"STATIC",
                                               L"Rate",
                                               WS_CHILD | WS_VISIBLE | SS_LEFT,
                                               0,
                                               0,
                                               0,
                                               0,
                                               hwnd,
                                               nullptr,
                                               createStruct->hInstance,
                                               nullptr);
                lfo.rateValueLabel = CreateWindowExW(0,
                                                     L"STATIC",
                                                     L"",
                                                     WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     hwnd,
                                                     nullptr,
                                                     createStruct->hInstance,
                                                     nullptr);
                lfo.rateSlider = CreateWindowExW(0,
                                                 TRACKBAR_CLASSW,
                                                 L"",
                                                 WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 hwnd,
                                                 nullptr,
                                                 createStruct->hInstance,
                                                 nullptr);
                lfo.shapeLabel = CreateWindowExW(0,
                                                 L"STATIC",
                                                 L"Shape",
                                                 WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                 0,
                                                 0,
                                                 0,
                                                 0,
                                                 hwnd,
                                                 nullptr,
                                                 createStruct->hInstance,
                                                 nullptr);
                lfo.shapeCombo = CreateWindowExW(0,
                                                 WC_COMBOBOXW,
                                                 L"",
                                                 WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                 0,
                                                 0,
                                                 0,
                                                 120,
                                                 hwnd,
                                                 nullptr,
                                                 createStruct->hInstance,
                                                 nullptr);
                lfo.deformLabel = CreateWindowExW(0,
                                                  L"STATIC",
                                                  L"Deform",
                                                  WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  hwnd,
                                                  nullptr,
                                                  createStruct->hInstance,
                                                  nullptr);
                lfo.deformValueLabel = CreateWindowExW(0,
                                                        L"STATIC",
                                                        L"",
                                                        WS_CHILD | WS_VISIBLE | SS_RIGHT,
                                                        0,
                                                        0,
                                                        0,
                                                        0,
                                                        hwnd,
                                                        nullptr,
                                                        createStruct->hInstance,
                                                        nullptr);
                lfo.deformSlider = CreateWindowExW(0,
                                                   TRACKBAR_CLASSW,
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   hwnd,
                                                   nullptr,
                                                   createStruct->hInstance,
                                                   nullptr);
            }

            HFONT font = reinterpret_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
            applyFont(newState, font);
            syncControls(newState);

            RECT rect{};
            GetClientRect(hwnd, &rect);
            layoutWindow(hwnd, newState, rect.right - rect.left, rect.bottom - rect.top);
            return 0;
        }
        case WM_DESTROY:
        {
            if (state)
            {
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            if (hwnd == gLfoWindow)
            {
                gLfoWindow = nullptr;
                requestMainMenuRefresh();
            }
            return 0;
        }
        case WM_SIZE:
        {
            if (!state)
                return 0;
            int width = LOWORD(lParam);
            int height = HIWORD(lParam);
            layoutWindow(hwnd, state, width, height);
            return 0;
        }
        case WM_LFO_SET_TRACK:
        {
            if (!state)
                return 0;
            state->trackId = static_cast<int>(wParam);
            syncControls(state);
            return 0;
        }
        case WM_LFO_REFRESH_VALUES:
        {
            if (!state)
                return 0;
            int trackId = static_cast<int>(wParam);
            if (trackId == 0 || state->trackId == trackId)
            {
                syncControls(state);
            }
            return 0;
        }
        case WM_COMMAND:
        {
            if (!state)
                return 0;
            HWND control = reinterpret_cast<HWND>(lParam);
            if (!control)
                return 0;
            for (size_t i = 0; i < state->lfos.size(); ++i)
            {
                auto& lfo = state->lfos[i];
                if (control == lfo.shapeCombo && HIWORD(wParam) == CBN_SELCHANGE)
                {
                    int sel = static_cast<int>(SendMessageW(lfo.shapeCombo, CB_GETCURSEL, 0, 0));
                    if (sel >= 0)
                    {
                        int data = static_cast<int>(SendMessageW(lfo.shapeCombo, CB_GETITEMDATA, static_cast<WPARAM>(sel), 0));
                        trackSetLfoShape(state->trackId, static_cast<int>(i), static_cast<LfoShape>(data));
                    }
                    return 0;
                }
            }
            return 0;
        }
        case WM_HSCROLL:
        {
            if (!state)
                return 0;
            HWND slider = reinterpret_cast<HWND>(lParam);
            if (!slider)
                return 0;

            for (size_t i = 0; i < state->lfos.size(); ++i)
            {
                auto& lfo = state->lfos[i];
                if (slider == lfo.rateSlider)
                {
                    int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
                    float rate = sliderToRate(pos);
                    trackSetLfoRate(state->trackId, static_cast<int>(i), rate);
                    updateValueLabels(state);
                    return 0;
                }
                if (slider == lfo.deformSlider)
                {
                    int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
                    float deform = sliderToDeform(pos);
                    trackSetLfoDeform(state->trackId, static_cast<int>(i), deform);
                    updateValueLabels(state);
                    return 0;
                }
            }
            return 0;
        }
        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };

    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kLfoWindowClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);

    if (RegisterClassW(&wc))
        gLfoWindowClassRegistered = true;
}

} // namespace

void openLfoWindow(HWND parent, int trackId)
{
    if (gLfoWindow && IsWindow(gLfoWindow))
    {
        PostMessageW(gLfoWindow, WM_LFO_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        SetForegroundWindow(gLfoWindow);
        return;
    }

    ensureWindowClass();
    if (!gLfoWindowClassRegistered)
        return;

    RECT parentRect{0, 0, 0, 0};
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
                                kLfoWindowClassName,
                                L"LFO Parameters",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kDefaultWindowWidth,
                                kDefaultWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                reinterpret_cast<LPVOID>(static_cast<intptr_t>(trackId)));
    if (hwnd)
    {
        gLfoWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        requestMainMenuRefresh();
    }
}

void notifyLfoWindowTrackChanged(int trackId)
{
    if (gLfoWindow && IsWindow(gLfoWindow))
    {
        PostMessageW(gLfoWindow, WM_LFO_SET_TRACK, static_cast<WPARAM>(trackId), 0);
    }
}

void notifyLfoWindowValuesChanged(int trackId)
{
    if (gLfoWindow && IsWindow(gLfoWindow))
    {
        PostMessageW(gLfoWindow, WM_LFO_REFRESH_VALUES, static_cast<WPARAM>(trackId), 0);
    }
}

void closeLfoWindow()
{
    if (gLfoWindow && IsWindow(gLfoWindow))
    {
        DestroyWindow(gLfoWindow);
        gLfoWindow = nullptr;
    }
}
