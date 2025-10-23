#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "wdl/lice/lice.h"

#include <windows.h>
#include <commdlg.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <chrono>

namespace {

std::wstring ToWideString(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

std::string FromWideString(const std::wstring& value)
{
    if (value.empty())
        return {};

    int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (sizeNeeded <= 0)
        return std::string(value.begin(), value.end());

    std::string result(static_cast<size_t>(sizeNeeded - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), sizeNeeded, nullptr, nullptr);
    return result;
}

constexpr int kWindowWidth = 800;
constexpr int kWindowHeight = 600;

constexpr int kTrackTabWidth = 140;
constexpr int kTrackTabHeight = 60;
constexpr int kTrackTabsTop = 180;
constexpr int kTrackTabsStartX = 40;
constexpr int kTrackTabsSpacing = 10;
constexpr int kTrackTabsToGridMargin = 20;
constexpr int kTrackTypeButtonHeight = 22;
constexpr int kTrackTypeButtonPadding = 6;
constexpr int kTrackTypeDropdownSpacing = 4;
constexpr int kTrackTypeDropdownOptionHeight = 24;

constexpr int kAudioDeviceDropdownSpacing = 4;
constexpr int kAudioDeviceDropdownOptionHeight = 24;

const std::array<TrackType, 2> kTrackTypeOptions = {TrackType::Synth, TrackType::Sample};

RECT playButton = {40, 40, 180, 110};
RECT loadSampleButton = {200, 40, 340, 110};
RECT bpmDownButton = {360, 55, 400, 95};
RECT bpmUpButton = {410, 55, 450, 95};
RECT stepCountDownButton = {470, 55, 510, 95};
RECT stepCountUpButton = {520, 55, 560, 95};
RECT pageDownButton = {580, 55, 620, 95};
RECT pageUpButton = {630, 55, 670, 95};
RECT addTrackButton = {690, 40, 780, 110};
RECT audioDeviceButton = {40, 115, 340, 145};
std::array<RECT, kSequencerStepsPerPage> stepRects;
int currentStepPage = 0;
int selectedTrackId = 0;
std::vector<int> trackTabIds;
std::vector<RECT> trackTabRects;
int openTrackTypeTrackId = 0;
bool audioDeviceDropdownOpen = false;
HWND gMainWindow = nullptr;

std::unique_ptr<LICE_SysBitmap> gSurface;

struct AudioDeviceDropdownOption
{
    RECT rect;
    std::wstring id;
    std::wstring label;
    bool isSelected = false;
};

std::vector<AudioDeviceDropdownOption> gAudioDeviceOptions;
std::vector<AudioOutputDevice> gCachedAudioDevices;
std::chrono::steady_clock::time_point gLastAudioDeviceRefresh = std::chrono::steady_clock::time_point::min();

inline LICE_pixel LICE_ColorFromCOLORREF(COLORREF color, int alpha = 255)
{
    return LICE_RGBA(GetRValue(color), GetGValue(color), GetBValue(color), alpha);
}

void drawText(LICE_SysBitmap& surface, const RECT& rect, const char* text, COLORREF color,
              UINT format = DT_CENTER | DT_VCENTER | DT_SINGLELINE)
{
    if (!text)
        return;

    int textWidth = 0;
    int textHeight = 0;
    LICE_MeasureText(text, &textWidth, &textHeight);

    const int rectWidth = rect.right - rect.left;
    const int rectHeight = rect.bottom - rect.top;

    int x = rect.left;
    if (format & DT_CENTER)
    {
        x = rect.left + (rectWidth - textWidth) / 2;
    }
    else if (format & DT_RIGHT)
    {
        x = rect.right - textWidth;
    }

    int maxX = rect.right - textWidth;
    if (maxX < rect.left)
        maxX = rect.left;
    x = std::clamp(x, static_cast<int>(rect.left), maxX); // Ensure clamp arguments use a consistent int type.

    int y = rect.top;
    if (format & DT_VCENTER)
    {
        y = rect.top + (rectHeight - textHeight) / 2;
    }
    else if (format & DT_BOTTOM)
    {
        y = rect.bottom - textHeight;
    }

    int maxY = rect.bottom - textHeight;
    if (maxY < rect.top)
        maxY = rect.top;
    y = std::clamp(y, static_cast<int>(rect.top), maxY); // Ensure clamp arguments use a consistent int type.

    LICE_DrawText(&surface, x, y, text, LICE_ColorFromCOLORREF(color), 1.0f, LICE_BLIT_MODE_COPY);
}

void refreshAudioDeviceList(bool forceRefresh)
{
    auto now = std::chrono::steady_clock::now();
    if (!forceRefresh && gLastAudioDeviceRefresh != std::chrono::steady_clock::time_point::min())
    {
        if (now - gLastAudioDeviceRefresh < std::chrono::seconds(2))
            return;
    }

    gCachedAudioDevices = getAvailableAudioOutputDevices();
    gLastAudioDeviceRefresh = now;
}

void buildStepRects()
{
    const int startX = 40;
    const int startY = kTrackTabsTop + kTrackTabHeight + kTrackTabsToGridMargin;
    const int stepWidth = 35;
    const int stepHeight = stepWidth;
    const int spacing = 10;

    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        RECT rect {};
        rect.left = startX + i * (stepWidth + spacing);
        rect.top = startY;
        rect.right = rect.left + stepWidth;
        rect.bottom = rect.top + stepHeight;
        stepRects[i] = rect;
    }
}

void clampCurrentPageForTrack(int activeTrackId)
{
    if (activeTrackId <= 0)
    {
        auto tracks = getTracks();
        if (!tracks.empty())
        {
            activeTrackId = tracks.front().id;
        }
    }

    int totalSteps = getSequencerStepCount(activeTrackId);
    if (totalSteps < 1)
        totalSteps = kSequencerStepsPerPage;
    int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
    if (totalPages < 1)
        totalPages = 1;
    if (currentStepPage >= totalPages)
        currentStepPage = totalPages - 1;
    if (currentStepPage < 0)
        currentStepPage = 0;
}

int ensureSelectedTrack(const std::vector<Track>& tracks)
{
    int previousSelected = selectedTrackId;

    if (tracks.empty())
    {
        selectedTrackId = 0;
    }
    else
    {
        auto selectedIt = std::find_if(tracks.begin(), tracks.end(), [](const Track& track) {
            return track.id == selectedTrackId;
        });

        if (selectedIt == tracks.end())
        {
            int activeTrackId = getActiveSequencerTrackId();
            auto activeIt = std::find_if(tracks.begin(), tracks.end(), [activeTrackId](const Track& track) {
                return track.id == activeTrackId;
            });

            if (activeIt != tracks.end())
            {
                selectedTrackId = activeTrackId;
            }
            else
            {
                selectedTrackId = tracks.front().id;
            }
        }
    }

    if (selectedTrackId != previousSelected)
    {
        setActiveSequencerTrackId(selectedTrackId);
        currentStepPage = 0;
    }

    return selectedTrackId;
}

void ensureTrackTabState(const std::vector<Track>& tracks)
{
    bool changed = tracks.size() != trackTabIds.size();

    if (!changed)
    {
        for (size_t i = 0; i < tracks.size(); ++i)
        {
            if (trackTabIds[i] != tracks[i].id)
            {
                changed = true;
                break;
            }
        }
    }

    if (changed)
    {
        trackTabIds.clear();
        trackTabRects.clear();

        const int startX = kTrackTabsStartX;
        const int startY = kTrackTabsTop;
        const int tabHeight = kTrackTabHeight;
        const int tabWidth = kTrackTabWidth;
        const int spacing = kTrackTabsSpacing;

        int currentX = startX;
        for (const auto& track : tracks)
        {
            trackTabIds.push_back(track.id);
            RECT rect {};
            rect.left = currentX;
            rect.top = startY;
            rect.right = rect.left + tabWidth;
            rect.bottom = rect.top + tabHeight;
            trackTabRects.push_back(rect);
            currentX = rect.right + spacing;
        }

        openTrackTypeTrackId = 0;
    }

    int previousSelected = selectedTrackId;
    int ensuredTrackId = ensureSelectedTrack(tracks);
    bool selectionChanged = ensuredTrackId != previousSelected;

    if ((changed || selectionChanged) && gMainWindow)
    {
        InvalidateRect(gMainWindow, nullptr, FALSE);
    }
}

bool pointInRect(const RECT& rect, int x, int y)
{
    return x >= rect.left && x <= rect.right && y >= rect.top && y <= rect.bottom;
}

const Track* findTrackById(const std::vector<Track>& tracks, int trackId)
{
    auto it = std::find_if(tracks.begin(), tracks.end(), [trackId](const Track& track) {
        return track.id == trackId;
    });
    if (it != tracks.end())
    {
        return &(*it);
    }
    return nullptr;
}

std::string trackTypeToString(TrackType type)
{
    switch (type)
    {
    case TrackType::Synth:
        return "Synth";
    case TrackType::Sample:
        return "Sample";
    }
    return "Unknown";
}

RECT getTrackTypeButtonRect(const RECT& tabRect)
{
    RECT rect = tabRect;
    rect.left += kTrackTypeButtonPadding;
    rect.right -= kTrackTypeButtonPadding;
    rect.bottom -= kTrackTypeButtonPadding;
    rect.top = rect.bottom - kTrackTypeButtonHeight;
    if (rect.top < tabRect.top + kTrackTypeButtonPadding)
    {
        rect.top = tabRect.top + kTrackTypeButtonPadding;
    }
    return rect;
}

void ensureSurfaceSize(int width, int height)
{
    if (width <= 0 || height <= 0)
        return;

    if (!gSurface)
    {
        gSurface = std::make_unique<LICE_SysBitmap>(width, height);
        return;
    }

    gSurface->resize(width, height);
}

void drawButton(LICE_SysBitmap& surface, const RECT& rect, COLORREF fill, COLORREF outline, const char* text)
{
    const int width = rect.right - rect.left;
    const int height = rect.bottom - rect.top;

    LICE_FillRect(&surface, rect.left, rect.top, width, height,
                  LICE_ColorFromCOLORREF(fill));
    LICE_DrawRect(&surface, rect.left, rect.top, width, height,
                  LICE_ColorFromCOLORREF(outline));

    RECT textRect = rect;
    drawText(surface, textRect, text, RGB(230, 230, 230));
}

void drawSequencer(LICE_SysBitmap& surface, int activeTrackId)
{
    bool playing = isPlaying.load(std::memory_order_relaxed);
    auto tracks = getTracks();
    if (activeTrackId <= 0 && !tracks.empty())
    {
        activeTrackId = tracks.front().id;
    }

    clampCurrentPageForTrack(activeTrackId);

    int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);

