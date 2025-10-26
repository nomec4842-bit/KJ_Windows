#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "wdl/lice/lice.h"

#include <windows.h>
#include <commdlg.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
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
constexpr int kWaveDropdownSpacing = 4;
constexpr int kWaveDropdownOptionHeight = 24;

const std::array<TrackType, 2> kTrackTypeOptions = {TrackType::Synth, TrackType::Sample};
const std::array<SynthWaveType, 4> kSynthWaveOptions = {SynthWaveType::Sine, SynthWaveType::Square,
                                                        SynthWaveType::Saw, SynthWaveType::Triangle};

RECT playButton = {40, 40, 180, 110};
RECT loadSampleButton = {200, 40, 340, 110};
RECT waveSelectButton = {200, 40, 340, 110};
RECT bpmDownButton = {360, 55, 400, 95};
RECT bpmUpButton = {410, 55, 450, 95};
RECT stepCountDownButton = {470, 55, 510, 95};
RECT stepCountUpButton = {520, 55, 560, 95};
RECT pageDownButton = {580, 55, 620, 95};
RECT pageUpButton = {630, 55, 670, 95};
RECT addTrackButton = {690, 40, 780, 110};
RECT audioDeviceButton = {40, 115, 340, 145};
RECT pianoRollToggleButton {};
RECT mixerPanelRect {};
std::array<RECT, kSequencerStepsPerPage> stepRects;
int currentStepPage = 0;
int selectedTrackId = 0;
std::vector<int> trackTabIds;
std::vector<RECT> trackTabRects;
int openTrackTypeTrackId = 0;
bool audioDeviceDropdownOpen = false;
bool waveDropdownOpen = false;
int waveDropdownTrackId = 0;
HWND gMainWindow = nullptr;
HWND gPianoRollWindow = nullptr;
bool gPianoRollClassRegistered = false;

struct PianoRollDragState
{
    bool active = false;
    bool startedOnExistingNote = false;
    bool columnChanged = false;
    int trackId = 0;
    int startStep = -1;
    int midiNote = -1;
    int currentEndStep = -1;
    std::vector<int> appliedSteps;
};

PianoRollDragState gPianoRollDrag;

constexpr wchar_t kPianoRollWindowClassName[] = L"KJPianoRollWindow";
constexpr int kPianoRollWindowWidth = 640;
constexpr int kPianoRollWindowHeight = 360;
constexpr int kPianoRollMargin = 10;
constexpr int kPianoRollKeyboardWidth = 80;
constexpr int kPianoRollNoteRows = 24;
constexpr int kPianoRollLowestNote = 48; // C3
constexpr int kPianoRollHighestNote = kPianoRollLowestNote + kPianoRollNoteRows - 1;
constexpr COLORREF kPianoRollGridBackground = RGB(30, 30, 30);
constexpr COLORREF kPianoRollGridLine = RGB(60, 60, 60);
constexpr COLORREF kPianoRollActiveNote = RGB(0, 140, 220);
constexpr COLORREF kPianoRollPlayingColumn = RGB(50, 50, 50);
constexpr COLORREF kPianoRollKeyboardLight = RGB(80, 80, 80);
constexpr COLORREF kPianoRollKeyboardDark = RGB(45, 45, 45);

struct PianoRollLayout
{
    RECT client{};
    RECT grid{};
    RECT keyboard{};
    std::array<int, kSequencerStepsPerPage + 1> columnX{};
    std::array<int, kPianoRollNoteRows + 1> rowY{};
};

PianoRollLayout computePianoRollLayout(const RECT& client)
{
    PianoRollLayout layout{};
    layout.client = client;

    LONG clientLeft = client.left;
    LONG clientTop = client.top;
    LONG clientRight = client.right;
    LONG clientBottom = client.bottom;

    if (clientRight < clientLeft)
        std::swap(clientRight, clientLeft);
    if (clientBottom < clientTop)
        std::swap(clientBottom, clientTop);

    layout.grid.left = static_cast<LONG>(clientLeft + kPianoRollMargin + kPianoRollKeyboardWidth);
    layout.grid.top = static_cast<LONG>(clientTop + kPianoRollMargin);
    layout.grid.right = static_cast<LONG>(std::max(layout.grid.left, clientRight - kPianoRollMargin));
    layout.grid.bottom = static_cast<LONG>(std::max(layout.grid.top, clientBottom - kPianoRollMargin));

    layout.keyboard.left = static_cast<LONG>(clientLeft + kPianoRollMargin);
    layout.keyboard.top = layout.grid.top;
    layout.keyboard.right = layout.grid.left;
    layout.keyboard.bottom = layout.grid.bottom;

    int gridWidth = std::max<LONG>(0, layout.grid.right - layout.grid.left);
    int gridHeight = std::max<LONG>(0, layout.grid.bottom - layout.grid.top);

    int baseColumnWidth = kSequencerStepsPerPage > 0 ? gridWidth / kSequencerStepsPerPage : 0;
    int columnRemainder = kSequencerStepsPerPage > 0 ? gridWidth % kSequencerStepsPerPage : 0;
    int x = layout.grid.left;
    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        layout.columnX[i] = x;
        int increment = baseColumnWidth + (i < columnRemainder ? 1 : 0);
        x += increment;
    }
    layout.columnX[kSequencerStepsPerPage] = layout.grid.right;

    int baseRowHeight = kPianoRollNoteRows > 0 ? gridHeight / kPianoRollNoteRows : 0;
    int rowRemainder = kPianoRollNoteRows > 0 ? gridHeight % kPianoRollNoteRows : 0;
    int y = layout.grid.top;
    for (int i = 0; i < kPianoRollNoteRows; ++i)
    {
        layout.rowY[i] = y;
        int increment = baseRowHeight + (i < rowRemainder ? 1 : 0);
        y += increment;
    }
    layout.rowY[kPianoRollNoteRows] = layout.grid.bottom;

    return layout;
}

bool midiNoteIsBlack(int midiNote)
{
    int note = midiNote % 12;
    if (note < 0)
        note += 12;
    switch (note)
    {
    case 1:
    case 3:
    case 6:
    case 8:
    case 10:
        return true;
    default:
        return false;
    }
}