    int totalSteps = getSequencerStepCount(activeTrackId);
    if (totalSteps < 1)
        totalSteps = kSequencerStepsPerPage;

    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        const RECT& rect = stepRects[i];
        const int width = rect.right - rect.left;
        const int height = rect.bottom - rect.top;
        int stepIndex = currentStepPage * kSequencerStepsPerPage + i;
        bool inRange = stepIndex < totalSteps;
        bool active = inRange && getTrackStepState(activeTrackId, stepIndex);

        COLORREF fill = active ? RGB(0, 120, 200) : RGB(45, 45, 45);
        if (!inRange)
        {
            fill = RGB(30, 30, 30);
        }
        LICE_FillRect(&surface, rect.left, rect.top, width, height,
                      LICE_ColorFromCOLORREF(fill));

        COLORREF borderColor = RGB(70, 70, 70);
        int penWidth = 2;
        if (playing && inRange && stepIndex == currentStep)
        {
            borderColor = RGB(255, 215, 0);
            penWidth = 3;
        }

        if (!inRange)
        {
            borderColor = RGB(50, 50, 50);
        }

        for (int p = 0; p < penWidth; ++p)
        {
            LICE_DrawRect(&surface, rect.left - p, rect.top - p,
                          width + p * 2, height + p * 2,
                          LICE_ColorFromCOLORREF(borderColor));
        }