std::wstring midiNoteToLabel(int midiNote)
{
    static const wchar_t* kNames[12] = {L"C", L"C#", L"D", L"D#", L"E", L"F", L"F#", L"G", L"G#", L"A", L"A#", L"B"};
    int clamped = std::clamp(midiNote, 0, 127);
    int octave = clamped / 12 - 1;
    const wchar_t* name = kNames[clamped % 12];
    std::wstring label(name);
    label += std::to_wstring(octave);
    return label;
}

int midiNoteToRow(int midiNote)
{
    if (midiNote < kPianoRollLowestNote || midiNote > kPianoRollHighestNote)
        return -1;
    return kPianoRollHighestNote - midiNote;
}

void invalidatePianoRollWindow()
{
    if (gPianoRollWindow && IsWindow(gPianoRollWindow))
    {
        InvalidateRect(gPianoRollWindow, nullptr, FALSE);
    }
}

namespace
{

bool stepContainsMidiNote(int trackId, int stepIndex, int midiNote)
{
    auto notes = trackGetStepNotes(trackId, stepIndex);
    return std::find(notes.begin(), notes.end(), midiNote) != notes.end();
}

void pianoRollApplyDragRange(int newEndStep)
{
    if (!gPianoRollDrag.active)
        return;

    int trackId = gPianoRollDrag.trackId;
    if (trackId <= 0)
        return;

    int totalSteps = getSequencerStepCount(trackId);
    if (totalSteps <= 0)
        return;

    newEndStep = std::clamp(newEndStep, gPianoRollDrag.startStep, totalSteps - 1);

    if (newEndStep == gPianoRollDrag.currentEndStep)
        return;

    gPianoRollDrag.columnChanged = true;

    std::vector<int> newRange;
    newRange.reserve(newEndStep - gPianoRollDrag.startStep + 1);
    for (int step = gPianoRollDrag.startStep; step <= newEndStep; ++step)
    {
        newRange.push_back(step);
        if (!stepContainsMidiNote(trackId, step, gPianoRollDrag.midiNote))
        {
            trackToggleStepNote(trackId, step, gPianoRollDrag.midiNote);
        }
    }

    for (int step : gPianoRollDrag.appliedSteps)
    {
        if (std::find(newRange.begin(), newRange.end(), step) == newRange.end())
        {
            if (stepContainsMidiNote(trackId, step, gPianoRollDrag.midiNote))
            {
                trackToggleStepNote(trackId, step, gPianoRollDrag.midiNote);
            }
        }
    }

    gPianoRollDrag.appliedSteps = std::move(newRange);
    gPianoRollDrag.currentEndStep = newEndStep;

    invalidatePianoRollWindow();
    if (gMainWindow && IsWindow(gMainWindow))
    {
        InvalidateRect(gMainWindow, nullptr, FALSE);
    }
}

void pianoRollResetDrag()
{
    gPianoRollDrag.active = false;
    gPianoRollDrag.startedOnExistingNote = false;
    gPianoRollDrag.columnChanged = false;
    gPianoRollDrag.trackId = 0;
    gPianoRollDrag.startStep = -1;
    gPianoRollDrag.midiNote = -1;
    gPianoRollDrag.currentEndStep = -1;
    gPianoRollDrag.appliedSteps.clear();
}

} // namespace

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

struct WaveDropdownOption
{
    RECT rect;
    SynthWaveType type = SynthWaveType::Sine;
    bool isSelected = false;
};

std::vector<WaveDropdownOption> gWaveOptions;

constexpr float kMixerVolumeMin = 0.0f;
constexpr float kMixerVolumeMax = 1.0f;
constexpr float kMixerPanMin = -1.0f;
constexpr float kMixerPanMax = 1.0f;
constexpr float kMixerEqMin = -12.0f;
constexpr float kMixerEqMax = 12.0f;

struct SliderControlRects
{
    RECT control{};
    RECT track{};
};

struct KnobControlRects
{
    RECT control{};
    POINT center{};
    int radius = 0;
};

SliderControlRects gVolumeSliderControl{};
SliderControlRects gPanSliderControl{};
std::array<KnobControlRects, 3> gEqKnobControls{};

const std::array<const char*, 3> kEqBandLabels = {"Low EQ", "Mid EQ", "High EQ"};

constexpr double kPi = 3.14159265358979323846;
constexpr double kKnobMinAngleDegrees = -135.0;
constexpr double kKnobMaxAngleDegrees = 135.0;
constexpr double kKnobSweepDegrees = kKnobMaxAngleDegrees - kKnobMinAngleDegrees;

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
    // Cast for consistent LONG type usage
    x = static_cast<int>(std::clamp<LONG>(static_cast<LONG>(x), rect.left, static_cast<LONG>(maxX)));

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
    // Cast for consistent LONG type usage
    y = static_cast<int>(std::clamp<LONG>(static_cast<LONG>(y), rect.top, static_cast<LONG>(maxY)));

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

    const int toggleHeight = 18;
    pianoRollToggleButton.left = startX;
    pianoRollToggleButton.right = startX + 160;
    pianoRollToggleButton.bottom = startY - 2;
    pianoRollToggleButton.top = pianoRollToggleButton.bottom - toggleHeight;
    LONG minimumTop = kTrackTabsTop + kTrackTabHeight;
    if (pianoRollToggleButton.top < minimumTop)
    {
        pianoRollToggleButton.top = minimumTop;
        pianoRollToggleButton.bottom = pianoRollToggleButton.top + toggleHeight;
        if (pianoRollToggleButton.bottom > startY - 2)
        {
            pianoRollToggleButton.bottom = startY - 2;
            pianoRollToggleButton.top = pianoRollToggleButton.bottom - toggleHeight;
        }
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

    invalidatePianoRollWindow();
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

std::string synthWaveTypeToString(SynthWaveType type)
{
    switch (type)
    {
    case SynthWaveType::Sine:
        return "Sine";
    case SynthWaveType::Square:
        return "Square";
    case SynthWaveType::Saw:
        return "Saw";
    case SynthWaveType::Triangle:
        return "Triangle";
    }
    return "Wave";
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

void closePianoRollWindow()
{
    if (gPianoRollWindow && IsWindow(gPianoRollWindow))
    {
        DestroyWindow(gPianoRollWindow);
        gPianoRollWindow = nullptr;
    }
}

LRESULT CALLBACK PianoRollWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    switch (msg)
    {
    case WM_CREATE:
        SetTimer(hwnd, 1, 50, nullptr);
        return 0;
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        pianoRollResetDrag();
        if (hwnd == gPianoRollWindow)
        {
            gPianoRollWindow = nullptr;
            if (gMainWindow)
            {
                InvalidateRect(gMainWindow, nullptr, FALSE);
            }
        }
        return 0;
    case WM_TIMER:
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_LBUTTONDOWN:
    {
        RECT client;
        GetClientRect(hwnd, &client);
        PianoRollLayout layout = computePianoRollLayout(client);
        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (x >= layout.grid.left && x < layout.grid.right && y >= layout.grid.top && y < layout.grid.bottom)
        {
            int column = -1;
            for (int i = 0; i < kSequencerStepsPerPage; ++i)
            {
                if (x >= layout.columnX[i] && x < layout.columnX[i + 1])
                {
                    column = i;
                    break;
                }
            }

            int row = -1;
            for (int i = 0; i < kPianoRollNoteRows; ++i)
            {
                if (y >= layout.rowY[i] && y < layout.rowY[i + 1])
                {
                    row = i;
                    break;
                }
            }

            if (column >= 0 && row >= 0)
            {
                int trackId = getActiveSequencerTrackId();
                if (trackId > 0)
                {
                    int totalSteps = getSequencerStepCount(trackId);
                    if (totalSteps < 1)
                        totalSteps = kSequencerStepsPerPage;

                    int stepIndex = currentStepPage * kSequencerStepsPerPage + column;
                    if (stepIndex < totalSteps)
                    {
                        int midiNote = kPianoRollHighestNote - row;
                        std::vector<int> existingRange;
                        bool notePresent = stepContainsMidiNote(trackId, stepIndex, midiNote);
                        if (notePresent)
                        {
                            existingRange.push_back(stepIndex);
                            for (int step = stepIndex + 1; step < totalSteps; ++step)
                            {
                                if (stepContainsMidiNote(trackId, step, midiNote))
                                {
                                    existingRange.push_back(step);
                                }
                                else
                                {
                                    break;
                                }
                            }
                        }

                        gPianoRollDrag.active = true;
                        gPianoRollDrag.startedOnExistingNote = notePresent;
                        gPianoRollDrag.columnChanged = false;
                        gPianoRollDrag.trackId = trackId;
                        gPianoRollDrag.startStep = stepIndex;
                        gPianoRollDrag.midiNote = midiNote;
                        gPianoRollDrag.currentEndStep = stepIndex;
                        gPianoRollDrag.appliedSteps = std::move(existingRange);

                        SetCapture(hwnd);
                    }
                }
            }
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (!gPianoRollDrag.active)
            break;

        RECT client;
        GetClientRect(hwnd, &client);
        PianoRollLayout layout = computePianoRollLayout(client);
        if (layout.grid.right <= layout.grid.left)
            return 0;

        int x = GET_X_LPARAM(lParam);
        if (x < layout.grid.left)
            x = layout.grid.left;
        if (x >= layout.grid.right)
            x = layout.grid.right - 1;

        int column = 0;
        for (int i = 0; i < kSequencerStepsPerPage; ++i)
        {
            if (x >= layout.columnX[i] && x < layout.columnX[i + 1])
            {
                column = i;
                break;
            }
        }

        int trackId = gPianoRollDrag.trackId;
        if (trackId > 0)
        {
            int totalSteps = getSequencerStepCount(trackId);
            if (totalSteps > 0)
            {
                int stepIndex = currentStepPage * kSequencerStepsPerPage + column;
                if (stepIndex >= totalSteps)
                    stepIndex = totalSteps - 1;
                if (stepIndex >= gPianoRollDrag.startStep)
                {
                    pianoRollApplyDragRange(stepIndex);
                }
            }
        }
        return 0;
    }
    case WM_LBUTTONUP:
    {
        if (gPianoRollDrag.active)
        {
            if (GetCapture() == hwnd)
                ReleaseCapture();

            int trackId = gPianoRollDrag.trackId;
            int stepIndex = gPianoRollDrag.startStep;
            int midiNote = gPianoRollDrag.midiNote;
            bool columnChanged = gPianoRollDrag.columnChanged;
            bool startedOnExisting = gPianoRollDrag.startedOnExistingNote;

            if (!columnChanged && trackId > 0 && stepIndex >= 0)
            {
                if (startedOnExisting)
                {
                    trackToggleStepNote(trackId, stepIndex, midiNote);
                }
                else if (!stepContainsMidiNote(trackId, stepIndex, midiNote))
                {
                    trackToggleStepNote(trackId, stepIndex, midiNote);
                }
            }

            pianoRollResetDrag();
            invalidatePianoRollWindow();
            if (gMainWindow && IsWindow(gMainWindow))
            {
                InvalidateRect(gMainWindow, nullptr, FALSE);
            }
        }
        return 0;
    }
    case WM_PAINT:
    {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        RECT client;
        GetClientRect(hwnd, &client);

        HBRUSH background = CreateSolidBrush(RGB(20, 20, 20));
        FillRect(hdc, &client, background);
        DeleteObject(background);

        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(230, 230, 230));

        PianoRollLayout layout = computePianoRollLayout(client);

        HBRUSH gridBrush = CreateSolidBrush(kPianoRollGridBackground);
        FillRect(hdc, &layout.grid, gridBrush);
        DeleteObject(gridBrush);

        int trackId = getActiveSequencerTrackId();
        int totalSteps = 0;
        if (trackId > 0)
            totalSteps = getSequencerStepCount(trackId);
        if (totalSteps < 1)
            totalSteps = kSequencerStepsPerPage;

        int startStep = currentStepPage * kSequencerStepsPerPage;
        int endStep = startStep + kSequencerStepsPerPage;

        if (trackId > 0)
        {
            HBRUSH disabledBrush = CreateSolidBrush(RGB(24, 24, 24));
            for (int column = 0; column < kSequencerStepsPerPage; ++column)
            {
                int stepIndex = startStep + column;
                if (stepIndex >= totalSteps)
                {
                    RECT disabledRect {layout.columnX[column], layout.grid.top, layout.columnX[column + 1], layout.grid.bottom};
                    FillRect(hdc, &disabledRect, disabledBrush);
                }
            }
            DeleteObject(disabledBrush);
        }

        int playingStep = sequencerCurrentStep.load(std::memory_order_relaxed);
        if (playingStep >= startStep && playingStep < endStep)
        {
            int column = playingStep - startStep;
            RECT columnRect {layout.columnX[column], layout.grid.top, layout.columnX[column + 1], layout.grid.bottom};
            HBRUSH playingBrush = CreateSolidBrush(kPianoRollPlayingColumn);
            FillRect(hdc, &columnRect, playingBrush);
            DeleteObject(playingBrush);
        }

        std::array<std::vector<int>, kSequencerStepsPerPage> columnNotes;
        if (trackId > 0)
        {
            for (int column = 0; column < kSequencerStepsPerPage; ++column)
            {
                int stepIndex = startStep + column;
                if (stepIndex >= totalSteps)
                    continue;

                auto notes = trackGetStepNotes(trackId, stepIndex);
                if (notes.empty() && getTrackStepState(trackId, stepIndex))
                {
                    int fallback = trackGetStepNote(trackId, stepIndex);
                    if (fallback >= 0)
                        notes.push_back(fallback);
                }
                columnNotes[column] = std::move(notes);
            }

            for (int row = 0; row < kPianoRollNoteRows; ++row)
            {
                int midiNote = kPianoRollHighestNote - row;
                int column = 0;
                while (column < kSequencerStepsPerPage)
                {
                    int stepIndex = startStep + column;
                    if (stepIndex >= totalSteps)
                        break;

                    const auto& notes = columnNotes[column];
                    bool active = std::find(notes.begin(), notes.end(), midiNote) != notes.end();
                    if (active)
                    {
                        int endColumn = column + 1;
                        while (endColumn < kSequencerStepsPerPage)
                        {
                            int nextStep = startStep + endColumn;
                            if (nextStep >= totalSteps)
                                break;

                            const auto& nextNotes = columnNotes[endColumn];
                            if (std::find(nextNotes.begin(), nextNotes.end(), midiNote) == nextNotes.end())
                                break;
                            ++endColumn;
                        }

                        RECT cellRect {layout.columnX[column], layout.rowY[row], layout.columnX[endColumn], layout.rowY[row + 1]};
                        HBRUSH noteBrush = CreateSolidBrush(kPianoRollActiveNote);
                        FillRect(hdc, &cellRect, noteBrush);
                        DeleteObject(noteBrush);
                        column = endColumn;
                    }
                    else
                    {
                        ++column;
                    }
                }
            }
        }

        for (int row = 0; row < kPianoRollNoteRows; ++row)
        {
            int midiNote = kPianoRollHighestNote - row;
            RECT keyRect {layout.keyboard.left, layout.rowY[row], layout.keyboard.right, layout.rowY[row + 1]};
            COLORREF keyColor = midiNoteIsBlack(midiNote) ? kPianoRollKeyboardDark : kPianoRollKeyboardLight;
            HBRUSH keyBrush = CreateSolidBrush(keyColor);
            FillRect(hdc, &keyRect, keyBrush);
            DeleteObject(keyBrush);

            RECT labelRect = keyRect;
            labelRect.left += 6;
            std::wstring label = midiNoteToLabel(midiNote);
            DrawTextW(hdc, label.c_str(), -1, &labelRect, DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        }

        HPEN gridPen = CreatePen(PS_SOLID, 1, kPianoRollGridLine);
        HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, gridPen));
        for (int row = 0; row <= kPianoRollNoteRows; ++row)
        {
            MoveToEx(hdc, layout.grid.left, layout.rowY[row], nullptr);
            LineTo(hdc, layout.grid.right, layout.rowY[row]);
        }
        for (int column = 0; column <= kSequencerStepsPerPage; ++column)
        {
            MoveToEx(hdc, layout.columnX[column], layout.grid.top, nullptr);
            LineTo(hdc, layout.columnX[column], layout.grid.bottom);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(gridPen);

        HBRUSH borderBrush = CreateSolidBrush(RGB(15, 15, 15));
        FrameRect(hdc, &layout.keyboard, borderBrush);
        FrameRect(hdc, &layout.grid, borderBrush);
        DeleteObject(borderBrush);

        if (trackId <= 0)
        {
            std::wstring message = L"Select a track to edit the piano roll.";
            DrawTextW(hdc, message.c_str(), -1, &layout.grid, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        }

        EndPaint(hwnd, &ps);
        return 0;
    }
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensurePianoRollWindowClass()
{
    if (gPianoRollClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = PianoRollWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kPianoRollWindowClassName;
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
    {
        gPianoRollClassRegistered = true;
    }
}

void togglePianoRollWindow(HWND parent)
{
    if (gPianoRollWindow && IsWindow(gPianoRollWindow))
    {
        closePianoRollWindow();
        if (gMainWindow)
        {
            InvalidateRect(gMainWindow, nullptr, FALSE);
        }
        return;
    }

    ensurePianoRollWindowClass();
    if (!gPianoRollClassRegistered)
        return;

    RECT parentRect {0, 0, 0, 0};
    if (parent && IsWindow(parent))
    {
        GetWindowRect(parent, &parentRect);
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parentRect.right > parentRect.left && parentRect.bottom > parentRect.top)
    {
        x = parentRect.left + 40;
        y = parentRect.top + 80;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kPianoRollWindowClassName,
                                L"Piano Roll",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kPianoRollWindowWidth,
                                kPianoRollWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gPianoRollWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
    }
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

double computeNormalized(double value, double minValue, double maxValue)
{
    if (maxValue <= minValue)
        return 0.0;
    double normalized = (value - minValue) / (maxValue - minValue);
    return std::clamp(normalized, 0.0, 1.0);
}

float sliderValueFromPosition(const SliderControlRects& slider, int x, float minValue, float maxValue)
{
    int trackWidth = slider.track.right - slider.track.left;
    if (trackWidth <= 0)
        return minValue;

    // Cast for consistent LONG type usage
    int clampedX = static_cast<int>(std::clamp<LONG>(static_cast<LONG>(x), slider.track.left, slider.track.right));
    double normalized = static_cast<double>(clampedX - slider.track.left) / static_cast<double>(trackWidth);
    double value = static_cast<double>(minValue) + normalized * (static_cast<double>(maxValue) - static_cast<double>(minValue));
    float result = static_cast<float>(value);
    return std::clamp(result, minValue, maxValue);
}

void fillCircle(LICE_SysBitmap& surface, int centerX, int centerY, int radius, COLORREF color)
{
    if (radius <= 0)
        return;

    int radiusSquared = radius * radius;
    LICE_pixel pixelColor = LICE_ColorFromCOLORREF(color);
    for (int dy = -radius; dy <= radius; ++dy)
    {
        int span = static_cast<int>(std::sqrt(static_cast<double>(radiusSquared - dy * dy)));
        int rowX = centerX - span;
        int rowWidth = span * 2 + 1;
        LICE_FillRect(&surface, rowX, centerY + dy, rowWidth, 1, pixelColor);
    }
}

void drawLine(LICE_SysBitmap& surface, int x0, int y0, int x1, int y1, COLORREF color)
{
    LICE_pixel pixelColor = LICE_ColorFromCOLORREF(color);
    int dx = std::abs(x1 - x0);
    int sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0);
    int sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;

    while (true)
    {
        LICE_FillRect(&surface, x0, y0, 1, 1, pixelColor);
        if (x0 == x1 && y0 == y1)
            break;
        int e2 = 2 * err;
        if (e2 >= dy)
        {
            err += dy;
            x0 += sx;
        }
        if (e2 <= dx)
        {
            err += dx;
            y0 += sy;
        }
    }
}

float knobValueFromPosition(const KnobControlRects& knob, int x, int y, float minValue, float maxValue)
{
    if (knob.radius <= 0)
        return std::clamp(minValue, minValue, maxValue);

    double dx = static_cast<double>(x - knob.center.x);
    double dy = static_cast<double>(knob.center.y - y);
    double angleDegrees = std::atan2(dy, dx) * (180.0 / kPi);
    double clampedAngle = std::clamp(angleDegrees, kKnobMinAngleDegrees, kKnobMaxAngleDegrees);
    double normalized = (clampedAngle - kKnobMinAngleDegrees) / kKnobSweepDegrees;
    double value = static_cast<double>(minValue) + normalized * (static_cast<double>(maxValue) - static_cast<double>(minValue));
    float result = static_cast<float>(value);
    return std::clamp(result, minValue, maxValue);
}

void drawKnobControl(LICE_SysBitmap& surface, KnobControlRects& knobRects, const RECT& area, double normalizedValue,
                     const char* label, const std::string& valueText)
{
    knobRects.control = area;
    knobRects.center = {0, 0};
    knobRects.radius = 0;

    int width = area.right - area.left;
    int height = area.bottom - area.top;
    if (width <= 0 || height <= 0)
        return;

    LICE_FillRect(&surface, area.left, area.top, width, height, LICE_ColorFromCOLORREF(RGB(35, 35, 35)));
    LICE_DrawRect(&surface, area.left, area.top, width, height, LICE_ColorFromCOLORREF(RGB(70, 70, 70)));

    RECT labelRect = area;
    labelRect.bottom = std::min(labelRect.top + 18, area.bottom);
    drawText(surface, labelRect, label, RGB(220, 220, 220), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT valueRect = area;
    valueRect.top = std::max<LONG>(valueRect.bottom - 18, area.top);
    drawText(surface, valueRect, valueText.c_str(), RGB(200, 200, 200), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT knobRect = area;
    knobRect.top = labelRect.bottom + 6;
    knobRect.bottom = valueRect.top - 6;
    knobRect.left += 10;
    knobRect.right -= 10;

    if (knobRect.bottom <= knobRect.top || knobRect.right <= knobRect.left)
        return;

    int knobWidth = knobRect.right - knobRect.left;
    int knobHeight = knobRect.bottom - knobRect.top;
    int diameter = std::min(knobWidth, knobHeight);
    if (diameter <= 0)
        return;

    knobRects.center.x = knobRect.left + knobWidth / 2;
    knobRects.center.y = knobRect.top + knobHeight / 2;
    int radius = std::max(diameter / 2 - 2, 0);
    knobRects.radius = radius;
    if (radius <= 0)
        return;

    fillCircle(surface, knobRects.center.x, knobRects.center.y, radius, RGB(28, 28, 28));
    if (radius > 2)
        fillCircle(surface, knobRects.center.x, knobRects.center.y, radius - 2, RGB(70, 70, 70));
    if (radius > 5)
        fillCircle(surface, knobRects.center.x, knobRects.center.y, radius - 5, RGB(110, 110, 110));
    if (radius > 9)
        fillCircle(surface, knobRects.center.x, knobRects.center.y, radius - 9, RGB(150, 150, 150));

    auto drawTick = [&](double angleDegrees, int thickness, COLORREF color) {
        double angleRad = angleDegrees * (kPi / 180.0);
        int outerX = knobRects.center.x + static_cast<int>(std::round(std::cos(angleRad) * static_cast<double>(radius)));
        int outerY = knobRects.center.y - static_cast<int>(std::round(std::sin(angleRad) * static_cast<double>(radius)));
        int innerRadius = std::max(radius - thickness, 0);
        int innerX = knobRects.center.x + static_cast<int>(std::round(std::cos(angleRad) * static_cast<double>(innerRadius)));
        int innerY = knobRects.center.y - static_cast<int>(std::round(std::sin(angleRad) * static_cast<double>(innerRadius)));
        drawLine(surface, innerX, innerY, outerX, outerY, color);
    };

    drawTick(kKnobMinAngleDegrees, 6, RGB(40, 40, 40));
    drawTick(0.0, 6, RGB(50, 50, 50));
    drawTick(kKnobMaxAngleDegrees, 6, RGB(40, 40, 40));

    double clampedNorm = std::clamp(normalizedValue, 0.0, 1.0);
    double indicatorAngleDeg = kKnobMinAngleDegrees + clampedNorm * kKnobSweepDegrees;
    double indicatorRad = indicatorAngleDeg * (kPi / 180.0);

    int indicatorInner = std::max(radius / 4, 2);
    int indicatorOuter = std::max(radius - 6, indicatorInner + 1);
    int startX = knobRects.center.x + static_cast<int>(std::round(std::cos(indicatorRad) * static_cast<double>(indicatorInner)));
    int startY = knobRects.center.y - static_cast<int>(std::round(std::sin(indicatorRad) * static_cast<double>(indicatorInner)));
    int endX = knobRects.center.x + static_cast<int>(std::round(std::cos(indicatorRad) * static_cast<double>(indicatorOuter)));
    int endY = knobRects.center.y - static_cast<int>(std::round(std::sin(indicatorRad) * static_cast<double>(indicatorOuter)));
    drawLine(surface, startX, startY, endX, endY, RGB(0, 200, 255));
}

std::string formatVolumeValue(float volume)
{
    int percent = static_cast<int>(std::round(std::clamp(volume, kMixerVolumeMin, kMixerVolumeMax) * 100.0f));
    return std::to_string(percent) + "%";
}

std::string formatPanValue(float pan)
{
    float clamped = std::clamp(pan, kMixerPanMin, kMixerPanMax);
    if (std::abs(clamped) < 0.01f)
        return "Center";

    float percent = std::round(std::abs(clamped) * 100.0f);
    std::ostringstream oss;
    oss << (clamped < 0.0f ? "L " : "R ") << static_cast<int>(percent) << "%";
    return oss.str();
}

std::string formatEqValue(float gainDb)
{
    float clamped = std::clamp(gainDb, kMixerEqMin, kMixerEqMax);
    std::ostringstream oss;
    oss << std::showpos << std::fixed << std::setprecision(1) << clamped << " dB";
    return oss.str();
}

void drawSliderControl(LICE_SysBitmap& surface, SliderControlRects& sliderRects, const RECT& area, double normalizedValue,
                       const char* label, const std::string& valueText)
{
    sliderRects.control = area;

    int width = area.right - area.left;
    int height = area.bottom - area.top;
    if (width <= 0 || height <= 0)
    {
        sliderRects.track = {0, 0, 0, 0};
        return;
    }

    LICE_FillRect(&surface, area.left, area.top, width, height, LICE_ColorFromCOLORREF(RGB(35, 35, 35)));
    LICE_DrawRect(&surface, area.left, area.top, width, height, LICE_ColorFromCOLORREF(RGB(70, 70, 70)));

    RECT labelRect = area;
    labelRect.bottom = std::min(labelRect.top + 18, area.bottom);
    drawText(surface, labelRect, label, RGB(220, 220, 220), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT valueRect = area;
    // Cast for consistent LONG type usage
    valueRect.top = std::max<LONG>(valueRect.bottom - 18, area.top);
    drawText(surface, valueRect, valueText.c_str(), RGB(200, 200, 200), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT trackRect = area;
    trackRect.top = labelRect.bottom + 6;
    trackRect.bottom = valueRect.top - 6;
    if (trackRect.bottom <= trackRect.top)
    {
        int mid = (area.top + area.bottom) / 2;
        trackRect.top = mid - 4;
        trackRect.bottom = mid + 4;
    }
    trackRect.left += 10;
    trackRect.right -= 10;
    if (trackRect.right <= trackRect.left)
    {
        trackRect.right = trackRect.left + 20;
    }

    // Cast for consistent LONG type usage
    int trackHeight = static_cast<int>(std::max<LONG>(4, trackRect.bottom - trackRect.top));
    int trackTop = trackRect.top;
    int trackWidth = trackRect.right - trackRect.left;
    LICE_FillRect(&surface, trackRect.left, trackTop, trackWidth, trackHeight, LICE_ColorFromCOLORREF(RGB(55, 55, 55)));
    LICE_DrawRect(&surface, trackRect.left, trackTop, trackWidth, trackHeight, LICE_ColorFromCOLORREF(RGB(90, 90, 90)));

    double clampedNorm = std::clamp(normalizedValue, 0.0, 1.0);
    int handleWidth = 12;
    int handleRange = std::max(trackWidth - handleWidth, 1);
    int handleX = trackRect.left + static_cast<int>(std::round(clampedNorm * handleRange));
    RECT handleRect {handleX, trackTop - 4, handleX + handleWidth, trackTop + trackHeight + 4};
    LICE_FillRect(&surface, handleRect.left, handleRect.top, handleRect.right - handleRect.left,
                  handleRect.bottom - handleRect.top, LICE_ColorFromCOLORREF(RGB(0, 120, 200)));
    LICE_DrawRect(&surface, handleRect.left, handleRect.top, handleRect.right - handleRect.left,
                  handleRect.bottom - handleRect.top, LICE_ColorFromCOLORREF(RGB(20, 20, 20)));

    sliderRects.track = trackRect;
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

void drawMixerControls(LICE_SysBitmap& surface, const RECT& client, const Track* activeTrack)
{
    int panelLeft = 40;
    int panelRight = client.right - 40;
    if (panelRight <= panelLeft)
    {
        mixerPanelRect = {0, 0, 0, 0};
        gVolumeSliderControl = {};
        gPanSliderControl = {};
        for (auto& knob : gEqKnobControls)
            knob = {};
        return;
    }

    int panelTop = stepRects[0].bottom + 40;
    int panelPadding = 20;
    int headerHeight = 24;
    int sliderHeight = 70;
    int horizontalSpacing = 30;
    int verticalSpacing = 24;
    int knobHeight = 110;

    RECT headerRect {panelLeft + panelPadding, panelTop + panelPadding,
                     panelRight - panelPadding, panelTop + panelPadding + headerHeight};

    int sliderRowTop = headerRect.bottom + 10;
    RECT volumeRect {panelLeft + panelPadding, sliderRowTop,
                     panelLeft + panelPadding + 220, sliderRowTop + sliderHeight};
    RECT panRect {volumeRect.right + horizontalSpacing, sliderRowTop,
                  volumeRect.right + horizontalSpacing + 220, sliderRowTop + sliderHeight};

    int eqAvailableWidth = panelRight - panelLeft - panelPadding * 2;
    int eqSpacing = horizontalSpacing;
    int eqWidth = (eqAvailableWidth - eqSpacing * 2) / 3;
    if (eqWidth < 100)
        eqWidth = 100;
    int eqRowTop = volumeRect.bottom + verticalSpacing;
    RECT eqRect0 {panelLeft + panelPadding, eqRowTop,
                  panelLeft + panelPadding + eqWidth, eqRowTop + knobHeight};
    RECT eqRect1 {eqRect0.right + eqSpacing, eqRowTop,
                  eqRect0.right + eqSpacing + eqWidth, eqRowTop + knobHeight};
    RECT eqRect2 {eqRect1.right + eqSpacing, eqRowTop,
                  eqRect1.right + eqSpacing + eqWidth, eqRowTop + knobHeight};

    int panelBottom = eqRect0.bottom + panelPadding;
    mixerPanelRect = {panelLeft, panelTop, panelRight, panelBottom};

    int width = mixerPanelRect.right - mixerPanelRect.left;
    int height = mixerPanelRect.bottom - mixerPanelRect.top;
    if (width <= 0 || height <= 0)
        return;

    LICE_FillRect(&surface, mixerPanelRect.left, mixerPanelRect.top, width, height,
                  LICE_ColorFromCOLORREF(RGB(30, 30, 30)));
    LICE_DrawRect(&surface, mixerPanelRect.left, mixerPanelRect.top, width, height,
                  LICE_ColorFromCOLORREF(RGB(70, 70, 70)));

    drawText(surface, headerRect, "Mixer", RGB(230, 230, 230), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    if (!activeTrack)
    {
        RECT messageRect {headerRect.left, headerRect.bottom + 20, headerRect.right, headerRect.bottom + 60};
        drawText(surface, messageRect, "Select a track to adjust mixer settings.", RGB(200, 200, 200),
                 DT_LEFT | DT_VCENTER | DT_SINGLELINE);
        gVolumeSliderControl = {};
        gPanSliderControl = {};
        for (auto& knob : gEqKnobControls)
            knob = {};
        return;
    }

    double volumeNorm = computeNormalized(activeTrack->volume, kMixerVolumeMin, kMixerVolumeMax);
    double panNorm = computeNormalized(activeTrack->pan, kMixerPanMin, kMixerPanMax);
    double lowNorm = computeNormalized(activeTrack->lowGainDb, kMixerEqMin, kMixerEqMax);
    double midNorm = computeNormalized(activeTrack->midGainDb, kMixerEqMin, kMixerEqMax);
    double highNorm = computeNormalized(activeTrack->highGainDb, kMixerEqMin, kMixerEqMax);

    drawSliderControl(surface, gVolumeSliderControl, volumeRect, volumeNorm,
                      "Volume", formatVolumeValue(activeTrack->volume));
    drawSliderControl(surface, gPanSliderControl, panRect, panNorm,
                      "Pan", formatPanValue(activeTrack->pan));

    drawKnobControl(surface, gEqKnobControls[0], eqRect0, lowNorm,
                    kEqBandLabels[0], formatEqValue(activeTrack->lowGainDb));
    drawKnobControl(surface, gEqKnobControls[1], eqRect1, midNorm,
                    kEqBandLabels[1], formatEqValue(activeTrack->midGainDb));
    drawKnobControl(surface, gEqKnobControls[2], eqRect2, highNorm,
                    kEqBandLabels[2], formatEqValue(activeTrack->highGainDb));
}

void renderUI(LICE_SysBitmap& surface, const RECT& client)
{
    LICE_Clear(&surface, LICE_ColorFromCOLORREF(RGB(20, 20, 20)));
    gAudioDeviceOptions.clear();
    gWaveOptions.clear();

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

    const Track* activeTrackPtr = findTrackById(tracks, activeTrackId);
    Track fallbackTrack{};
    if (!activeTrackPtr && activeTrackId > 0)
    {
        fallbackTrack.id = activeTrackId;
        fallbackTrack.type = trackGetType(activeTrackId);
        fallbackTrack.synthWaveType = trackGetSynthWaveType(activeTrackId);
        fallbackTrack.volume = trackGetVolume(activeTrackId);
        fallbackTrack.pan = trackGetPan(activeTrackId);
        fallbackTrack.lowGainDb = trackGetEqLowGain(activeTrackId);
        fallbackTrack.midGainDb = trackGetEqMidGain(activeTrackId);
        fallbackTrack.highGainDb = trackGetEqHighGain(activeTrackId);
        activeTrackPtr = &fallbackTrack;
    }

    bool showSampleLoader = false;
    bool showWaveSelector = false;
    SynthWaveType activeWaveType = SynthWaveType::Sine;
    if (activeTrackPtr)
    {
        if (activeTrackPtr->type == TrackType::Sample)
        {
            showSampleLoader = true;
        }
        else if (activeTrackPtr->type == TrackType::Synth)
        {
            showWaveSelector = true;
            activeWaveType = activeTrackPtr->synthWaveType;
        }
    }

    if (waveDropdownOpen && (!showWaveSelector || waveDropdownTrackId != activeTrackId))
    {
        waveDropdownOpen = false;
        waveDropdownTrackId = 0;
    }

    if (showSampleLoader)
    {
        drawButton(surface, loadSampleButton,
                   RGB(50, 50, 50), RGB(120, 120, 120),
                   "Load Sample");
    }
    else if (showWaveSelector)
    {
        std::string waveLabel = synthWaveTypeToString(activeWaveType);
        drawButton(surface, waveSelectButton,
                   RGB(50, 50, 50), RGB(120, 120, 120),
                   waveLabel.c_str());

        if (waveDropdownOpen && waveDropdownTrackId == activeTrackId)
        {
            RECT optionRect = waveSelectButton;
            optionRect.top = waveSelectButton.bottom + kWaveDropdownSpacing;
            optionRect.bottom = optionRect.top + kWaveDropdownOptionHeight;

            for (SynthWaveType option : kSynthWaveOptions)
            {
                WaveDropdownOption dropdownOption{};
                dropdownOption.rect = optionRect;
                dropdownOption.type = option;
                dropdownOption.isSelected = option == activeWaveType;
                gWaveOptions.push_back(dropdownOption);

                COLORREF optionFill = dropdownOption.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
                COLORREF optionOutline = dropdownOption.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
                std::string optionLabel = synthWaveTypeToString(option);
                drawButton(surface, optionRect, optionFill, optionOutline, optionLabel.c_str());

                optionRect.top = optionRect.bottom + kWaveDropdownSpacing;
                optionRect.bottom = optionRect.top + kWaveDropdownOptionHeight;
            }
        }
    }

    drawButton(surface, bpmDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, bpmUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, stepCountDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "-");
    drawButton(surface, stepCountUpButton, RGB(50, 50, 50), RGB(120, 120, 120), "+");
    drawButton(surface, pageDownButton, RGB(50, 50, 50), RGB(120, 120, 120), "<");
    drawButton(surface, pageUpButton, RGB(50, 50, 50), RGB(120, 120, 120), ">");
    drawButton(surface, addTrackButton, RGB(50, 50, 50), RGB(120, 120, 120), "+Track");

    bool pianoRollOpen = gPianoRollWindow && IsWindow(gPianoRollWindow);
    COLORREF pianoRollFill = pianoRollOpen ? RGB(0, 90, 160) : RGB(50, 50, 50);
    COLORREF pianoRollOutline = pianoRollOpen ? RGB(20, 20, 20) : RGB(120, 120, 120);
    drawButton(surface, pianoRollToggleButton, pianoRollFill, pianoRollOutline,
               pianoRollOpen ? "Hide Piano Roll" : "Show Piano Roll");

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

    drawMixerControls(surface, client, activeTrackPtr);

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
                    waveDropdownOpen = false;
                    waveDropdownTrackId = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            if (!pointInRect(audioDeviceButton, x, y))
            {
                audioDeviceDropdownOpen = false;
                waveDropdownOpen = false;
                waveDropdownTrackId = 0;
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
            waveDropdownOpen = false;
            waveDropdownTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (pointInRect(pianoRollToggleButton, x, y))
        {
            togglePianoRollWindow(hwnd);
            audioDeviceDropdownOpen = false;
            waveDropdownOpen = false;
            waveDropdownTrackId = 0;
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

        bool showSampleLoader = false;
        bool showWaveSelector = false;
        if (const Track* activeTrack = findTrackById(tracks, activeTrackId))
        {
            showSampleLoader = activeTrack->type == TrackType::Sample;
            showWaveSelector = activeTrack->type == TrackType::Synth;
        }
        else if (activeTrackId > 0)
        {
            TrackType trackType = trackGetType(activeTrackId);
            showSampleLoader = trackType == TrackType::Sample;
            showWaveSelector = trackType == TrackType::Synth;
        }

        if (!showWaveSelector)
        {
            waveDropdownOpen = false;
            waveDropdownTrackId = 0;
        }

        bool waveDropdownWasOpen = waveDropdownOpen;
        int previousWaveDropdownTrack = waveDropdownTrackId;

        if (waveDropdownOpen && waveDropdownTrackId == activeTrackId)
        {
            for (const auto& option : gWaveOptions)
            {
                if (pointInRect(option.rect, x, y))
                {
                    trackSetSynthWaveType(activeTrackId, option.type);
                    waveDropdownOpen = false;
                    waveDropdownTrackId = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
        }

        if (showWaveSelector && pointInRect(waveSelectButton, x, y))
        {
            if (waveDropdownOpen && waveDropdownTrackId == activeTrackId)
            {
                waveDropdownOpen = false;
                waveDropdownTrackId = 0;
            }
            else
            {
                waveDropdownOpen = true;
                waveDropdownTrackId = activeTrackId;
            }
            openTrackTypeTrackId = 0;
            audioDeviceDropdownOpen = false;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (activeTrackId > 0)
        {
            if (pointInRect(gVolumeSliderControl.control, x, y))
            {
                float newVolume = sliderValueFromPosition(gVolumeSliderControl, x, kMixerVolumeMin, kMixerVolumeMax);
                trackSetVolume(activeTrackId, newVolume);
                openTrackTypeTrackId = 0;
                waveDropdownOpen = false;
                audioDeviceDropdownOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            if (pointInRect(gPanSliderControl.control, x, y))
            {
                float newPan = sliderValueFromPosition(gPanSliderControl, x, kMixerPanMin, kMixerPanMax);
                trackSetPan(activeTrackId, newPan);
                openTrackTypeTrackId = 0;
                waveDropdownOpen = false;
                audioDeviceDropdownOpen = false;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
            }

            for (size_t eqIndex = 0; eqIndex < gEqKnobControls.size(); ++eqIndex)
            {
                if (pointInRect(gEqKnobControls[eqIndex].control, x, y))
                {
                    float newGain = knobValueFromPosition(gEqKnobControls[eqIndex], x, y, kMixerEqMin, kMixerEqMax);
                    switch (eqIndex)
                    {
                    case 0:
                        trackSetEqLowGain(activeTrackId, newGain);
                        break;
                    case 1:
                        trackSetEqMidGain(activeTrackId, newGain);
                        break;
                    case 2:
                        trackSetEqHighGain(activeTrackId, newGain);
                        break;
                    default:
                        break;
                    }
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }
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
                        if (option != TrackType::Synth && waveDropdownTrackId == track.id)
                        {
                            waveDropdownOpen = false;
                            waveDropdownTrackId = 0;
                        }
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
                waveDropdownOpen = false;
                waveDropdownTrackId = 0;
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

                if (waveDropdownOpen)
                {
                    waveDropdownOpen = false;
                    waveDropdownTrackId = 0;
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
                    invalidatePianoRollWindow();
                }
                return 0;
            }
        }

        if (previousDropdownTrack != 0 && openTrackTypeTrackId == previousDropdownTrack)
        {
            openTrackTypeTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (waveDropdownWasOpen && waveDropdownOpen && waveDropdownTrackId == previousWaveDropdownTrack)
        {
            waveDropdownOpen = false;
            waveDropdownTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
        }

        if (pointInRect(playButton, x, y))
        {
            bool playing = isPlaying.load(std::memory_order_relaxed);
            isPlaying.store(!playing, std::memory_order_relaxed);
            requestSequencerReset();
            InvalidateRect(hwnd, nullptr, FALSE);
            invalidatePianoRollWindow();
            return 0;
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
            invalidatePianoRollWindow();
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
            invalidatePianoRollWindow();
            return 0;
        }

        if (pointInRect(pageDownButton, x, y))
        {
            if (currentStepPage > 0)
            {
                --currentStepPage;
                InvalidateRect(hwnd, nullptr, FALSE);
                invalidatePianoRollWindow();
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
                invalidatePianoRollWindow();
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
            invalidatePianoRollWindow();
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
                    invalidatePianoRollWindow();
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
        closePianoRollWindow();
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