        RECT labelRect = rect;
        labelRect.top = rect.bottom - 22;
        labelRect.left += 4;
        std::string label = inRange ? std::to_string(stepIndex + 1) : "-";
        drawText(surface, labelRect, label.c_str(), RGB(220, 220, 220),
                 DT_LEFT | DT_BOTTOM | DT_SINGLELINE);
    }
}

void renderUI(LICE_SysBitmap& surface, const RECT& client)
{
    LICE_Clear(&surface, LICE_ColorFromCOLORREF(RGB(20, 20, 20)));
    gAudioDeviceOptions.clear();

    drawButton(surface, playButton,
               isPlaying.load(std::memory_order_relaxed) ? RGB(0, 150, 0) : RGB(120, 0, 0),
               RGB(30, 30, 30),
               isPlaying.load(std::memory_order_relaxed) ? "Stop" : "Play");

    auto tracks = getTracks();
    ensureTrackTabState(tracks);

    int activeTrackId = selectedTrackId;
    if (activeTrackId <= 0 && !tracks.empty())
    {
        activeTrackId = tracks.front().id;
        setActiveSequencerTrackId(activeTrackId);
    }

    bool showSampleLoader = false;
    if (const Track* activeTrack = findTrackById(tracks, activeTrackId))
    {
        showSampleLoader = activeTrack->type == TrackType::Sample;
    }
    else if (activeTrackId > 0)
    {
        showSampleLoader = trackGetType(activeTrackId) == TrackType::Sample;
    }

    if (showSampleLoader)
    {
        drawButton(surface, loadSampleButton,
                   RGB(50, 50, 50), RGB(120, 120, 120),
                   "Load Sample");
    }

    drawButton(surface, bpmDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, bpmUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, stepCountDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, stepCountUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, pageDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "<");
    drawButton(surface, pageUpButton, RGB(50, 50, 50), RGB(120, 120, 120), ">");
    drawButton(surface, addTrackButton, RGB(50, 50, 50), RGB(120, 120, 120), "+Track");

    auto activeOutputDevice = getActiveAudioOutputDevice();
    std::wstring audioLabel = L"Audio Output: ";
    if (!activeOutputDevice.name.empty())
    {
        audioLabel += activeOutputDevice.name;
    }
    else
    {
        audioLabel += L"System Default";
    }
    std::string audioLabelUtf8 = FromWideString(audioLabel);
    if (audioLabelUtf8.empty())
    {
        audioLabelUtf8 = "Audio Output";
    }
    drawButton(surface, audioDeviceButton, RGB(50, 50, 50), RGB(120, 120, 120), audioLabelUtf8.c_str());

    if (audioDeviceDropdownOpen)
    {
        refreshAudioDeviceList(false);
        RECT optionRect = audioDeviceButton;
        optionRect.top = audioDeviceButton.bottom + kAudioDeviceDropdownSpacing;
        optionRect.bottom = optionRect.top + kAudioDeviceDropdownOptionHeight;

        std::wstring requestedDeviceId = getRequestedAudioOutputDeviceId();
        std::wstring defaultLabel = L"System Default";
        if (requestedDeviceId.empty() && !activeOutputDevice.name.empty())
        {
            defaultLabel += L" (" + activeOutputDevice.name + L")";
        }

        AudioDeviceDropdownOption defaultOption{};
        defaultOption.rect = optionRect;
        defaultOption.id.clear();
        defaultOption.label = std::move(defaultLabel);
        defaultOption.isSelected = requestedDeviceId.empty();
        gAudioDeviceOptions.push_back(defaultOption);

        COLORREF defaultFill = defaultOption.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
        COLORREF defaultOutline = defaultOption.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
        std::string defaultLabelUtf8 = FromWideString(gAudioDeviceOptions.back().label);
        if (defaultLabelUtf8.empty())
            defaultLabelUtf8 = "System Default";
        drawButton(surface, optionRect, defaultFill, defaultOutline, defaultLabelUtf8.c_str());

        optionRect.top = optionRect.bottom + kAudioDeviceDropdownSpacing;
        optionRect.bottom = optionRect.top + kAudioDeviceDropdownOptionHeight;

        for (const auto& device : gCachedAudioDevices)
        {
            AudioDeviceDropdownOption option{};
            option.rect = optionRect;
            option.id = device.id;
            option.label = device.name.empty() ? L"Audio Device" : device.name;
            option.isSelected = !requestedDeviceId.empty() && device.id == requestedDeviceId;
            gAudioDeviceOptions.push_back(option);

            COLORREF optionFill = option.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
            COLORREF optionOutline = option.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
            std::string optionLabelUtf8 = FromWideString(option.label);
            if (optionLabelUtf8.empty())
                optionLabelUtf8 = "Audio Device";
            drawButton(surface, optionRect, optionFill, optionOutline, optionLabelUtf8.c_str());

            optionRect.top = optionRect.bottom + kAudioDeviceDropdownSpacing;
            optionRect.bottom = optionRect.top + kAudioDeviceDropdownOptionHeight;
        }
    }

    int bpm = sequencerBPM.load(std::memory_order_relaxed);
    std::string bpmText = "Tempo: " + std::to_string(bpm) + " BPM";
    RECT bpmRect {470, 20, client.right - 40, 50};
    drawText(surface, bpmRect, bpmText.c_str(), RGB(220, 220, 220),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    int totalSteps = getSequencerStepCount(activeTrackId);
    if (totalSteps < 1)
        totalSteps = kSequencerStepsPerPage;
    std::string stepText = "Steps: " + std::to_string(totalSteps);
    RECT stepRect {470, 95, client.right - 40, 125};
    drawText(surface, stepRect, stepText.c_str(), RGB(220, 220, 220),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    clampCurrentPageForTrack(activeTrackId);
    int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
    if (totalPages < 1)
        totalPages = 1;
    std::string pageText = "Page: " + std::to_string(currentStepPage + 1) + "/" + std::to_string(totalPages);
    RECT pageRect {470, 130, client.right - 40, 160};
    drawText(surface, pageRect, pageText.c_str(), RGB(220, 220, 220),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    size_t trackCount = tracks.size();
    std::string trackText = "Tracks: " + std::to_string(trackCount);
    RECT trackRect {40, 155, client.right - 40, 185};
    drawText(surface, trackRect, trackText.c_str(), RGB(220, 220, 220),
             DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    const size_t rectCount = trackTabRects.size();
    const size_t tabCount = std::min(rectCount, tracks.size());
    struct PendingDropdownOption
    {
        int trackId;
        TrackType type;
        RECT rect;
        bool isSelected;
    };
    std::vector<PendingDropdownOption> dropdownOptions;
    for (size_t i = 0; i < tabCount; ++i)
    {
        const auto& track = tracks[i];
        const RECT& tabRect = trackTabRects[i];
        bool isActive = track.id == activeTrackId;
        COLORREF fill = isActive ? RGB(0, 120, 200) : RGB(60, 60, 60);
        COLORREF outline = isActive ? RGB(20, 20, 20) : RGB(120, 120, 120);
        drawButton(surface, tabRect, fill, outline, "");

        RECT typeRect = getTrackTypeButtonRect(tabRect);
        RECT nameRect = tabRect;
        nameRect.bottom = typeRect.top - 4;
        if (nameRect.bottom <= nameRect.top)
        {
            nameRect.bottom = typeRect.top;
        }
        drawText(surface, nameRect, track.name.c_str(), RGB(230, 230, 230),
                 DT_CENTER | DT_VCENTER | DT_SINGLELINE);

        std::string typeLabel = "Type: " + trackTypeToString(track.type);
        COLORREF typeFill = isActive ? RGB(0, 90, 160) : RGB(45, 45, 45);
        COLORREF typeOutline = isActive ? RGB(20, 20, 20) : RGB(120, 120, 120);
        drawButton(surface, typeRect, typeFill, typeOutline, typeLabel.c_str());

        if (track.id == openTrackTypeTrackId)
        {
            RECT optionRect = typeRect;
            optionRect.top = typeRect.bottom + kTrackTypeDropdownSpacing;
            optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
            for (TrackType option : kTrackTypeOptions)
            {
                PendingDropdownOption pending {};
                pending.trackId = track.id;
                pending.type = option;
                pending.rect = optionRect;
                pending.isSelected = (option == track.type);
                dropdownOptions.push_back(pending);

                optionRect.top = optionRect.bottom + kTrackTypeDropdownSpacing;
                optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
            }
        }
    }

    drawSequencer(surface, activeTrackId);

    for (const auto& option : dropdownOptions)
    {
        COLORREF optionFill = option.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
        COLORREF optionOutline = option.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
        std::string optionLabel = trackTypeToString(option.type);
        drawButton(surface, option.rect, optionFill, optionOutline, optionLabel.c_str());
    }
}

} // namespace

LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        gMainWindow = hwnd;
        buildStepRects();
        SetTimer(hwnd, 1, 60, nullptr);
        return 0;
    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        if (audioDeviceDropdownOpen)
        {
            for (const auto& option : gAudioDeviceOptions)
            {
                if (pointInRect(option.rect, x, y))
                {
                    setActiveAudioOutputDevice(option.id);
                    audioDeviceDropdownOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            if (!pointInRect(audioDeviceButton, x, y))
            {
                audioDeviceDropdownOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
        }

        if (pointInRect(audioDeviceButton, x, y))
        {
            audioDeviceDropdownOpen = !audioDeviceDropdownOpen;
            if (audioDeviceDropdownOpen)
            {
                refreshAudioDeviceList(true);
            }
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        auto tracks = getTracks();
        ensureTrackTabState(tracks);
        int activeTrackId = selectedTrackId;
        if (activeTrackId <= 0 && !tracks.empty())
        {
            activeTrackId = tracks.front().id;
            setActiveSequencerTrackId(activeTrackId);
        }

        int previousDropdownTrack = openTrackTypeTrackId;
        for (size_t i = 0; i < trackTabRects.size() && i < tracks.size(); ++i)
        {
            const auto& track = tracks[i];
            const RECT& tabRect = trackTabRects[i];
            RECT typeRect = getTrackTypeButtonRect(tabRect);

            if (track.id == openTrackTypeTrackId)
            {
                RECT optionRect = typeRect;
                optionRect.top = typeRect.bottom + kTrackTypeDropdownSpacing;
                optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
                for (TrackType option : kTrackTypeOptions)
                {
                    if (pointInRect(optionRect, x, y))
                    {
                        trackSetType(track.id, option);
                        openTrackTypeTrackId = 0;
                        InvalidateRect(hwnd, nullptr, FALSE);
                        return 0;
                    }
                    optionRect.top = optionRect.bottom + kTrackTypeDropdownSpacing;
                    optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
                }
            }

            if (pointInRect(typeRect, x, y))
            {
                if (openTrackTypeTrackId == track.id)
                {
                    openTrackTypeTrackId = 0;
                }
                else
                {
                    openTrackTypeTrackId = track.id;
                }
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (pointInRect(tabRect, x, y))
            {
                bool stateChanged = false;
                if (openTrackTypeTrackId != 0)
                {
                    openTrackTypeTrackId = 0;
                    stateChanged = true;
                }

                int newTrackId = track.id;
                if (newTrackId != selectedTrackId)
                {
                    selectedTrackId = newTrackId;
                    setActiveSequencerTrackId(selectedTrackId);
                    currentStepPage = 0;
                    stateChanged = true;
                }

                if (stateChanged)
                {
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        if (previousDropdownTrack != 0 && openTrackTypeTrackId == previousDropdownTrack)
        {
            openTrackTypeTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (pointInRect(playButton, x, y))
        {
            bool playing = isPlaying.load(std::memory_order_relaxed);
            isPlaying.store(!playing, std::memory_order_relaxed);
            requestSequencerReset();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        bool showSampleLoader = false;
        if (const Track* activeTrack = findTrackById(tracks, activeTrackId))
        {
            showSampleLoader = activeTrack->type == TrackType::Sample;
        }
        else if (activeTrackId > 0)
        {
            showSampleLoader = trackGetType(activeTrackId) == TrackType::Sample;
        }

        if (showSampleLoader && pointInRect(loadSampleButton, x, y))
        {
            wchar_t fileBuffer[MAX_PATH] = {0};
            OPENFILENAMEW ofn = {0};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = hwnd;
            ofn.lpstrFilter = L"WAV Files\0*.wav\0All Files\0*.*\0";
            ofn.lpstrFile = fileBuffer;
            ofn.nMaxFile = MAX_PATH;
            ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
            ofn.lpstrDefExt = L"wav";

            if (GetOpenFileNameW(&ofn))
            {
                std::filesystem::path selectedPath(fileBuffer);
                if (activeTrackId <= 0 || !loadSampleFile(activeTrackId, selectedPath))
                {
                    MessageBoxW(hwnd,
                                L"Failed to load selected sample.",
                                L"Load Sample",
                                MB_OK | MB_ICONERROR);
                }
                InvalidateRect(hwnd, nullptr, FALSE);
            }

            return 0;
        }

        if (pointInRect(bpmDownButton, x, y))
        {
            int bpm = sequencerBPM.load(std::memory_order_relaxed);
            bpm = std::clamp(bpm - 5, 40, 240);
            sequencerBPM.store(bpm, std::memory_order_relaxed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(bpmUpButton, x, y))
        {
            int bpm = sequencerBPM.load(std::memory_order_relaxed);
            bpm = std::clamp(bpm + 5, 40, 240);
            sequencerBPM.store(bpm, std::memory_order_relaxed);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(stepCountDownButton, x, y))
        {
            int steps = getSequencerStepCount(activeTrackId);
            if (steps < 1)
                steps = kSequencerStepsPerPage;
            if (activeTrackId > 0)
            {
                setSequencerStepCount(activeTrackId, steps - 1);
            }
            clampCurrentPageForTrack(activeTrackId);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(stepCountUpButton, x, y))
        {
            int steps = getSequencerStepCount(activeTrackId);
            if (steps < 1)
                steps = kSequencerStepsPerPage;
            if (activeTrackId > 0)
            {
                setSequencerStepCount(activeTrackId, steps + 1);
            }
            clampCurrentPageForTrack(activeTrackId);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(pageDownButton, x, y))
        {
            if (currentStepPage > 0)
            {
                --currentStepPage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (pointInRect(pageUpButton, x, y))
        {
            int totalSteps = getSequencerStepCount(activeTrackId);
            if (totalSteps < 1)
                totalSteps = kSequencerStepsPerPage;
            int totalPages = (totalSteps + kSequencerStepsPerPage - 1) / kSequencerStepsPerPage;
            if (totalPages < 1)
                totalPages = 1;
            if (currentStepPage < totalPages - 1)
            {
                ++currentStepPage;
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        }

        if (pointInRect(addTrackButton, x, y))
        {
            Track newTrack = addTrack();
            if (activeTrackId <= 0 && newTrack.id > 0)
            {
                setActiveSequencerTrackId(newTrack.id);
            }
            auto updatedTracks = getTracks();
            ensureTrackTabState(updatedTracks);
            std::string message = newTrack.name + " created.";
            std::wstring wideMessage = ToWideString(message);
            MessageBoxW(hwnd,
                        wideMessage.c_str(),
                        L"Add Track",
                        MB_OK | MB_ICONINFORMATION);
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        for (int i = 0; i < kSequencerStepsPerPage; ++i)
        {
            if (pointInRect(stepRects[i], x, y))
            {
                int stepIndex = currentStepPage * kSequencerStepsPerPage + i;
                int totalSteps = getSequencerStepCount(activeTrackId);
                if (totalSteps < 1)
                    totalSteps = kSequencerStepsPerPage;
                if (stepIndex < totalSteps && activeTrackId > 0)
                {
                    toggleSequencerStep(activeTrackId, stepIndex);
                    InvalidateRect(hwnd, nullptr, FALSE);
                }
                return 0;
            }
        }

        return 0;
    }
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_SIZE:
    {
        int width = LOWORD(lParam);
        int height = HIWORD(lParam);
        ensureSurfaceSize(width, height);
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);
        ensureSurfaceSize(client.right, client.bottom);
        if (gSurface)
        {
            renderUI(*gSurface, client);
            LICE_Scale_BitBlt(hdc, 0, 0, gSurface->getWidth(), gSurface->getHeight(), gSurface.get(), 0, 0, SRCCOPY); // Use LICE helper to blit the surface to the window HDC.
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        gSurface.reset();
        gMainWindow = nullptr;
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void initGUI()
{
    WNDCLASSW wc = {0};
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"KJWDLWindow";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"KJWDLWindow", L"KJ",
                                WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, kWindowWidth, kWindowHeight,
                                nullptr, nullptr, wc.hInstance, nullptr);
    if (!hwnd)
    {
        MessageBoxW(nullptr, L"Window creation failed!", L"Error", MB_OK);
        return;
    }

    gMainWindow = hwnd;

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    MSG msg = {0};
    while (GetMessageW(&msg, nullptr, 0, 0))
    {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
}

