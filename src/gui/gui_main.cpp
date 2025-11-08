#include "gui/gui_main.h"
#include "core/audio_engine.h"
#include "core/project_io.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_refresh.h"
#include "gui/menu_commands.h"
#include "gui/compressor_window.h"
#include "gui/waveform_window.h"
#include "wdl/lice/lice.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commdlg.h>
#include <commctrl.h>
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

std::string formatVolumeValue(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value << " dB";
    return stream.str();
}

std::string formatPanValue(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value << " L/R";
    return stream.str();
}

std::string formatEqValue(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value << " dB";
    return stream.str();
}

std::string formatDelayTimeValue(float value)
{
    std::ostringstream stream;
    int precision = value < 100.0f ? 1 : 0;
    stream << std::fixed << std::setprecision(precision) << value << " ms";
    return stream.str();
}

std::string formatDelayPercentValue(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(1) << (value * 100.0f) << " %";
    return stream.str();
}

void notifyEffectsWindowTrackValuesChanged(int trackId)
{
    (void)trackId;
    // TODO: update FX window when track parameters change.
}

std::string formatNormalizedValue(float value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2) << value;
    return stream.str();
}

std::string formatPitchValue(float value)
{
    int semitones = static_cast<int>(std::lround(value));
    std::ostringstream stream;
    stream << std::showpos << semitones << " st";
    return stream.str();
}

std::string formatPitchRangeValue(float value)
{
    int semitones = static_cast<int>(std::lround(value));
    std::ostringstream stream;
    stream << semitones << " st";
    return stream.str();
}

std::string formatSecondsValue(float value)
{
    std::ostringstream stream;
    int precision = value < 1.0f ? 3 : 2;
    stream << std::fixed << std::setprecision(precision) << value << " s";
    return stream.str();
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

LONG computeDropdownStartTop(const RECT& anchor, int optionCount, int optionHeight, int spacing, const RECT& client)
{
    if (optionCount <= 0)
        return anchor.bottom;

    LONG totalHeight = static_cast<LONG>(optionCount * optionHeight + (optionCount - 1) * spacing);
    LONG belowStart = anchor.bottom + spacing;
    if (belowStart + totalHeight <= client.bottom)
        return belowStart;

    LONG aboveStart = anchor.top - spacing - totalHeight;
    if (aboveStart >= client.top)
        return aboveStart;

    LONG minTop = client.top;
    LONG maxTop = client.bottom - totalHeight;
    if (maxTop < minTop)
        maxTop = minTop;

    if (belowStart < minTop)
        return minTop;
    if (belowStart > maxTop)
        return maxTop;
    return belowStart;
}

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
RECT effectsToggleButton {};
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
int gPianoRollSelectedMenuTab = 0;
bool gPianoRollMenuCollapsed = false;
HWND gEffectsWindow = nullptr;
bool gEffectsWindowClassRegistered = false;
HWND gSidechainWindow = nullptr;
bool gSidechainWindowClassRegistered = false;
HWND gEqWindow = nullptr;
bool gEqWindowClassRegistered = false;
HWND gDelayWindow = nullptr;
bool gDelayWindowClassRegistered = false;
HMENU gViewMenu = nullptr;

void notifyEffectsWindowTrackListChanged();
void notifyEffectsWindowActiveTrackChanged(int trackId);
void notifyEffectsWindowTrackValuesChanged(int trackId);
void notifySidechainWindowTrackChanged(int trackId);
void notifySidechainWindowValuesChanged(int trackId);
void notifySidechainWindowTrackListChanged();
void notifyEqWindowTrackChanged(int trackId);
void notifyEqWindowValuesChanged(int trackId);
void notifyDelayWindowTrackChanged(int trackId);
void notifyDelayWindowValuesChanged(int trackId);
void openSidechainWindow(HWND parent, int trackId);
void openEqWindow(HWND parent, int trackId);
void openDelayWindow(HWND parent, int trackId);

void updateViewMenuChecks()
{
    if (!gViewMenu)
        return;

    UINT pianoState = (gPianoRollWindow && IsWindow(gPianoRollWindow)) ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(gViewMenu, kMenuCommandTogglePianoRoll, MF_BYCOMMAND | pianoState);

    UINT effectsState = (gEffectsWindow && IsWindow(gEffectsWindow)) ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(gViewMenu, kMenuCommandToggleEffects, MF_BYCOMMAND | effectsState);

    UINT waveformState = isWaveformWindowOpen() ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(gViewMenu, kMenuCommandToggleWaveform, MF_BYCOMMAND | waveformState);
}

constexpr UINT kMenuCommandLoadProject = 1001;
constexpr UINT kMenuCommandSaveProject = 1002;

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

struct PianoRollParamDragState
{
    bool active = false;
    int parameterIndex = 0;
    int trackId = 0;
    int lastStepIndex = -1;
    RECT laneRect{};
    std::array<LONG, kSequencerStepsPerPage + 1> columnX{};
};

PianoRollParamDragState gPianoRollParamDrag;

constexpr UINT kPianoRollContextDeleteNoteId = 5001;
constexpr UINT kPianoRollContextDeleteRangeId = 5002;

constexpr wchar_t kPianoRollWindowClassName[] = L"KJPianoRollWindow";
constexpr int kPianoRollWindowWidth = 640;
constexpr int kPianoRollWindowHeight = 360;
constexpr int kPianoRollMargin = 10;
constexpr int kPianoRollKeyboardWidth = 80;
constexpr int kPianoRollMenuSpacing = 8;
constexpr int kPianoRollMenuAreaHeight = 140;
constexpr int kPianoRollTabBarHeight = 28;
constexpr int kPianoRollNoteRows = 24;
constexpr int kPianoRollLowestNote = 48; // C3
constexpr int kPianoRollHighestNote = kPianoRollLowestNote + kPianoRollNoteRows - 1;
constexpr COLORREF kPianoRollGridBackground = RGB(30, 30, 30);
constexpr COLORREF kPianoRollGridLine = RGB(60, 60, 60);
constexpr COLORREF kPianoRollActiveNote = RGB(0, 140, 220);
constexpr COLORREF kPianoRollPlayingColumn = RGB(50, 50, 50);
constexpr COLORREF kPianoRollKeyboardLight = RGB(80, 80, 80);
constexpr COLORREF kPianoRollKeyboardDark = RGB(45, 45, 45);
constexpr COLORREF kPianoRollMenuBackground = RGB(26, 26, 26);
constexpr COLORREF kPianoRollMenuContentBackground = RGB(32, 32, 32);
constexpr COLORREF kPianoRollMenuTabActive = RGB(65, 90, 130);
constexpr COLORREF kPianoRollMenuTabInactive = RGB(45, 45, 45);

constexpr int kPianoRollMenuTabCount = 3;
constexpr int kPianoRollCollapseBarHeight = 28;
constexpr int kPianoRollCollapseButtonSize = 20;
constexpr int kPianoRollCollapseButtonPadding = 8;
const std::array<const wchar_t*, kPianoRollMenuTabCount> kPianoRollMenuTabLabels = {
    L"Velocity", L"Pitch", L"Effect"
};

constexpr wchar_t kEffectsWindowClassName[] = L"KJEffectsWindow";
constexpr int kEffectsWindowWidth = 520;
constexpr int kEffectsWindowHeight = 360;
constexpr wchar_t kSidechainWindowClassName[] = L"KJSidechainWindow";
constexpr int kSidechainWindowWidth = 420;
constexpr int kSidechainWindowHeight = 320;

constexpr UINT WM_EFFECTS_REFRESH_VALUES = WM_APP + 1;
constexpr UINT WM_EFFECTS_RELOAD_TRACKS = WM_APP + 2;
constexpr UINT WM_EFFECTS_SELECT_TRACK = WM_APP + 3;
constexpr UINT WM_EQ_REFRESH_VALUES = WM_APP + 10;
constexpr UINT WM_EQ_SET_TRACK = WM_APP + 11;
constexpr UINT WM_DELAY_REFRESH_VALUES = WM_APP + 20;
constexpr UINT WM_DELAY_SET_TRACK = WM_APP + 21;
constexpr UINT WM_SIDECHAIN_REFRESH_VALUES = WM_APP + 30;
constexpr UINT WM_SIDECHAIN_SET_TRACK = WM_APP + 31;
constexpr UINT WM_SIDECHAIN_RELOAD_TRACKS = WM_APP + 32;

struct PianoRollLayout
{
    RECT client{};
    RECT grid{};
    RECT keyboard{};
    RECT menuArea{};
    RECT menuTabBar{};
    RECT menuContent{};
    RECT collapseBar{};
    RECT collapseButton{};
    std::array<int, kSequencerStepsPerPage + 1> columnX{};
    std::array<int, kPianoRollNoteRows + 1> rowY{};
    std::array<RECT, kPianoRollMenuTabCount> tabRects{};
    bool menuCollapsed = false;
};

PianoRollLayout computePianoRollLayout(const RECT& client)
{
    PianoRollLayout layout{};
    layout.client = client;
    layout.menuCollapsed = gPianoRollMenuCollapsed;

    LONG clientLeft = client.left;
    LONG clientTop = client.top;
    LONG clientRight = client.right;
    LONG clientBottom = client.bottom;

    if (clientRight < clientLeft)
        std::swap(clientRight, clientLeft);
    if (clientBottom < clientTop)
        std::swap(clientBottom, clientTop);

    LONG innerLeft = clientLeft + kPianoRollMargin;
    LONG innerTop = clientTop + kPianoRollMargin;
    LONG innerRight = std::max(innerLeft, clientRight - kPianoRollMargin);
    LONG innerBottom = std::max(innerTop, clientBottom - kPianoRollMargin);

    LONG collapseBarTop = std::max(innerTop, innerBottom - kPianoRollCollapseBarHeight);
    layout.collapseBar.left = innerLeft;
    layout.collapseBar.right = innerRight;
    layout.collapseBar.bottom = innerBottom;
    layout.collapseBar.top = collapseBarTop;

    LONG menuHeight = layout.menuCollapsed ? 0 : std::min<LONG>(kPianoRollMenuAreaHeight, innerBottom - innerTop);
    if (menuHeight > 0)
    {
        layout.menuArea.left = innerLeft;
        layout.menuArea.right = innerRight;
        layout.menuArea.bottom = innerBottom;
        layout.menuArea.top = innerBottom - menuHeight;

        layout.menuTabBar = layout.menuArea;
        layout.menuTabBar.bottom = std::min(layout.menuArea.bottom, layout.menuArea.top + kPianoRollTabBarHeight);

        layout.menuContent = layout.menuArea;
        layout.menuContent.top = layout.menuTabBar.bottom;

        layout.collapseBar = layout.menuTabBar;
    }
    else
    {
        layout.menuArea = RECT{innerLeft, innerBottom, innerLeft, innerBottom};
        layout.menuTabBar = layout.menuArea;
        layout.menuContent = layout.menuArea;
    }

    LONG gridBottom = menuHeight > 0 ? layout.menuArea.top - kPianoRollMenuSpacing : layout.collapseBar.top - kPianoRollMenuSpacing;
    if (gridBottom < innerTop)
        gridBottom = innerTop;

    LONG keyboardRight = std::min(innerRight, innerLeft + kPianoRollKeyboardWidth);

    layout.grid.left = keyboardRight;
    layout.grid.top = innerTop;
    layout.grid.right = innerRight;
    layout.grid.bottom = gridBottom;

    layout.keyboard.left = innerLeft;
    layout.keyboard.top = innerTop;
    layout.keyboard.right = keyboardRight;
    layout.keyboard.bottom = gridBottom;

    if (layout.grid.bottom < layout.grid.top)
        layout.grid.bottom = layout.grid.top;
    if (layout.grid.right < layout.grid.left)
        layout.grid.right = layout.grid.left;
    if (layout.keyboard.bottom < layout.keyboard.top)
        layout.keyboard.bottom = layout.keyboard.top;
    if (layout.keyboard.right < layout.keyboard.left)
        layout.keyboard.right = layout.keyboard.left;

    if (layout.menuTabBar.right < layout.menuTabBar.left)
        layout.menuTabBar.right = layout.menuTabBar.left;
    if (layout.menuTabBar.bottom < layout.menuTabBar.top)
        layout.menuTabBar.bottom = layout.menuTabBar.top;
    if (layout.menuContent.right < layout.menuContent.left)
        layout.menuContent.right = layout.menuContent.left;
    if (layout.menuContent.bottom < layout.menuContent.top)
        layout.menuContent.bottom = layout.menuContent.top;

    LONG tabRightLimit = layout.menuTabBar.right;
    if (!layout.menuCollapsed)
    {
        LONG reserved = kPianoRollCollapseButtonSize + 2 * kPianoRollCollapseButtonPadding;
        if (tabRightLimit - reserved > layout.menuTabBar.left)
            tabRightLimit -= reserved;
        else
            tabRightLimit = layout.menuTabBar.left;
    }

    LONG tabWidth = (layout.menuTabBar.bottom > layout.menuTabBar.top)
                        ? std::max<LONG>(0, tabRightLimit - layout.menuTabBar.left)
                        : 0;
    int baseTabWidth = kPianoRollMenuTabCount > 0 ? tabWidth / kPianoRollMenuTabCount : 0;
    int tabRemainder = kPianoRollMenuTabCount > 0 ? tabWidth % kPianoRollMenuTabCount : 0;
    LONG tabX = layout.menuTabBar.left;
    for (int i = 0; i < kPianoRollMenuTabCount; ++i)
    {
        LONG increment = baseTabWidth + (i < tabRemainder ? 1 : 0);
        LONG nextTabX = tabX + increment;
        if (nextTabX > tabRightLimit)
            nextTabX = tabRightLimit;
        layout.tabRects[static_cast<size_t>(i)] = RECT{tabX, layout.menuTabBar.top, nextTabX, layout.menuTabBar.bottom};
        tabX = nextTabX;
    }

    LONG buttonRight = layout.collapseBar.right - kPianoRollCollapseButtonPadding;
    LONG buttonLeft = std::max(layout.collapseBar.left + kPianoRollCollapseButtonPadding,
                               buttonRight - kPianoRollCollapseButtonSize);
    if (buttonLeft > buttonRight)
        buttonLeft = buttonRight;
    LONG buttonHeight = std::min<LONG>(kPianoRollCollapseButtonSize, layout.collapseBar.bottom - layout.collapseBar.top);
    LONG buttonTop = layout.collapseBar.top + ((layout.collapseBar.bottom - layout.collapseBar.top - buttonHeight) / 2);
    if (buttonTop < layout.collapseBar.top)
        buttonTop = layout.collapseBar.top;
    LONG buttonBottom = buttonTop + buttonHeight;
    if (buttonBottom > layout.collapseBar.bottom)
        buttonBottom = layout.collapseBar.bottom;
    layout.collapseButton = RECT{buttonLeft, buttonTop, buttonRight, buttonBottom};

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

std::vector<int> getStepNotesForDisplay(int trackId, int stepIndex)
{
    std::vector<int> notes = trackGetStepNotes(trackId, stepIndex);
    if (notes.empty() && getTrackStepState(trackId, stepIndex))
    {
        int fallback = trackGetStepNote(trackId, stepIndex);
        if (fallback >= 0)
            notes.push_back(fallback);
    }
    return notes;
}

bool stepHasDisplayableNote(int trackId, int stepIndex)
{
    auto notes = getStepNotesForDisplay(trackId, stepIndex);
    return !notes.empty();
}

COLORREF scaleColor(COLORREF color, double factor)
{
    factor = std::clamp(factor, 0.25, 1.5);
    auto clampChannel = [](int value) {
        return static_cast<BYTE>(std::clamp(value, 0, 255));
    };

    int red = static_cast<int>(std::lround(static_cast<double>(GetRValue(color)) * factor));
    int green = static_cast<int>(std::lround(static_cast<double>(GetGValue(color)) * factor));
    int blue = static_cast<int>(std::lround(static_cast<double>(GetBValue(color)) * factor));

    return RGB(clampChannel(red), clampChannel(green), clampChannel(blue));
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

void pianoRollInvalidateAfterEdit()
{
    invalidatePianoRollWindow();
    if (gMainWindow && IsWindow(gMainWindow))
    {
        InvalidateRect(gMainWindow, nullptr, FALSE);
    }
}

void pianoRollDeleteNoteRange(int trackId, int stepIndex, int midiNote)
{
    if (trackId <= 0 || stepIndex < 0)
        return;

    int totalSteps = getSequencerStepCount(trackId);
    if (totalSteps <= 0)
        return;

    std::vector<int> steps;
    for (int step = stepIndex; step >= 0; --step)
    {
        if (stepContainsMidiNote(trackId, step, midiNote))
        {
            steps.push_back(step);
        }
        else
        {
            break;
        }
    }

    std::reverse(steps.begin(), steps.end());

    for (int step = stepIndex + 1; step < totalSteps; ++step)
    {
        if (stepContainsMidiNote(trackId, step, midiNote))
        {
            steps.push_back(step);
        }
        else
        {
            break;
        }
    }

    for (int step : steps)
    {
        if (stepContainsMidiNote(trackId, step, midiNote))
        {
            trackToggleStepNote(trackId, step, midiNote);
        }
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

RECT computePianoRollMenuLaneRect(const PianoRollLayout& layout)
{
    RECT laneRect {0, 0, 0, 0};
    if (layout.menuCollapsed)
        return laneRect;

    if (layout.menuContent.right <= layout.menuContent.left || layout.menuContent.bottom <= layout.menuContent.top)
        return laneRect;

    LONG top = layout.menuContent.top + 8; // heading top offset
    top += 24; // heading height
    top += 4;  // spacing to description
    top += 40; // description height
    top += 6;  // spacing to lane

    laneRect.left = layout.menuContent.left + 12;
    laneRect.right = layout.menuContent.right - 12;
    laneRect.top = top;
    laneRect.bottom = layout.menuContent.bottom - 8;

    if (laneRect.right < laneRect.left)
        laneRect.right = laneRect.left;
    if (laneRect.bottom < laneRect.top)
        laneRect.bottom = laneRect.top;

    return laneRect;
}

void computePianoRollLaneColumns(const RECT& laneRect, std::array<LONG, kSequencerStepsPerPage + 1>& columns)
{
    LONG left = laneRect.left;
    LONG right = laneRect.right;
    if (right < left)
        right = left;

    int stepCount = kSequencerStepsPerPage;
    int totalWidth = static_cast<int>(std::max<LONG>(0, right - left));
    int baseWidth = stepCount > 0 ? totalWidth / stepCount : 0;
    int remainder = stepCount > 0 ? totalWidth % stepCount : 0;
    LONG x = left;
    for (int i = 0; i < stepCount; ++i)
    {
        columns[static_cast<size_t>(i)] = x;
        int increment = baseWidth + (i < remainder ? 1 : 0);
        LONG nextX = x + increment;
        if (i == stepCount - 1)
            nextX = right;
        if (nextX < x)
            nextX = x;
        x = nextX;
    }
    columns[static_cast<size_t>(stepCount)] = right;
}

int pianoRollLaneColumnFromX(const std::array<LONG, kSequencerStepsPerPage + 1>& columns, int x)
{
    for (int i = 0; i < kSequencerStepsPerPage; ++i)
    {
        LONG left = columns[static_cast<size_t>(i)];
        LONG right = columns[static_cast<size_t>(i + 1)];
        if (right < left)
            right = left;
        if (x >= left && x < right)
            return i;
    }
    return (x >= columns.back()) ? (kSequencerStepsPerPage - 1) : 0;
}

void pianoRollResetParamDrag()
{
    gPianoRollParamDrag.active = false;
    gPianoRollParamDrag.parameterIndex = 0;
    gPianoRollParamDrag.trackId = 0;
    gPianoRollParamDrag.lastStepIndex = -1;
    gPianoRollParamDrag.laneRect = RECT{0, 0, 0, 0};
    gPianoRollParamDrag.columnX.fill(0);
}

void pianoRollApplyMenuParameter(int parameterIndex,
                                 int trackId,
                                 int stepIndex,
                                 int pointerX,
                                 int pointerY,
                                 const RECT& laneRect,
                                 LONG columnLeft,
                                 LONG columnRight)
{
    if (trackId <= 0 || stepIndex < 0)
        return;

    LONG innerLeft = columnLeft + 2;
    LONG innerRight = columnRight - 2;
    LONG innerTop = laneRect.top + 2;
    LONG innerBottom = laneRect.bottom - 2;
    if (innerRight <= innerLeft || innerBottom <= innerTop)
        return;

    switch (parameterIndex)
    {
    case 0:
    {
        if (!stepHasDisplayableNote(trackId, stepIndex))
            break;

        LONG clampedY = std::clamp(pointerY, static_cast<int>(innerTop), static_cast<int>(innerBottom));
        int height = static_cast<int>(innerBottom - innerTop);
        float normalized = (height > 0)
                               ? 1.0f - static_cast<float>(clampedY - innerTop) / static_cast<float>(height)
                               : kTrackStepVelocityMax;
        normalized = std::clamp(normalized, kTrackStepVelocityMin, kTrackStepVelocityMax);

        auto notes = getStepNotesForDisplay(trackId, stepIndex);
        if (notes.empty())
            break;

        if (notes.size() == 1)
        {
            trackSetStepVelocity(trackId, stepIndex, normalized);
            break;
        }

        LONG clampedX = std::clamp(pointerX, static_cast<int>(innerLeft), static_cast<int>(innerRight - 1));
        LONG innerWidth = innerRight - innerLeft;
        if (innerWidth <= 0)
            break;

        double normalizedX = static_cast<double>(clampedX - innerLeft) /
                             static_cast<double>(std::max<LONG>(1, innerWidth));
        int targetIndex = static_cast<int>(std::floor(normalizedX * static_cast<double>(notes.size())));
        if (targetIndex < 0)
            targetIndex = 0;
        if (targetIndex >= static_cast<int>(notes.size()))
            targetIndex = static_cast<int>(notes.size()) - 1;
        int targetMidiNote = notes[static_cast<size_t>(targetIndex)];
        trackSetStepNoteVelocity(trackId, stepIndex, targetMidiNote, normalized);
        break;
    }
    case 1:
    {
        LONG clampedY = std::clamp(pointerY, static_cast<int>(innerTop), static_cast<int>(innerBottom));
        int height = static_cast<int>(innerBottom - innerTop);
        float normalized = (height > 0)
                               ? 1.0f - static_cast<float>(clampedY - innerTop) / static_cast<float>(height)
                               : 0.5f;
        double maxAbs = std::max(std::abs(static_cast<double>(kTrackStepPitchMin)),
                                 std::abs(static_cast<double>(kTrackStepPitchMax)));
        double pitch = (normalized - 0.5) * 2.0 * maxAbs;
        pitch = std::clamp(pitch, static_cast<double>(kTrackStepPitchMin), static_cast<double>(kTrackStepPitchMax));
        trackSetStepPitchOffset(trackId, stepIndex, static_cast<float>(pitch));
        break;
    }
    default:
        break;
    }
}

} // namespace

void requestMainMenuRefresh()
{
    // TODO: refresh main menu or trigger redraw
    updateViewMenuChecks();
    if (gMainWindow && IsWindow(gMainWindow))
    {
        DrawMenuBar(gMainWindow);
    }
}

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

struct TrackTypeDropdownOption
{
    RECT rect{};
    int trackId = 0;
    TrackType type = TrackType::Synth;
    bool isSelected = false;
};

std::vector<TrackTypeDropdownOption> gTrackTypeDropdownOptions;

constexpr float kMixerVolumeMin = 0.0f;
constexpr float kMixerVolumeMax = 1.0f;
constexpr float kMixerPanMin = -1.0f;
constexpr float kMixerPanMax = 1.0f;
constexpr float kMixerEqMin = -12.0f;
constexpr float kMixerEqMax = 12.0f;
constexpr float kMixerDelayTimeMin = 10.0f;
constexpr float kMixerDelayTimeMax = 2000.0f;
constexpr float kMixerDelayFeedbackMin = 0.0f;
constexpr float kMixerDelayFeedbackMax = 0.95f;
constexpr float kMixerDelayMixMin = 0.0f;
constexpr float kMixerDelayMixMax = 1.0f;
constexpr float kSynthFormantMin = 0.0f;
constexpr float kSynthFormantMax = 1.0f;
constexpr float kSynthFeedbackMin = 0.0f;
constexpr float kSynthFeedbackMax = 1.0f;
constexpr float kSynthPitchMin = -24.0f;
constexpr float kSynthPitchMax = 24.0f;
constexpr float kSynthPitchRangeMin = 1.0f;
constexpr float kSynthPitchRangeMax = 24.0f;
constexpr float kSynthAttackMin = 0.0f;
constexpr float kSynthAttackMax = 4.0f;
constexpr float kSynthDecayMin = 0.0f;
constexpr float kSynthDecayMax = 4.0f;
constexpr float kSynthSustainMin = 0.0f;
constexpr float kSynthSustainMax = 1.0f;
constexpr float kSynthReleaseMin = 0.0f;
constexpr float kSynthReleaseMax = 4.0f;
constexpr float kSampleAttackMin = 0.0f;
constexpr float kSampleAttackMax = 4.0f;
constexpr float kSampleReleaseMin = 0.0f;
constexpr float kSampleReleaseMax = 4.0f;

struct SliderControlRects
{
    RECT control{};
    RECT track{};
};

SliderControlRects gSynthFormantSliderControl{};
SliderControlRects gSynthFeedbackSliderControl{};
SliderControlRects gSynthPitchSliderControl{};
SliderControlRects gSynthPitchRangeSliderControl{};
SliderControlRects gSynthAttackSliderControl{};
SliderControlRects gSynthDecaySliderControl{};
SliderControlRects gSynthSustainSliderControl{};
SliderControlRects gSynthReleaseSliderControl{};
SliderControlRects gSampleAttackSliderControl{};
SliderControlRects gSampleReleaseSliderControl{};

enum class SliderDragTarget
{
    None,
    SynthFormant,
    SynthFeedback,
    SynthPitch,
    SynthPitchRange,
    SynthAttack,
    SynthDecay,
    SynthSustain,
    SynthRelease,
    SampleAttack,
    SampleRelease,
};

struct SliderDragState
{
    SliderDragTarget target = SliderDragTarget::None;
    int trackId = 0;
};

SliderDragState gSliderDrag{};

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

    effectsToggleButton = pianoRollToggleButton;
    effectsToggleButton.left = pianoRollToggleButton.right + 12;
    effectsToggleButton.right = effectsToggleButton.left + (pianoRollToggleButton.right - pianoRollToggleButton.left);
    if (effectsToggleButton.right <= effectsToggleButton.left)
    {
        effectsToggleButton.right = effectsToggleButton.left + 120;
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

template <typename Options, typename Predicate>
bool computeDropdownBoundsIf(const Options& options, RECT& bounds, Predicate predicate)
{
    bool hasBounds = false;
    for (const auto& option : options)
    {
        if (!predicate(option))
            continue;

        if (!hasBounds)
        {
            bounds = option.rect;
            hasBounds = true;
        }
        else
        {
            bounds.left = std::min(bounds.left, option.rect.left);
            bounds.top = std::min(bounds.top, option.rect.top);
            bounds.right = std::max(bounds.right, option.rect.right);
            bounds.bottom = std::max(bounds.bottom, option.rect.bottom);
        }
    }
    return hasBounds;
}

template <typename Options>
bool computeDropdownBounds(const Options& options, RECT& bounds)
{
    return computeDropdownBoundsIf(options, bounds, [](const auto&) { return true; });
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
        requestMainMenuRefresh();
    }
}

void showLoadProjectDialog(HWND hwnd)
{
    wchar_t fileBuffer[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"KJ Project Files (*.jik)\0*.jik\0All Files\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"jik";

    if (GetOpenFileNameW(&ofn))
    {
        std::filesystem::path selectedPath(fileBuffer);
        if (!loadProjectFromFile(selectedPath))
        {
            MessageBoxW(hwnd,
                        L"Failed to load project.",
                        L"Load Project",
                        MB_OK | MB_ICONERROR);
            return;
        }

        auto tracks = getTracks();
        if (!tracks.empty())
        {
            selectedTrackId = tracks.front().id;
            setActiveSequencerTrackId(selectedTrackId);
        }
        else
        {
            selectedTrackId = 0;
            setActiveSequencerTrackId(0);
        }

        currentStepPage = 0;
        openTrackTypeTrackId = 0;
        waveDropdownOpen = false;
        waveDropdownTrackId = 0;
        audioDeviceDropdownOpen = false;

        notifyEffectsWindowTrackListChanged();
        if (selectedTrackId > 0)
        {
            notifyEffectsWindowActiveTrackChanged(selectedTrackId);
            notifyEffectsWindowTrackValuesChanged(selectedTrackId);
        }

        invalidatePianoRollWindow();
        if (hwnd && IsWindow(hwnd))
        {
            InvalidateRect(hwnd, nullptr, FALSE);
        }
        if (gMainWindow && IsWindow(gMainWindow) && hwnd != gMainWindow)
        {
            InvalidateRect(gMainWindow, nullptr, FALSE);
        }

        MessageBoxW(hwnd,
                    L"Project loaded successfully.",
                    L"Load Project",
                    MB_OK | MB_ICONINFORMATION);
    }
}

void showSaveProjectDialog(HWND hwnd)
{
    wchar_t fileBuffer[MAX_PATH] = {0};
    OPENFILENAMEW ofn = {0};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = hwnd;
    ofn.lpstrFilter = L"KJ Project Files (*.jik)\0*.jik\0All Files\0*.*\0";
    ofn.lpstrFile = fileBuffer;
    ofn.nMaxFile = MAX_PATH;
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_PATHMUSTEXIST;
    ofn.lpstrDefExt = L"jik";

    if (GetSaveFileNameW(&ofn))
    {
        std::filesystem::path selectedPath(fileBuffer);
        if (!saveProjectToFile(selectedPath))
        {
            MessageBoxW(hwnd,
                        L"Failed to save project.",
                        L"Save Project",
                        MB_OK | MB_ICONERROR);
        }
        else
        {
            MessageBoxW(hwnd,
                        L"Project saved successfully.",
                        L"Save Project",
                        MB_OK | MB_ICONINFORMATION);
        }
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
        pianoRollResetParamDrag();
        if (hwnd == gPianoRollWindow)
        {
            gPianoRollWindow = nullptr;
            if (gMainWindow)
            {
                InvalidateRect(gMainWindow, nullptr, FALSE);
            }
        }
        requestMainMenuRefresh();
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

        if (x >= layout.collapseButton.left && x < layout.collapseButton.right && y >= layout.collapseButton.top &&
            y < layout.collapseButton.bottom)
        {
            gPianoRollMenuCollapsed = !gPianoRollMenuCollapsed;
            pianoRollResetParamDrag();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (layout.menuCollapsed && x >= layout.collapseBar.left && x < layout.collapseBar.right &&
            y >= layout.collapseBar.top && y < layout.collapseBar.bottom)
        {
            gPianoRollMenuCollapsed = false;
            pianoRollResetParamDrag();
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (x >= layout.menuTabBar.left && x < layout.menuTabBar.right && y >= layout.menuTabBar.top &&
            y < layout.menuTabBar.bottom)
        {
            for (int i = 0; i < kPianoRollMenuTabCount; ++i)
            {
                const RECT& tabRect = layout.tabRects[static_cast<size_t>(i)];
                if (x >= tabRect.left && x < tabRect.right)
                {
                    if (gPianoRollSelectedMenuTab != i)
                    {
                        gPianoRollSelectedMenuTab = i;
                        InvalidateRect(hwnd, nullptr, FALSE);
                    }
                    break;
                }
            }
            return 0;
        }

        if (!layout.menuCollapsed && gPianoRollSelectedMenuTab < 2)
        {
            RECT laneRect = computePianoRollMenuLaneRect(layout);
            if (laneRect.right > laneRect.left && laneRect.bottom > laneRect.top && x >= laneRect.left && x < laneRect.right &&
                y >= laneRect.top && y < laneRect.bottom)
            {
                int trackId = getActiveSequencerTrackId();
                if (trackId > 0)
                {
                    std::array<LONG, kSequencerStepsPerPage + 1> laneColumns {};
                    computePianoRollLaneColumns(laneRect, laneColumns);
                    int column = pianoRollLaneColumnFromX(laneColumns, x);
                    if (column >= 0)
                    {
                        int totalSteps = getSequencerStepCount(trackId);
                        int stepIndex = currentStepPage * kSequencerStepsPerPage + column;
                        if (stepIndex >= 0 && stepIndex < totalSteps)
                        {
                            bool allowParameterDrag = (gPianoRollSelectedMenuTab != 0) ||
                                                       stepHasDisplayableNote(trackId, stepIndex);
                            if (!allowParameterDrag)
                                return 0;

                            gPianoRollParamDrag.active = true;
                            gPianoRollParamDrag.parameterIndex = gPianoRollSelectedMenuTab;
                            gPianoRollParamDrag.trackId = trackId;
                            gPianoRollParamDrag.lastStepIndex = stepIndex;
                            gPianoRollParamDrag.laneRect = laneRect;
                            gPianoRollParamDrag.columnX = laneColumns;
                            if (GetCapture() != hwnd)
                                SetCapture(hwnd);
                            pianoRollApplyMenuParameter(gPianoRollParamDrag.parameterIndex,
                                                        trackId,
                                                        stepIndex,
                                                        x,
                                                        y,
                                                        laneRect,
                                                        laneColumns[static_cast<size_t>(column)],
                                                        laneColumns[static_cast<size_t>(column + 1)]);
                            pianoRollInvalidateAfterEdit();
                        }
                    }
                }
                return 0;
            }
        }

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
    case WM_RBUTTONUP:
    {
        RECT client;
        GetClientRect(hwnd, &client);
        PianoRollLayout layout = computePianoRollLayout(client);

        int x = GET_X_LPARAM(lParam);
        int y = GET_Y_LPARAM(lParam);

        if (x < layout.grid.left || x >= layout.grid.right || y < layout.grid.top || y >= layout.grid.bottom)
            return 0;

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

        if (column < 0 || row < 0)
            return 0;

        int trackId = getActiveSequencerTrackId();
        if (trackId <= 0)
            return 0;

        int actualStepCount = getSequencerStepCount(trackId);
        int totalSteps = actualStepCount < 1 ? kSequencerStepsPerPage : actualStepCount;

        int stepIndex = currentStepPage * kSequencerStepsPerPage + column;
        if (stepIndex >= totalSteps)
            return 0;

        int midiNote = kPianoRollHighestNote - row;
        if (!stepContainsMidiNote(trackId, stepIndex, midiNote))
            return 0;

        int contiguousCount = 1;
        for (int step = stepIndex - 1; step >= 0; --step)
        {
            if (stepContainsMidiNote(trackId, step, midiNote))
                ++contiguousCount;
            else
                break;
        }
        for (int step = stepIndex + 1; step < actualStepCount; ++step)
        {
            if (stepContainsMidiNote(trackId, step, midiNote))
                ++contiguousCount;
            else
                break;
        }

        HMENU menu = CreatePopupMenu();
        if (!menu)
            return 0;

        AppendMenuW(menu, MF_STRING, kPianoRollContextDeleteNoteId, L"Delete Note");
        if (contiguousCount > 1)
        {
            AppendMenuW(menu, MF_STRING, kPianoRollContextDeleteRangeId, L"Delete Note Range");
        }

        POINT screenPoint {x, y};
        ClientToScreen(hwnd, &screenPoint);
        SetForegroundWindow(hwnd);
        UINT command = TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_RETURNCMD, screenPoint.x, screenPoint.y, 0, hwnd, nullptr);
        DestroyMenu(menu);

        switch (command)
        {
        case kPianoRollContextDeleteNoteId:
            if (stepContainsMidiNote(trackId, stepIndex, midiNote))
            {
                trackToggleStepNote(trackId, stepIndex, midiNote);
                pianoRollInvalidateAfterEdit();
            }
            break;
        case kPianoRollContextDeleteRangeId:
            pianoRollDeleteNoteRange(trackId, stepIndex, midiNote);
            pianoRollInvalidateAfterEdit();
            break;
        default:
            break;
        }
        return 0;
    }
    case WM_MOUSEMOVE:
    {
        if (gPianoRollParamDrag.active)
        {
            RECT laneRect = gPianoRollParamDrag.laneRect;
            if (laneRect.right > laneRect.left && laneRect.bottom > laneRect.top)
            {
                int pointerX = GET_X_LPARAM(lParam);
                int pointerY = GET_Y_LPARAM(lParam);
                if (pointerX < laneRect.left)
                    pointerX = laneRect.left;
                if (pointerX >= laneRect.right)
                    pointerX = laneRect.right - 1;
                if (pointerY < laneRect.top)
                    pointerY = laneRect.top;
                if (pointerY >= laneRect.bottom)
                    pointerY = laneRect.bottom - 1;

                int column = pianoRollLaneColumnFromX(gPianoRollParamDrag.columnX, pointerX);
                if (column < 0)
                    column = 0;
                if (column >= kSequencerStepsPerPage)
                    column = kSequencerStepsPerPage - 1;

                int trackId = gPianoRollParamDrag.trackId;
                if (trackId > 0)
                {
                    int totalSteps = getSequencerStepCount(trackId);
                    int stepIndex = currentStepPage * kSequencerStepsPerPage + column;
                    if (stepIndex >= 0 && stepIndex < totalSteps)
                    {
                        gPianoRollParamDrag.lastStepIndex = stepIndex;
                        pianoRollApplyMenuParameter(gPianoRollParamDrag.parameterIndex,
                                                    trackId,
                                                    stepIndex,
                                                    pointerX,
                                                    pointerY,
                                                    laneRect,
                                                    gPianoRollParamDrag.columnX[static_cast<size_t>(column)],
                                                    gPianoRollParamDrag.columnX[static_cast<size_t>(column + 1)]);
                        pianoRollInvalidateAfterEdit();
                    }
                }
            }
        }

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
        if (gPianoRollParamDrag.active)
        {
            if (GetCapture() == hwnd)
                ReleaseCapture();
            pianoRollResetParamDrag();
            invalidatePianoRollWindow();
            if (gMainWindow && IsWindow(gMainWindow))
            {
                InvalidateRect(gMainWindow, nullptr, FALSE);
            }
        }

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

                columnNotes[column] = getStepNotesForDisplay(trackId, stepIndex);
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

        if (layout.menuArea.right > layout.menuArea.left && layout.menuArea.bottom > layout.menuArea.top)
        {
            HBRUSH menuBackground = CreateSolidBrush(kPianoRollMenuBackground);
            FillRect(hdc, &layout.menuArea, menuBackground);
            DeleteObject(menuBackground);

            if (layout.menuContent.right > layout.menuContent.left && layout.menuContent.bottom > layout.menuContent.top)
            {
                HBRUSH contentBackground = CreateSolidBrush(kPianoRollMenuContentBackground);
                FillRect(hdc, &layout.menuContent, contentBackground);
                DeleteObject(contentBackground);
            }

            if (layout.menuTabBar.right > layout.menuTabBar.left && layout.menuTabBar.bottom > layout.menuTabBar.top)
            {
                HBRUSH tabBarBackground = CreateSolidBrush(kPianoRollMenuBackground);
                FillRect(hdc, &layout.menuTabBar, tabBarBackground);
                DeleteObject(tabBarBackground);

                HBRUSH activeTabBrush = CreateSolidBrush(kPianoRollMenuTabActive);
                HBRUSH inactiveTabBrush = CreateSolidBrush(kPianoRollMenuTabInactive);
                for (int i = 0; i < kPianoRollMenuTabCount; ++i)
                {
                    const RECT& tabRect = layout.tabRects[static_cast<size_t>(i)];
                    if (tabRect.right <= tabRect.left || tabRect.bottom <= tabRect.top)
                        continue;

                    HBRUSH brush = (gPianoRollSelectedMenuTab == i) ? activeTabBrush : inactiveTabBrush;
                    FillRect(hdc, &tabRect, brush);

                    RECT textRect = tabRect;
                    textRect.left += 10;
                    textRect.right -= 10;
                    textRect.top += 6;
                    DrawTextW(hdc, kPianoRollMenuTabLabels[static_cast<size_t>(i)], -1, &textRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
                DeleteObject(activeTabBrush);
                DeleteObject(inactiveTabBrush);
            }

            RECT headingRect = layout.menuContent;
            headingRect.left += 12;
            headingRect.right -= 12;
            headingRect.top += 10;
            headingRect.bottom = headingRect.top + 24;

            if (headingRect.right > headingRect.left)
            {
                std::wstring heading = std::wstring(kPianoRollMenuTabLabels[static_cast<size_t>(gPianoRollSelectedMenuTab)]) +
                                       L" Controls";
                DrawTextW(hdc, heading.c_str(), -1, &headingRect, DT_LEFT | DT_TOP | DT_SINGLELINE | DT_END_ELLIPSIS);
            }

            RECT descriptionRect = headingRect;
            descriptionRect.top = headingRect.bottom + 4;
            descriptionRect.bottom = descriptionRect.top + 48;

            if (descriptionRect.right > descriptionRect.left)
            {
                std::wstring description;
                switch (gPianoRollSelectedMenuTab)
                {
                case 0:
                    description = L"Shape the intensity of each note. Drag a step to set its velocity envelope.";
                    break;
                case 1:
                    description = L"Fine-tune note pitch in semitones or cents for expressive runs.";
                    break;
                case 2:
                default:
                    description = L"Route per-note modulation and effects such as filters or delays.";
                    break;
                }
                DrawTextW(hdc, description.c_str(), -1, &descriptionRect, DT_LEFT | DT_TOP | DT_WORDBREAK);
            }

            RECT laneRect = computePianoRollMenuLaneRect(layout);
            if (laneRect.right > laneRect.left && laneRect.bottom > laneRect.top)
            {
                HBRUSH laneBackground = CreateSolidBrush(RGB(40, 40, 40));
                FillRect(hdc, &laneRect, laneBackground);
                DeleteObject(laneBackground);

                std::array<LONG, kSequencerStepsPerPage + 1> laneColumns {};
                computePianoRollLaneColumns(laneRect, laneColumns);

                HPEN lanePen = CreatePen(PS_SOLID, 1, RGB(55, 55, 55));
                HPEN oldLanePen = static_cast<HPEN>(SelectObject(hdc, lanePen));
                for (int i = 0; i <= kSequencerStepsPerPage; ++i)
                {
                    LONG lineX = laneColumns[static_cast<size_t>(i)];
                    MoveToEx(hdc, lineX, laneRect.top, nullptr);
                    LineTo(hdc, lineX, laneRect.bottom);
                }
                SelectObject(hdc, oldLanePen);
                DeleteObject(lanePen);

                if (gPianoRollSelectedMenuTab == 1)
                {
                    HPEN centerPen = CreatePen(PS_SOLID, 1, RGB(75, 75, 75));
                    HPEN oldPen = static_cast<HPEN>(SelectObject(hdc, centerPen));
                    LONG midY = (laneRect.top + laneRect.bottom) / 2;
                    MoveToEx(hdc, laneRect.left, midY, nullptr);
                    LineTo(hdc, laneRect.right, midY);
                    SelectObject(hdc, oldPen);
                    DeleteObject(centerPen);
                }

                HBRUSH barBrush = CreateSolidBrush(kPianoRollActiveNote);
                HBRUSH disabledBrush = CreateSolidBrush(RGB(30, 30, 30));

                for (int column = 0; column < kSequencerStepsPerPage; ++column)
                {
                    LONG columnLeft = laneColumns[static_cast<size_t>(column)];
                    LONG columnRight = laneColumns[static_cast<size_t>(column + 1)];
                    if (columnRight < columnLeft)
                        columnRight = columnLeft;

                    RECT columnRect {columnLeft, laneRect.top, columnRight, laneRect.bottom};
                    int stepIndex = startStep + column;
                    bool validStep = trackId > 0 && stepIndex < totalSteps;

                    if (!validStep)
                    {
                        FillRect(hdc, &columnRect, disabledBrush);
                        continue;
                    }

                    LONG innerLeft = columnLeft + 2;
                    LONG innerRight = columnRight - 2;
                    LONG innerTop = laneRect.top + 2;
                    LONG innerBottom = laneRect.bottom - 2;
                    if (innerRight <= innerLeft || innerBottom <= innerTop)
                        continue;

                    switch (gPianoRollSelectedMenuTab)
                    {
                    case 0:
                    {
                        const auto& notes = columnNotes[static_cast<size_t>(column)];
                        if (notes.empty())
                            break;

                        LONG laneHeight = innerBottom - innerTop;
                        if (laneHeight <= 0)
                            break;

                        int sliderCount = static_cast<int>(notes.size());
                        if (sliderCount <= 0)
                            break;

                        double range = static_cast<double>(kTrackStepVelocityMax) -
                                        static_cast<double>(kTrackStepVelocityMin);
                        if (range <= 0.0)
                            range = 1.0;

                        LONG barBottom = innerBottom;
                        auto computeBarTop = [&](float velocityValue) {
                            double normalized = (static_cast<double>(velocityValue) -
                                                 static_cast<double>(kTrackStepVelocityMin)) /
                                                range;
                            normalized = std::clamp(normalized, 0.0, 1.0);
                            LONG top = barBottom -
                                       static_cast<LONG>(std::round(normalized * static_cast<double>(laneHeight)));
                            if (top < innerTop)
                                top = innerTop;
                            return top;
                        };

                        if (sliderCount == 1)
                        {
                            int midiNote = notes.front();
                            float noteVelocity = trackGetStepNoteVelocity(trackId, stepIndex, midiNote);
                            LONG barTop = computeBarTop(noteVelocity);
                            RECT barRect {innerLeft, barTop, innerRight, barBottom};
                            if (barRect.bottom > barRect.top && barRect.right > barRect.left)
                                FillRect(hdc, &barRect, barBrush);
                            break;
                        }

                        double columnWidth = static_cast<double>(std::max<LONG>(1, innerRight - innerLeft));
                        double segmentWidth = columnWidth / static_cast<double>(sliderCount);
                        double currentLeft = static_cast<double>(innerLeft);
                        for (int noteIndex = 0; noteIndex < sliderCount; ++noteIndex)
                        {
                            double nextLeft = currentLeft + segmentWidth;
                            LONG segmentLeft = static_cast<LONG>(std::lround(currentLeft));
                            LONG segmentRight = (noteIndex == sliderCount - 1)
                                                    ? innerRight
                                                    : static_cast<LONG>(std::lround(nextLeft));

                            if (sliderCount > 1)
                            {
                                if (noteIndex > 0 && segmentLeft < innerRight)
                                    ++segmentLeft;
                                if (noteIndex < sliderCount - 1 && segmentRight > segmentLeft)
                                    --segmentRight;
                            }

                            if (segmentRight > innerRight)
                                segmentRight = innerRight;
                            if (segmentRight <= segmentLeft)
                            {
                                currentLeft = nextLeft;
                                continue;
                            }

                            int midiNote = notes[static_cast<size_t>(noteIndex)];
                            float noteVelocity = trackGetStepNoteVelocity(trackId, stepIndex, midiNote);
                            LONG barTop = computeBarTop(noteVelocity);
                            RECT barRect {segmentLeft, barTop, segmentRight, barBottom};
                            if (barRect.bottom > barRect.top && barRect.right > barRect.left)
                            {
                                double shadeStep = sliderCount > 1
                                                       ? 0.25 / static_cast<double>(sliderCount - 1)
                                                       : 0.0;
                                double shadeFactor = 1.0 - shadeStep * static_cast<double>(noteIndex);
                                COLORREF segmentColor = scaleColor(kPianoRollActiveNote, shadeFactor);
                                HBRUSH segmentBrush = CreateSolidBrush(segmentColor);
                                FillRect(hdc, &barRect, segmentBrush);
                                DeleteObject(segmentBrush);
                            }

                            currentLeft = nextLeft;
                        }
                        break;
                    }
                    case 1:
                    {
                        float pitch = trackGetStepPitchOffset(trackId, stepIndex);
                        double maxAbs = std::max(std::abs(static_cast<double>(kTrackStepPitchMin)),
                                                 std::abs(static_cast<double>(kTrackStepPitchMax)));
                        double normalized = maxAbs > 0.0 ? static_cast<double>(pitch) / maxAbs : 0.0;
                        normalized = std::clamp(normalized, -1.0, 1.0);
                        LONG centerY = (innerTop + innerBottom) / 2;
                        if (normalized >= 0.0)
                        {
                            LONG barTop = centerY - static_cast<LONG>(std::round(normalized * (innerBottom - innerTop) * 0.5));
                            if (barTop < innerTop)
                                barTop = innerTop;
                            RECT barRect {innerLeft, barTop, innerRight, centerY};
                            if (barRect.bottom > barRect.top && barRect.right > barRect.left)
                                FillRect(hdc, &barRect, barBrush);
                        }
                        else
                        {
                            LONG barBottom = centerY + static_cast<LONG>(std::round(-normalized * (innerBottom - innerTop) * 0.5));
                            if (barBottom > innerBottom)
                                barBottom = innerBottom;
                            RECT barRect {innerLeft, centerY, innerRight, barBottom};
                            if (barRect.bottom > barRect.top && barRect.right > barRect.left)
                                FillRect(hdc, &barRect, barBrush);
                        }
                        break;
                    }
                    default:
                        break;
                    }
                }

                DeleteObject(disabledBrush);
                DeleteObject(barBrush);
            }
        }

        if (layout.collapseBar.right > layout.collapseBar.left && layout.collapseBar.bottom > layout.collapseBar.top)
        {
            if (layout.menuCollapsed)
            {
                HBRUSH collapseBrush = CreateSolidBrush(kPianoRollMenuBackground);
                FillRect(hdc, &layout.collapseBar, collapseBrush);
                DeleteObject(collapseBrush);

                RECT labelRect = layout.collapseBar;
                labelRect.left += 12;
                labelRect.right = layout.collapseButton.left - 8;
                if (labelRect.right > labelRect.left)
                {
                    DrawTextW(hdc, L"Note Editor", -1, &labelRect,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                }
            }

            if (layout.collapseButton.right > layout.collapseButton.left &&
                layout.collapseButton.bottom > layout.collapseButton.top)
            {
                HBRUSH buttonBrush = CreateSolidBrush(layout.menuCollapsed ? kPianoRollMenuTabActive
                                                                           : kPianoRollMenuTabInactive);
                FillRect(hdc, &layout.collapseButton, buttonBrush);
                DeleteObject(buttonBrush);

                HBRUSH buttonBorder = CreateSolidBrush(RGB(80, 80, 80));
                FrameRect(hdc, &layout.collapseButton, buttonBorder);
                DeleteObject(buttonBorder);

                HBRUSH arrowBrush = CreateSolidBrush(RGB(230, 230, 230));
                HGDIOBJ oldBrush = SelectObject(hdc, arrowBrush);
                HGDIOBJ oldPen = SelectObject(hdc, GetStockObject(NULL_PEN));

                int centerX = (layout.collapseButton.left + layout.collapseButton.right) / 2;
                int centerY = (layout.collapseButton.top + layout.collapseButton.bottom) / 2;
                int halfWidth = 6;
                int halfHeight = 4;
                POINT arrow[3];
                if (layout.menuCollapsed)
                {
                    arrow[0] = {centerX - halfWidth, centerY + halfHeight};
                    arrow[1] = {centerX + halfWidth, centerY + halfHeight};
                    arrow[2] = {centerX, centerY - halfHeight};
                }
                else
                {
                    arrow[0] = {centerX - halfWidth, centerY - halfHeight};
                    arrow[1] = {centerX + halfWidth, centerY - halfHeight};
                    arrow[2] = {centerX, centerY + halfHeight};
                }
                Polygon(hdc, arrow, 3);

                SelectObject(hdc, oldPen);
                SelectObject(hdc, oldBrush);
                DeleteObject(arrowBrush);
            }
        }

        HBRUSH borderBrush = CreateSolidBrush(RGB(15, 15, 15));
        FrameRect(hdc, &layout.keyboard, borderBrush);
        FrameRect(hdc, &layout.grid, borderBrush);
        if (layout.menuArea.right > layout.menuArea.left && layout.menuArea.bottom > layout.menuArea.top)
            FrameRect(hdc, &layout.menuArea, borderBrush);
        else if (layout.collapseBar.right > layout.collapseBar.left && layout.collapseBar.bottom > layout.collapseBar.top)
            FrameRect(hdc, &layout.collapseBar, borderBrush);
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
        requestMainMenuRefresh();
    }
}

enum
{
    kEffectsTrackListId = 1001,
    kEffectsVolumeSliderId = 1002,
    kEffectsPanSliderId = 1003,
    kEffectsVolumeToggleId = 1010,
    kEffectsPanToggleId = 1011,
    kEffectsListViewId = 1050,
};

enum class EffectListItemType
{
    Eq,
    Delay,
    Compressor,
    Sidechain,
};

struct EffectListEntry
{
    EffectListItemType type;
    std::wstring name;
};

struct EffectsWindowState
{
    HWND trackList = nullptr;
    HWND trackLabel = nullptr;
    HWND effectList = nullptr;
    HWND volumeHeader = nullptr;
    HWND volumeSlider = nullptr;
    HWND volumeValueLabel = nullptr;
    HWND panHeader = nullptr;
    HWND panSlider = nullptr;
    HWND panValueLabel = nullptr;
    bool volumeExpanded = true;
    bool panExpanded = true;
    bool effectListUpdating = false;
    std::vector<EffectListEntry> effectEntries;
    int selectedTrackId = 0;
};

EffectsWindowState* getEffectsWindowState(HWND hwnd)
{
    return reinterpret_cast<EffectsWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void effectsWindowApplyFont(const EffectsWindowState& state, HFONT font)
{
    const HWND controls[] = {
        state.trackList,
        state.trackLabel,
        state.effectList,
        state.volumeHeader,
        state.volumeSlider,
        state.volumeValueLabel,
        state.panHeader,
        state.panSlider,
        state.panValueLabel,
    };

    for (HWND control : controls)
    {
        if (control)
        {
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        }
    }
}

void effectsWindowUpdateSectionHeader(HWND header, const wchar_t* text, bool expanded)
{
    if (!header || !text)
        return;

    std::wstring displayText = expanded ? L"\u25BC " : L"\u25B6 ";
    displayText += text;
    SetWindowTextW(header, displayText.c_str());
}

void effectsWindowLayout(HWND hwnd, EffectsWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int listWidth = 180;
    const int headerHeight = 18;
    const int sliderHeight = 32;
    const int controlSpacing = 10;
    const int labelToSliderSpacing = 4;
    const int valueLabelWidth = 110;
    const int effectListMinHeight = 120;

    int usableWidth = std::max(width - padding * 3 - listWidth, 160);
    int listHeight = std::max(height - padding * 2, 80);

    if (state->trackList)
    {
        MoveWindow(state->trackList, padding, padding, listWidth, listHeight, TRUE);
    }

    int rightLeft = padding + listWidth + padding;
    if (rightLeft + usableWidth > width - padding)
    {
        usableWidth = std::max(width - rightLeft - padding, 160);
    }

    int currentY = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, rightLeft, currentY, usableWidth, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    if (state->effectList)
    {
        int availableHeight = height - currentY - padding;
        if (availableHeight <= 0)
            availableHeight = effectListMinHeight;

        int effectListHeight = std::max(effectListMinHeight, availableHeight / 2);
        if (effectListHeight > availableHeight)
            effectListHeight = availableHeight;

        MoveWindow(state->effectList, rightLeft, currentY, usableWidth, effectListHeight, TRUE);
        currentY += effectListHeight + controlSpacing;
    }

    auto layoutSection = [&](HWND header, HWND slider, HWND valueLabel, bool expanded)
    {
        if (!header || !valueLabel)
            return;

        ShowWindow(header, SW_SHOW);
        ShowWindow(valueLabel, SW_SHOW);
        if (slider)
            ShowWindow(slider, expanded ? SW_SHOW : SW_HIDE);

        MoveWindow(header, rightLeft, currentY, usableWidth - valueLabelWidth, headerHeight, TRUE);
        MoveWindow(valueLabel, rightLeft + usableWidth - valueLabelWidth, currentY, valueLabelWidth, headerHeight, TRUE);
        currentY += headerHeight;

        if (expanded && slider)
        {
            currentY += labelToSliderSpacing;
            MoveWindow(slider, rightLeft, currentY, usableWidth, sliderHeight, TRUE);
            currentY += sliderHeight + controlSpacing;
        }
        else
        {
            currentY += controlSpacing;
        }
    };

    layoutSection(state->volumeHeader, state->volumeSlider, state->volumeValueLabel, state->volumeExpanded);
    layoutSection(state->panHeader, state->panSlider, state->panValueLabel, state->panExpanded);
}

void effectsWindowSetValueText(HWND control, const std::string& text)
{
    if (!control)
        return;

    std::wstring wide = ToWideString(text);
    if (wide.empty())
        wide = L"-";
    SetWindowTextW(control, wide.c_str());
}

void effectsWindowDisableControls(EffectsWindowState* state)
{
    if (!state)
        return;

    const HWND sliders[] = {
        state->volumeSlider,
        state->panSlider,
    };

    for (HWND slider : sliders)
    {
        if (slider)
        {
            EnableWindow(slider, FALSE);
            SendMessageW(slider, TBM_SETPOS, TRUE, 0);
        }
    }

    const HWND valueLabels[] = {
        state->volumeValueLabel,
        state->panValueLabel,
    };

    for (HWND label : valueLabels)
    {
        if (label)
        {
            SetWindowTextW(label, L"-");
        }
    }

    if (state->effectList)
    {
        state->effectListUpdating = true;
        int itemCount = ListView_GetItemCount(state->effectList);
        for (int i = 0; i < itemCount; ++i)
        {
            ListView_SetCheckState(state->effectList, i, FALSE);
        }
        state->effectListUpdating = false;
        EnableWindow(state->effectList, FALSE);
    }
}

void effectsWindowSyncControls(HWND hwnd, EffectsWindowState* state)
{
    if (!state)
        return;

    Track fallbackTrack {};
    const Track* trackPtr = nullptr;

    if (state->selectedTrackId > 0)
    {
        auto tracks = getTracks();
        trackPtr = findTrackById(tracks, state->selectedTrackId);
        if (!trackPtr)
        {
            fallbackTrack.id = state->selectedTrackId;
            fallbackTrack.type = trackGetType(state->selectedTrackId);
            fallbackTrack.synthWaveType = trackGetSynthWaveType(state->selectedTrackId);
            fallbackTrack.volume = trackGetVolume(state->selectedTrackId);
            fallbackTrack.pan = trackGetPan(state->selectedTrackId);
            fallbackTrack.lowGainDb = trackGetEqLowGain(state->selectedTrackId);
            fallbackTrack.midGainDb = trackGetEqMidGain(state->selectedTrackId);
            fallbackTrack.highGainDb = trackGetEqHighGain(state->selectedTrackId);
            fallbackTrack.eqEnabled = trackGetEqEnabled(state->selectedTrackId);
            fallbackTrack.delayEnabled = trackGetDelayEnabled(state->selectedTrackId);
            fallbackTrack.delayTimeMs = trackGetDelayTimeMs(state->selectedTrackId);
            fallbackTrack.delayFeedback = trackGetDelayFeedback(state->selectedTrackId);
            fallbackTrack.delayMix = trackGetDelayMix(state->selectedTrackId);
            fallbackTrack.compressorEnabled = trackGetCompressorEnabled(state->selectedTrackId);
            fallbackTrack.compressorThresholdDb = trackGetCompressorThresholdDb(state->selectedTrackId);
            fallbackTrack.compressorRatio = trackGetCompressorRatio(state->selectedTrackId);
            fallbackTrack.compressorAttack = trackGetCompressorAttack(state->selectedTrackId);
            fallbackTrack.compressorRelease = trackGetCompressorRelease(state->selectedTrackId);
            fallbackTrack.formant = trackGetSynthFormant(state->selectedTrackId);
            fallbackTrack.feedback = trackGetSynthFeedback(state->selectedTrackId);
            fallbackTrack.pitch = trackGetSynthPitch(state->selectedTrackId);
            fallbackTrack.pitchRange = trackGetSynthPitchRange(state->selectedTrackId);
            fallbackTrack.synthAttack = trackGetSynthAttack(state->selectedTrackId);
            fallbackTrack.synthDecay = trackGetSynthDecay(state->selectedTrackId);
            fallbackTrack.synthSustain = trackGetSynthSustain(state->selectedTrackId);
            fallbackTrack.synthRelease = trackGetSynthRelease(state->selectedTrackId);
            fallbackTrack.sampleAttack = trackGetSampleAttack(state->selectedTrackId);
            fallbackTrack.sampleRelease = trackGetSampleRelease(state->selectedTrackId);
            trackPtr = &fallbackTrack;
        }
    }

    if (trackPtr)
    {
        std::wstring trackLabel = ToWideString(trackPtr->name);
        if (trackLabel.empty())
            trackLabel = L"Unnamed Track";
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, trackLabel.c_str());

        if (state->volumeSlider)
        {
            EnableWindow(state->volumeSlider, TRUE);
            int volumePos = static_cast<int>(std::lround(std::clamp(trackPtr->volume, kMixerVolumeMin, kMixerVolumeMax) * 100.0f));
            SendMessageW(state->volumeSlider, TBM_SETPOS, TRUE, volumePos);
            effectsWindowSetValueText(state->volumeValueLabel, formatVolumeValue(trackPtr->volume));
        }

        if (state->panSlider)
        {
            EnableWindow(state->panSlider, TRUE);
            float clampedPan = std::clamp(trackPtr->pan, kMixerPanMin, kMixerPanMax);
            int panPos = static_cast<int>(std::lround((clampedPan - kMixerPanMin) * 100.0f));
            SendMessageW(state->panSlider, TBM_SETPOS, TRUE, panPos);
            effectsWindowSetValueText(state->panValueLabel, formatPanValue(trackPtr->pan));
        }

        if (state->effectList)
        {
            EnableWindow(state->effectList, TRUE);
            state->effectListUpdating = true;
            for (size_t i = 0; i < state->effectEntries.size(); ++i)
            {
                bool checked = false;
                switch (state->effectEntries[i].type)
                {
                case EffectListItemType::Eq:
                    checked = trackPtr->eqEnabled;
                    break;
                case EffectListItemType::Delay:
                    checked = trackPtr->delayEnabled;
                    break;
                case EffectListItemType::Compressor:
                    checked = trackPtr->compressorEnabled;
                    break;
                case EffectListItemType::Sidechain:
                    checked = trackPtr->sidechainEnabled;
                    break;
                }
                ListView_SetCheckState(state->effectList, static_cast<int>(i), checked ? TRUE : FALSE);
            }
            state->effectListUpdating = false;
        }
    }
    else
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"Select a track from the list.");
        effectsWindowDisableControls(state);
    }

    if (hwnd && IsWindow(hwnd))
        InvalidateRect(hwnd, nullptr, TRUE);
}

void effectsWindowPopulateTrackList(HWND hwnd, EffectsWindowState* state)
{
    if (!state || !state->trackList)
        return;

    int desiredTrackId = state->selectedTrackId;

    SendMessageW(state->trackList, LB_RESETCONTENT, 0, 0);

    auto tracks = getTracks();
    int selectIndex = -1;
    for (const auto& track : tracks)
    {
        std::wstring name = ToWideString(track.name);
        if (name.empty())
        {
            name = L"Track " + std::to_wstring(track.id);
        }

        int index = static_cast<int>(SendMessageW(state->trackList, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name.c_str())));
        SendMessageW(state->trackList, LB_SETITEMDATA, index, track.id);
        if (track.id == desiredTrackId)
            selectIndex = index;
    }

    if (selectIndex >= 0)
    {
        SendMessageW(state->trackList, LB_SETCURSEL, selectIndex, 0);
    }
    else if (!tracks.empty())
    {
        SendMessageW(state->trackList, LB_SETCURSEL, 0, 0);
        state->selectedTrackId = tracks.front().id;
    }
    else
    {
        state->selectedTrackId = 0;
    }
}

void effectsWindowEnsureSelectionVisible(HWND hwnd, EffectsWindowState* state)
{
    if (!state || !state->trackList || state->selectedTrackId <= 0)
        return;

    int count = static_cast<int>(SendMessageW(state->trackList, LB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        if (static_cast<int>(SendMessageW(state->trackList, LB_GETITEMDATA, i, 0)) == state->selectedTrackId)
        {
            SendMessageW(state->trackList, LB_SETCURSEL, i, 0);
            SendMessageW(state->trackList, LB_SETTOPINDEX, i, 0);
            break;
        }
    }
}

struct SidechainWindowState
{
    int trackId = 0;
    HWND trackLabel = nullptr;
    HWND enableCheckbox = nullptr;
    HWND inputLabel = nullptr;
    HWND inputCombo = nullptr;
    HWND amountSlider = nullptr;
    HWND amountValueLabel = nullptr;
    HWND attackSlider = nullptr;
    HWND attackValueLabel = nullptr;
    HWND releaseSlider = nullptr;
    HWND releaseValueLabel = nullptr;
    bool populatingSource = false;
};

SidechainWindowState* getSidechainWindowState(HWND hwnd)
{
    return reinterpret_cast<SidechainWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void sidechainWindowApplyFont(const SidechainWindowState& state, HFONT font)
{
    const HWND controls[] = {
        state.trackLabel,
        state.enableCheckbox,
        state.inputLabel,
        state.inputCombo,
        state.amountSlider,
        state.amountValueLabel,
        state.attackSlider,
        state.attackValueLabel,
        state.releaseSlider,
        state.releaseValueLabel,
    };

    for (HWND control : controls)
    {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void sidechainWindowDisableControls(SidechainWindowState* state)
{
    if (!state)
        return;

    if (state->inputCombo)
        EnableWindow(state->inputCombo, FALSE);

    const HWND sliders[] = {
        state->amountSlider,
        state->attackSlider,
        state->releaseSlider,
    };

    for (HWND slider : sliders)
    {
        if (slider)
        {
            EnableWindow(slider, FALSE);
            SendMessageW(slider, TBM_SETPOS, TRUE, 0);
        }
    }

    const HWND labels[] = {
        state->amountValueLabel,
        state->attackValueLabel,
        state->releaseValueLabel,
    };

    for (HWND label : labels)
    {
        if (label)
            SetWindowTextW(label, L"-");
    }
}

void sidechainWindowPopulateSourceList(SidechainWindowState* state, int currentSourceTrackId)
{
    if (!state || !state->inputCombo)
        return;

    state->populatingSource = true;
    SendMessageW(state->inputCombo, CB_RESETCONTENT, 0, 0);

    std::wstring noneText = L"None";
    int noneIndex = static_cast<int>(SendMessageW(state->inputCombo,
                                                  CB_ADDSTRING,
                                                  0,
                                                  reinterpret_cast<LPARAM>(noneText.c_str())));
    if (noneIndex >= 0)
        SendMessageW(state->inputCombo, CB_SETITEMDATA, noneIndex, static_cast<LPARAM>(-1));

    int selectIndex = (currentSourceTrackId <= 0) ? noneIndex : -1;

    auto tracks = getTracks();
    for (const auto& track : tracks)
    {
        std::wstring name = ToWideString(track.name);
        if (name.empty())
            name = L"Track " + std::to_wstring(track.id);

        int index = static_cast<int>(SendMessageW(state->inputCombo,
                                                  CB_ADDSTRING,
                                                  0,
                                                  reinterpret_cast<LPARAM>(name.c_str())));
        if (index >= 0)
        {
            SendMessageW(state->inputCombo, CB_SETITEMDATA, index, static_cast<LPARAM>(track.id));
            if (track.id == currentSourceTrackId)
                selectIndex = index;
        }
    }

    if (selectIndex >= 0)
        SendMessageW(state->inputCombo, CB_SETCURSEL, selectIndex, 0);
    else if (noneIndex >= 0)
        SendMessageW(state->inputCombo, CB_SETCURSEL, noneIndex, 0);
    else
        SendMessageW(state->inputCombo, CB_SETCURSEL, 0, 0);

    state->populatingSource = false;
}

void sidechainWindowSyncControls(HWND hwnd, SidechainWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"No track selected");
        if (state->enableCheckbox)
        {
            EnableWindow(state->enableCheckbox, FALSE);
            SendMessageW(state->enableCheckbox, BM_SETCHECK, BST_UNCHECKED, 0);
        }
        sidechainWindowPopulateSourceList(state, -1);
        sidechainWindowDisableControls(state);
        if (hwnd && IsWindow(hwnd))
            InvalidateRect(hwnd, nullptr, TRUE);
        return;
    }

    Track fallbackTrack {};
    const Track* trackPtr = nullptr;
    auto tracks = getTracks();
    trackPtr = findTrackById(tracks, trackId);
    if (!trackPtr)
    {
        fallbackTrack.id = trackId;
        fallbackTrack.name = "Track " + std::to_string(trackId);
        fallbackTrack.sidechainEnabled = trackGetSidechainEnabled(trackId);
        fallbackTrack.sidechainSourceTrackId = trackGetSidechainSourceTrack(trackId);
        fallbackTrack.sidechainAmount = trackGetSidechainAmount(trackId);
        fallbackTrack.sidechainAttack = trackGetSidechainAttack(trackId);
        fallbackTrack.sidechainRelease = trackGetSidechainRelease(trackId);
        trackPtr = &fallbackTrack;
    }

    std::wstring labelText = ToWideString(trackPtr->name);
    if (labelText.empty())
        labelText = L"Unnamed Track";
    labelText += L" - Sidechain";
    if (state->trackLabel)
        SetWindowTextW(state->trackLabel, labelText.c_str());

    if (state->enableCheckbox)
    {
        EnableWindow(state->enableCheckbox, TRUE);
        SendMessageW(state->enableCheckbox,
                     BM_SETCHECK,
                     trackPtr->sidechainEnabled ? BST_CHECKED : BST_UNCHECKED,
                     0);
    }

    sidechainWindowPopulateSourceList(state, trackPtr->sidechainSourceTrackId);
    if (state->inputCombo)
        EnableWindow(state->inputCombo, TRUE);

    auto setSlider = [&](HWND slider,
                         HWND valueLabel,
                         float value,
                         float minValue,
                         float maxValue,
                         float scale,
                         auto formatter)
    {
        if (!slider)
            return;
        float clamped = std::clamp(value, minValue, maxValue);
        EnableWindow(slider, trackPtr->sidechainEnabled ? TRUE : FALSE);
        int pos = static_cast<int>(std::lround(clamped * scale));
        SendMessageW(slider, TBM_SETPOS, TRUE, pos);
        if (valueLabel)
            effectsWindowSetValueText(valueLabel, formatter(clamped));
    };

    setSlider(state->amountSlider,
              state->amountValueLabel,
              trackPtr->sidechainAmount,
              0.0f,
              1.0f,
              100.0f,
              [](float v) { return formatDelayPercentValue(v); });

    setSlider(state->attackSlider,
              state->attackValueLabel,
              trackPtr->sidechainAttack,
              0.0f,
              4.0f,
              1000.0f,
              [](float v) { return formatSecondsValue(v); });

    setSlider(state->releaseSlider,
              state->releaseValueLabel,
              trackPtr->sidechainRelease,
              0.0f,
              4.0f,
              1000.0f,
              [](float v) { return formatSecondsValue(v); });

    if (hwnd && IsWindow(hwnd))
        InvalidateRect(hwnd, nullptr, TRUE);
}

void sidechainWindowLayout(HWND hwnd, SidechainWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int headerHeight = 24;
    const int sliderHeight = 32;
    const int controlSpacing = 12;
    const int valueLabelWidth = 110;
    const int inputLabelWidth = 100;

    int currentY = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    if (state->enableCheckbox)
    {
        MoveWindow(state->enableCheckbox, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    if (state->inputLabel && state->inputCombo)
    {
        int comboWidth = std::max(100, width - padding * 2 - inputLabelWidth - controlSpacing);
        MoveWindow(state->inputLabel, padding, currentY, inputLabelWidth, headerHeight, TRUE);
        MoveWindow(state->inputCombo,
                   padding + inputLabelWidth + controlSpacing,
                   currentY,
                   comboWidth,
                   headerHeight * 6,
                   TRUE);
        currentY += headerHeight + controlSpacing;
    }

    auto layoutSlider = [&](HWND slider, HWND valueLabel)
    {
        if (!slider)
            return;
        int sliderWidth = std::max(100, width - padding * 3 - valueLabelWidth);
        MoveWindow(slider, padding, currentY, sliderWidth, sliderHeight, TRUE);
        if (valueLabel)
        {
            MoveWindow(valueLabel,
                       padding + sliderWidth + padding,
                       currentY,
                       valueLabelWidth,
                       sliderHeight,
                       TRUE);
        }
        currentY += sliderHeight + controlSpacing;
    };

    layoutSlider(state->amountSlider, state->amountValueLabel);
    layoutSlider(state->attackSlider, state->attackValueLabel);
    layoutSlider(state->releaseSlider, state->releaseValueLabel);
}

LRESULT CALLBACK SidechainWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    SidechainWindowState* state = getSidechainWindowState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* newState = new SidechainWindowState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

        HINSTANCE instance = GetModuleHandle(nullptr);
        DWORD staticStyle = WS_CHILD | WS_VISIBLE;

        newState->trackLabel = CreateWindowExW(0,
                                               L"STATIC",
                                               L"",
                                               staticStyle,
                                               0,
                                               0,
                                               100,
                                               20,
                                               hwnd,
                                               nullptr,
                                               instance,
                                               nullptr);

        newState->enableCheckbox = CreateWindowExW(0,
                                                   L"BUTTON",
                                                   L"Enable sidechain",
                                                   staticStyle | WS_TABSTOP | BS_AUTOCHECKBOX,
                                                   0,
                                                   0,
                                                   100,
                                                   20,
                                                   hwnd,
                                                   nullptr,
                                                   instance,
                                                   nullptr);

        newState->inputLabel = CreateWindowExW(0,
                                               L"STATIC",
                                               L"Input Track:",
                                               staticStyle,
                                               0,
                                               0,
                                               100,
                                               20,
                                               hwnd,
                                               nullptr,
                                               instance,
                                               nullptr);

        newState->inputCombo = CreateWindowExW(0,
                                               L"COMBOBOX",
                                               L"",
                                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                                               0,
                                               0,
                                               120,
                                               120,
                                               hwnd,
                                               nullptr,
                                               instance,
                                               nullptr);

        auto createSlider = [&](HWND& slider, HWND& valueLabel, int rangeMin, int rangeMax, int ticFreq)
        {
            slider = CreateWindowExW(0,
                                     TRACKBAR_CLASSW,
                                     L"",
                                     WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                     0,
                                     0,
                                     100,
                                     30,
                                     hwnd,
                                     nullptr,
                                     instance,
                                     nullptr);
            if (slider)
            {
                SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
                SendMessageW(slider, TBM_SETTICFREQ, ticFreq, 0);
            }
            valueLabel = CreateWindowExW(0,
                                         L"STATIC",
                                         L"-",
                                         staticStyle | SS_RIGHT,
                                         0,
                                         0,
                                         80,
                                         20,
                                         hwnd,
                                         nullptr,
                                         instance,
                                         nullptr);
        };

        createSlider(newState->amountSlider, newState->amountValueLabel, 0, 100, 10);
        createSlider(newState->attackSlider, newState->attackValueLabel, 0, 4000, 250);
        createSlider(newState->releaseSlider, newState->releaseValueLabel, 0, 4000, 250);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        sidechainWindowApplyFont(*newState, font);

        RECT client {0, 0, 0, 0};
        GetClientRect(hwnd, &client);
        sidechainWindowLayout(hwnd, newState, client.right - client.left, client.bottom - client.top);
        sidechainWindowPopulateSourceList(newState, -1);
        sidechainWindowSyncControls(hwnd, newState);
        return 0;
    }
    case WM_SIZE:
        if (state)
            sidechainWindowLayout(hwnd, state, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (state)
        {
            HWND control = reinterpret_cast<HWND>(lParam);
            if (control == state->enableCheckbox && HIWORD(wParam) == BN_CLICKED)
            {
                int trackId = state->trackId;
                if (trackId > 0)
                {
                    bool enabled = SendMessageW(state->enableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                    trackSetSidechainEnabled(trackId, enabled);
                    sidechainWindowSyncControls(hwnd, state);
                    notifyEffectsWindowTrackValuesChanged(trackId);
                    if (gMainWindow && IsWindow(gMainWindow))
                        InvalidateRect(gMainWindow, nullptr, FALSE);
                }
                return 0;
            }

            if (control == state->inputCombo && HIWORD(wParam) == CBN_SELCHANGE)
            {
                if (state->populatingSource)
                    return 0;

                int trackId = state->trackId;
                if (trackId > 0)
                {
                    int selection = static_cast<int>(SendMessageW(state->inputCombo, CB_GETCURSEL, 0, 0));
                    if (selection >= 0)
                    {
                        LRESULT data = SendMessageW(state->inputCombo, CB_GETITEMDATA, selection, 0);
                        int sourceTrackId = static_cast<int>(data);
                        trackSetSidechainSourceTrack(trackId, sourceTrackId);
                        notifyEffectsWindowTrackValuesChanged(trackId);
                        if (gMainWindow && IsWindow(gMainWindow))
                            InvalidateRect(gMainWindow, nullptr, FALSE);
                    }
                }
                return 0;
            }
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

            if (control == state->amountSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float value = std::clamp(static_cast<float>(pos) / 100.0f, 0.0f, 1.0f);
                trackSetSidechainAmount(trackId, value);
                sidechainWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return 0;
            }

            if (control == state->attackSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float value = std::clamp(static_cast<float>(pos) / 1000.0f, 0.0f, 4.0f);
                trackSetSidechainAttack(trackId, value);
                sidechainWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return 0;
            }

            if (control == state->releaseSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float value = std::clamp(static_cast<float>(pos) / 1000.0f, 0.0f, 4.0f);
                trackSetSidechainRelease(trackId, value);
                sidechainWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return 0;
            }
        }
        return 0;
    case WM_SIDECHAIN_SET_TRACK:
        if (state)
        {
            state->trackId = static_cast<int>(wParam);
            sidechainWindowSyncControls(hwnd, state);
        }
        return 0;
    case WM_SIDECHAIN_REFRESH_VALUES:
        if (state)
            sidechainWindowSyncControls(hwnd, state);
        return 0;
    case WM_SIDECHAIN_RELOAD_TRACKS:
        if (state)
            sidechainWindowSyncControls(hwnd, state);
        return 0;
    case WM_DESTROY:
    {
        auto* toDelete = state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        delete toDelete;
        if (hwnd == gSidechainWindow)
        {
            gSidechainWindow = nullptr;
            requestMainMenuRefresh();
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureSidechainWindowClass()
{
    if (gSidechainWindowClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = SidechainWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kSidechainWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
        gSidechainWindowClassRegistered = true;
}

void openSidechainWindow(HWND parent, int trackId)
{
    ensureSidechainWindowClass();
    if (!gSidechainWindowClassRegistered)
        return;

    if (gSidechainWindow && IsWindow(gSidechainWindow))
    {
        ShowWindow(gSidechainWindow, SW_SHOWNORMAL);
        SetForegroundWindow(gSidechainWindow);
        PostMessageW(gSidechainWindow, WM_SIDECHAIN_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        return;
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parent && IsWindow(parent))
    {
        RECT rect {0, 0, 0, 0};
        GetWindowRect(parent, &rect);
        x = rect.right + 20;
        y = rect.top + 20;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kSidechainWindowClassName,
                                L"Track Sidechain",
                                WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME,
                                x,
                                y,
                                kSidechainWindowWidth,
                                kSidechainWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gSidechainWindow = hwnd;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_SIDECHAIN_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        requestMainMenuRefresh();
    }
}

void notifySidechainWindowTrackChanged(int trackId)
{
    if (gSidechainWindow && IsWindow(gSidechainWindow))
        PostMessageW(gSidechainWindow, WM_SIDECHAIN_SET_TRACK, static_cast<WPARAM>(trackId), 0);
}

void notifySidechainWindowValuesChanged(int trackId)
{
    if (gSidechainWindow && IsWindow(gSidechainWindow))
    {
        SidechainWindowState* state = getSidechainWindowState(gSidechainWindow);
        if (!state || state->trackId == trackId)
            PostMessageW(gSidechainWindow, WM_SIDECHAIN_REFRESH_VALUES, 0, 0);
    }
}

void notifySidechainWindowTrackListChanged()
{
    if (gSidechainWindow && IsWindow(gSidechainWindow))
        PostMessageW(gSidechainWindow, WM_SIDECHAIN_RELOAD_TRACKS, 0, 0);
}

struct EqWindowState
{
    int trackId = 0;
    HWND trackLabel = nullptr;
    HWND enableCheckbox = nullptr;
    HWND lowSlider = nullptr;
    HWND lowValueLabel = nullptr;
    HWND midSlider = nullptr;
    HWND midValueLabel = nullptr;
    HWND highSlider = nullptr;
    HWND highValueLabel = nullptr;
};

EqWindowState* getEqWindowState(HWND hwnd)
{
    return reinterpret_cast<EqWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void eqWindowApplyFont(const EqWindowState& state, HFONT font)
{
    const HWND controls[] = {
        state.trackLabel,
        state.enableCheckbox,
        state.lowSlider,
        state.lowValueLabel,
        state.midSlider,
        state.midValueLabel,
        state.highSlider,
        state.highValueLabel,
    };

    for (HWND control : controls)
    {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void eqWindowSetValueText(HWND label, float value)
{
    if (!label)
        return;
    std::wstring text = ToWideString(formatEqValue(value));
    SetWindowTextW(label, text.c_str());
}

void eqWindowSyncControls(HWND hwnd, EqWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"No track selected");
        if (state->enableCheckbox)
            EnableWindow(state->enableCheckbox, FALSE);
        const HWND sliders[] = {state->lowSlider, state->midSlider, state->highSlider};
        const HWND labels[] = {state->lowValueLabel, state->midValueLabel, state->highValueLabel};
        for (HWND slider : sliders)
        {
            if (slider)
            {
                EnableWindow(slider, FALSE);
                SendMessageW(slider, TBM_SETPOS, TRUE, 0);
            }
        }
        for (HWND label : labels)
        {
            if (label)
                SetWindowTextW(label, L"-");
        }
        return;
    }

    Track fallbackTrack {};
    const Track* trackPtr = nullptr;
    auto tracks = getTracks();
    trackPtr = findTrackById(tracks, trackId);
    if (!trackPtr)
    {
        fallbackTrack.id = trackId;
        fallbackTrack.name = "Track " + std::to_string(trackId);
        fallbackTrack.lowGainDb = trackGetEqLowGain(trackId);
        fallbackTrack.midGainDb = trackGetEqMidGain(trackId);
        fallbackTrack.highGainDb = trackGetEqHighGain(trackId);
        fallbackTrack.eqEnabled = trackGetEqEnabled(trackId);
        trackPtr = &fallbackTrack;
    }

    std::wstring labelText = ToWideString(trackPtr->name);
    if (labelText.empty())
        labelText = L"Unnamed Track";
    labelText += L" - Equalizer";
    if (state->trackLabel)
        SetWindowTextW(state->trackLabel, labelText.c_str());

    bool eqEnabled = trackPtr->eqEnabled;
    if (state->enableCheckbox)
    {
        EnableWindow(state->enableCheckbox, TRUE);
        SendMessageW(state->enableCheckbox, BM_SETCHECK, eqEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    auto syncSlider = [&](HWND slider, HWND valueLabel, float gain)
    {
        if (!slider)
            return;
        EnableWindow(slider, eqEnabled ? TRUE : FALSE);
        int pos = static_cast<int>(std::lround((gain - kMixerEqMin) * 10.0f));
        SendMessageW(slider, TBM_SETPOS, TRUE, pos);
        eqWindowSetValueText(valueLabel, gain);
    };

    syncSlider(state->lowSlider, state->lowValueLabel, trackPtr->lowGainDb);
    syncSlider(state->midSlider, state->midValueLabel, trackPtr->midGainDb);
    syncSlider(state->highSlider, state->highValueLabel, trackPtr->highGainDb);
}

void eqWindowLayout(HWND hwnd, EqWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int headerHeight = 24;
    const int sliderHeight = 32;
    const int controlSpacing = 12;
    const int valueLabelWidth = 100;

    int currentY = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    if (state->enableCheckbox)
    {
        MoveWindow(state->enableCheckbox, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    auto layoutSlider = [&](HWND slider, HWND valueLabel)
    {
        if (!slider)
            return;
        int sliderWidth = std::max(100, width - padding * 3 - valueLabelWidth);
        MoveWindow(slider, padding, currentY, sliderWidth, sliderHeight, TRUE);
        if (valueLabel)
        {
            MoveWindow(valueLabel,
                       padding + sliderWidth + padding,
                       currentY,
                       valueLabelWidth,
                       sliderHeight,
                       TRUE);
        }
        currentY += sliderHeight + controlSpacing;
    };

    layoutSlider(state->lowSlider, state->lowValueLabel);
    layoutSlider(state->midSlider, state->midValueLabel);
    layoutSlider(state->highSlider, state->highValueLabel);
}

LRESULT CALLBACK EqWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    EqWindowState* state = getEqWindowState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* create = reinterpret_cast<LPCREATESTRUCT>(lParam);
        auto* newState = new EqWindowState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

        HINSTANCE instance = create->hInstance;
        DWORD staticStyle = WS_CHILD | WS_VISIBLE;

        newState->trackLabel = CreateWindowExW(0,
                                               L"STATIC",
                                               L"Equalizer",
                                               staticStyle,
                                               0,
                                               0,
                                               100,
                                               20,
                                               hwnd,
                                               nullptr,
                                               instance,
                                               nullptr);
        newState->enableCheckbox = CreateWindowExW(0,
                                                   L"BUTTON",
                                                   L"Enable Equalizer",
                                                   staticStyle | BS_AUTOCHECKBOX | WS_TABSTOP,
                                                   0,
                                                   0,
                                                   150,
                                                   24,
                                                   hwnd,
                                                   nullptr,
                                                   instance,
                                                   nullptr);

        auto createSlider = [&](HWND& slider, HWND& valueLabel)
        {
            slider = CreateWindowExW(0,
                                      TRACKBAR_CLASSW,
                                      L"",
                                      WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                      0,
                                      0,
                                      100,
                                      30,
                                      hwnd,
                                      nullptr,
                                      instance,
                                      nullptr);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, 240));
            SendMessageW(slider, TBM_SETTICFREQ, 20, 0);
            valueLabel = CreateWindowExW(0,
                                         L"STATIC",
                                         L"-",
                                         staticStyle | SS_RIGHT,
                                         0,
                                         0,
                                         80,
                                         20,
                                         hwnd,
                                         nullptr,
                                         instance,
                                         nullptr);
        };

        createSlider(newState->lowSlider, newState->lowValueLabel);
        createSlider(newState->midSlider, newState->midValueLabel);
        createSlider(newState->highSlider, newState->highValueLabel);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        eqWindowApplyFont(*newState, font);

        RECT client {0, 0, 0, 0};
        GetClientRect(hwnd, &client);
        eqWindowLayout(hwnd, newState, client.right - client.left, client.bottom - client.top);
        eqWindowSyncControls(hwnd, newState);
        return 0;
    }
    case WM_SIZE:
        if (state)
            eqWindowLayout(hwnd, state, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (state && reinterpret_cast<HWND>(lParam) == state->enableCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            int trackId = state->trackId;
            if (trackId > 0)
            {
                bool enabled = SendMessageW(state->enableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                trackSetEqEnabled(trackId, enabled);
                eqWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
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

            auto handleSlider = [&](HWND slider, auto setter)
            {
                if (control != slider)
                    return false;
                int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
                float gain = std::clamp(static_cast<float>(pos) / 10.0f + kMixerEqMin, kMixerEqMin, kMixerEqMax);
                setter(trackId, gain);
                eqWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return true;
            };

            if (handleSlider(state->lowSlider, trackSetEqLowGain))
                return 0;
            if (handleSlider(state->midSlider, trackSetEqMidGain))
                return 0;
            if (handleSlider(state->highSlider, trackSetEqHighGain))
                return 0;
        }
        return 0;
    case WM_EQ_SET_TRACK:
        if (state)
        {
            state->trackId = static_cast<int>(wParam);
            eqWindowSyncControls(hwnd, state);
        }
        return 0;
    case WM_EQ_REFRESH_VALUES:
        if (state)
            eqWindowSyncControls(hwnd, state);
        return 0;
    case WM_DESTROY:
    {
        auto* toDelete = state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        delete toDelete;
        if (hwnd == gEqWindow)
        {
            gEqWindow = nullptr;
            requestMainMenuRefresh();
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureEqWindowClass()
{
    if (gEqWindowClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = EqWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"KJEqWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
        gEqWindowClassRegistered = true;
}

void openEqWindow(HWND parent, int trackId)
{
    ensureEqWindowClass();
    if (!gEqWindowClassRegistered)
        return;

    if (gEqWindow && IsWindow(gEqWindow))
    {
        ShowWindow(gEqWindow, SW_SHOWNORMAL);
        SetForegroundWindow(gEqWindow);
        PostMessageW(gEqWindow, WM_EQ_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        return;
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parent && IsWindow(parent))
    {
        RECT rect {0, 0, 0, 0};
        GetWindowRect(parent, &rect);
        x = rect.right + 20;
        y = rect.top + 20;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                L"KJEqWindow",
                                L"Track Equalizer",
                                WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME,
                                x,
                                y,
                                360,
                                260,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gEqWindow = hwnd;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_EQ_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        requestMainMenuRefresh();
    }
}

void notifyEqWindowTrackChanged(int trackId)
{
    if (gEqWindow && IsWindow(gEqWindow))
        PostMessageW(gEqWindow, WM_EQ_SET_TRACK, static_cast<WPARAM>(trackId), 0);
}

void notifyEqWindowValuesChanged(int trackId)
{
    if (gEqWindow && IsWindow(gEqWindow))
    {
        EqWindowState* state = getEqWindowState(gEqWindow);
        if (!state || state->trackId == trackId)
            PostMessageW(gEqWindow, WM_EQ_REFRESH_VALUES, 0, 0);
    }
}

struct DelayWindowState
{
    int trackId = 0;
    HWND trackLabel = nullptr;
    HWND enableCheckbox = nullptr;
    HWND timeSlider = nullptr;
    HWND timeValueLabel = nullptr;
    HWND feedbackSlider = nullptr;
    HWND feedbackValueLabel = nullptr;
    HWND mixSlider = nullptr;
    HWND mixValueLabel = nullptr;
};

DelayWindowState* getDelayWindowState(HWND hwnd)
{
    return reinterpret_cast<DelayWindowState*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
}

void delayWindowApplyFont(const DelayWindowState& state, HFONT font)
{
    const HWND controls[] = {
        state.trackLabel,
        state.enableCheckbox,
        state.timeSlider,
        state.timeValueLabel,
        state.feedbackSlider,
        state.feedbackValueLabel,
        state.mixSlider,
        state.mixValueLabel,
    };
    for (HWND control : controls)
    {
        if (control)
            SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

void delayWindowLayout(HWND hwnd, DelayWindowState* state, int width, int height)
{
    if (!state)
        return;

    const int padding = 12;
    const int headerHeight = 24;
    const int sliderHeight = 32;
    const int controlSpacing = 12;
    const int valueLabelWidth = 100;

    int currentY = padding;

    if (state->trackLabel)
    {
        MoveWindow(state->trackLabel, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    if (state->enableCheckbox)
    {
        MoveWindow(state->enableCheckbox, padding, currentY, width - padding * 2, headerHeight, TRUE);
        currentY += headerHeight + controlSpacing;
    }

    auto layoutSlider = [&](HWND slider, HWND valueLabel)
    {
        if (!slider)
            return;
        int sliderWidth = std::max(100, width - padding * 3 - valueLabelWidth);
        MoveWindow(slider, padding, currentY, sliderWidth, sliderHeight, TRUE);
        if (valueLabel)
        {
            MoveWindow(valueLabel,
                       padding + sliderWidth + padding,
                       currentY,
                       valueLabelWidth,
                       sliderHeight,
                       TRUE);
        }
        currentY += sliderHeight + controlSpacing;
    };

    layoutSlider(state->timeSlider, state->timeValueLabel);
    layoutSlider(state->feedbackSlider, state->feedbackValueLabel);
    layoutSlider(state->mixSlider, state->mixValueLabel);
}

void delayWindowSyncControls(HWND hwnd, DelayWindowState* state)
{
    if (!state)
        return;

    int trackId = state->trackId;
    if (trackId <= 0)
    {
        if (state->trackLabel)
            SetWindowTextW(state->trackLabel, L"No track selected");
        if (state->enableCheckbox)
            EnableWindow(state->enableCheckbox, FALSE);
        const HWND sliders[] = {state->timeSlider, state->feedbackSlider, state->mixSlider};
        const HWND labels[] = {state->timeValueLabel, state->feedbackValueLabel, state->mixValueLabel};
        for (HWND slider : sliders)
        {
            if (slider)
            {
                EnableWindow(slider, FALSE);
                SendMessageW(slider, TBM_SETPOS, TRUE, 0);
            }
        }
        for (HWND label : labels)
        {
            if (label)
                SetWindowTextW(label, L"-");
        }
        return;
    }

    Track fallbackTrack {};
    const Track* trackPtr = nullptr;
    auto tracks = getTracks();
    trackPtr = findTrackById(tracks, trackId);
    if (!trackPtr)
    {
        fallbackTrack.id = trackId;
        fallbackTrack.name = "Track " + std::to_string(trackId);
        fallbackTrack.delayEnabled = trackGetDelayEnabled(trackId);
        fallbackTrack.delayTimeMs = trackGetDelayTimeMs(trackId);
        fallbackTrack.delayFeedback = trackGetDelayFeedback(trackId);
        fallbackTrack.delayMix = trackGetDelayMix(trackId);
        trackPtr = &fallbackTrack;
    }

    std::wstring labelText = ToWideString(trackPtr->name);
    if (labelText.empty())
        labelText = L"Unnamed Track";
    labelText += L" - Delay";
    if (state->trackLabel)
        SetWindowTextW(state->trackLabel, labelText.c_str());

    bool delayEnabled = trackPtr->delayEnabled;
    if (state->enableCheckbox)
    {
        EnableWindow(state->enableCheckbox, TRUE);
        SendMessageW(state->enableCheckbox, BM_SETCHECK, delayEnabled ? BST_CHECKED : BST_UNCHECKED, 0);
    }

    if (state->timeSlider)
    {
        EnableWindow(state->timeSlider, delayEnabled ? TRUE : FALSE);
        int pos = static_cast<int>(std::lround(std::clamp(trackPtr->delayTimeMs, kMixerDelayTimeMin, kMixerDelayTimeMax)));
        SendMessageW(state->timeSlider, TBM_SETPOS, TRUE, pos);
        if (state->timeValueLabel)
        {
            std::wstring text = ToWideString(formatDelayTimeValue(trackPtr->delayTimeMs));
            SetWindowTextW(state->timeValueLabel, text.c_str());
        }
    }

    auto syncPercentSlider = [&](HWND slider, HWND valueLabel, float value)
    {
        if (!slider)
            return;
        EnableWindow(slider, delayEnabled ? TRUE : FALSE);
        float clamped = std::clamp(value, 0.0f, 1.0f);
        int pos = static_cast<int>(std::lround(clamped * 100.0f));
        SendMessageW(slider, TBM_SETPOS, TRUE, pos);
        if (valueLabel)
        {
            std::wstring text = ToWideString(formatDelayPercentValue(clamped));
            SetWindowTextW(valueLabel, text.c_str());
        }
    };

    syncPercentSlider(state->feedbackSlider, state->feedbackValueLabel, trackPtr->delayFeedback);
    syncPercentSlider(state->mixSlider, state->mixValueLabel, trackPtr->delayMix);
}

LRESULT CALLBACK DelayWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    DelayWindowState* state = getDelayWindowState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* create = reinterpret_cast<LPCREATESTRUCT>(lParam);
        auto* newState = new DelayWindowState();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

        HINSTANCE instance = create->hInstance;
        DWORD staticStyle = WS_CHILD | WS_VISIBLE;

        newState->trackLabel = CreateWindowExW(0,
                                               L"STATIC",
                                               L"Delay",
                                               staticStyle,
                                               0,
                                               0,
                                               100,
                                               20,
                                               hwnd,
                                               nullptr,
                                               instance,
                                               nullptr);
        newState->enableCheckbox = CreateWindowExW(0,
                                                   L"BUTTON",
                                                   L"Enable Delay",
                                                   staticStyle | BS_AUTOCHECKBOX | WS_TABSTOP,
                                                   0,
                                                   0,
                                                   150,
                                                   24,
                                                   hwnd,
                                                   nullptr,
                                                   instance,
                                                   nullptr);

        auto createSlider = [&](HWND& slider, HWND& valueLabel, int rangeMin, int rangeMax, int ticFreq)
        {
            slider = CreateWindowExW(0,
                                      TRACKBAR_CLASSW,
                                      L"",
                                      WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                      0,
                                      0,
                                      100,
                                      30,
                                      hwnd,
                                      nullptr,
                                      instance,
                                      nullptr);
            SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
            SendMessageW(slider, TBM_SETTICFREQ, ticFreq, 0);
            valueLabel = CreateWindowExW(0,
                                         L"STATIC",
                                         L"-",
                                         staticStyle | SS_RIGHT,
                                         0,
                                         0,
                                         80,
                                         20,
                                         hwnd,
                                         nullptr,
                                         instance,
                                         nullptr);
        };

        createSlider(newState->timeSlider, newState->timeValueLabel, static_cast<int>(kMixerDelayTimeMin), static_cast<int>(kMixerDelayTimeMax), 100);
        createSlider(newState->feedbackSlider, newState->feedbackValueLabel, 0, 100, 10);
        createSlider(newState->mixSlider, newState->mixValueLabel, 0, 100, 10);

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        delayWindowApplyFont(*newState, font);

        RECT client {0, 0, 0, 0};
        GetClientRect(hwnd, &client);
        delayWindowLayout(hwnd, newState, client.right - client.left, client.bottom - client.top);
        delayWindowSyncControls(hwnd, newState);
        return 0;
    }
    case WM_SIZE:
        if (state)
            delayWindowLayout(hwnd, state, LOWORD(lParam), HIWORD(lParam));
        return 0;
    case WM_COMMAND:
        if (state && reinterpret_cast<HWND>(lParam) == state->enableCheckbox && HIWORD(wParam) == BN_CLICKED)
        {
            int trackId = state->trackId;
            if (trackId > 0)
            {
                bool enabled = SendMessageW(state->enableCheckbox, BM_GETCHECK, 0, 0) == BST_CHECKED;
                trackSetDelayEnabled(trackId, enabled);
                delayWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
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

            if (control == state->timeSlider)
            {
                int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
                float newValue = std::clamp(static_cast<float>(pos), kMixerDelayTimeMin, kMixerDelayTimeMax);
                trackSetDelayTimeMs(trackId, newValue);
                delayWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return 0;
            }

            auto handlePercentSlider = [&](HWND slider, auto setter, float minValue, float maxValue)
            {
                if (control != slider)
                    return false;
                int pos = static_cast<int>(SendMessageW(slider, TBM_GETPOS, 0, 0));
                float value = std::clamp(static_cast<float>(pos) / 100.0f, minValue, maxValue);
                setter(trackId, value);
                delayWindowSyncControls(hwnd, state);
                notifyEffectsWindowTrackValuesChanged(trackId);
                if (gMainWindow && IsWindow(gMainWindow))
                    InvalidateRect(gMainWindow, nullptr, FALSE);
                return true;
            };

            if (handlePercentSlider(state->feedbackSlider, trackSetDelayFeedback, kMixerDelayFeedbackMin, kMixerDelayFeedbackMax))
                return 0;
            if (handlePercentSlider(state->mixSlider, trackSetDelayMix, kMixerDelayMixMin, kMixerDelayMixMax))
                return 0;
        }
        return 0;
    case WM_DELAY_SET_TRACK:
        if (state)
        {
            state->trackId = static_cast<int>(wParam);
            delayWindowSyncControls(hwnd, state);
        }
        return 0;
    case WM_DELAY_REFRESH_VALUES:
        if (state)
            delayWindowSyncControls(hwnd, state);
        return 0;
    case WM_DESTROY:
    {
        auto* toDelete = state;
        SetWindowLongPtr(hwnd, GWLP_USERDATA, 0);
        delete toDelete;
        if (hwnd == gDelayWindow)
        {
            gDelayWindow = nullptr;
            requestMainMenuRefresh();
        }
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ensureDelayWindowClass()
{
    if (gDelayWindowClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = DelayWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = L"KJDelayWindow";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
        gDelayWindowClassRegistered = true;
}

void openDelayWindow(HWND parent, int trackId)
{
    ensureDelayWindowClass();
    if (!gDelayWindowClassRegistered)
        return;

    if (gDelayWindow && IsWindow(gDelayWindow))
    {
        ShowWindow(gDelayWindow, SW_SHOWNORMAL);
        SetForegroundWindow(gDelayWindow);
        PostMessageW(gDelayWindow, WM_DELAY_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        return;
    }

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parent && IsWindow(parent))
    {
        RECT rect {0, 0, 0, 0};
        GetWindowRect(parent, &rect);
        x = rect.right + 20;
        y = rect.top + 20;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                L"KJDelayWindow",
                                L"Track Delay",
                                WS_OVERLAPPEDWINDOW ^ WS_THICKFRAME,
                                x,
                                y,
                                380,
                                280,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gDelayWindow = hwnd;
        ShowWindow(hwnd, SW_SHOWNORMAL);
        UpdateWindow(hwnd);
        PostMessageW(hwnd, WM_DELAY_SET_TRACK, static_cast<WPARAM>(trackId), 0);
        requestMainMenuRefresh();
    }
}

void notifyDelayWindowTrackChanged(int trackId)
{
    if (gDelayWindow && IsWindow(gDelayWindow))
        PostMessageW(gDelayWindow, WM_DELAY_SET_TRACK, static_cast<WPARAM>(trackId), 0);
}

void notifyDelayWindowValuesChanged(int trackId)
{
    if (gDelayWindow && IsWindow(gDelayWindow))
    {
        DelayWindowState* state = getDelayWindowState(gDelayWindow);
        if (!state || state->trackId == trackId)
            PostMessageW(gDelayWindow, WM_DELAY_REFRESH_VALUES, 0, 0);
    }
}

void closeEffectsWindow()
{
    if (gEffectsWindow && IsWindow(gEffectsWindow))
    {
        DestroyWindow(gEffectsWindow);
        gEffectsWindow = nullptr;
        requestMainMenuRefresh();
    }
}

LRESULT CALLBACK EffectsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

void ensureEffectsWindowClass()
{
    if (gEffectsWindowClassRegistered)
        return;

    WNDCLASSW wc = {0};
    wc.lpfnWndProc = EffectsWndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kEffectsWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    if (RegisterClassW(&wc))
    {
        gEffectsWindowClassRegistered = true;
    }
}

void toggleEffectsWindow(HWND parent)
{
    if (gEffectsWindow && IsWindow(gEffectsWindow))
    {
        closeEffectsWindow();
        if (gMainWindow)
        {
            InvalidateRect(gMainWindow, nullptr, FALSE);
        }
        return;
    }

    ensureEffectsWindowClass();
    if (!gEffectsWindowClassRegistered)
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
        x = parentRect.left + 60;
        y = parentRect.top + 60;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kEffectsWindowClassName,
                                L"Track Effects",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kEffectsWindowWidth,
                                kEffectsWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gEffectsWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        requestMainMenuRefresh();
    }
}

void notifyEffectsWindowTrackListChanged()
{
    if (gEffectsWindow && IsWindow(gEffectsWindow))
    {
        PostMessageW(gEffectsWindow, WM_EFFECTS_RELOAD_TRACKS, 0, 0);
    }
    int activeTrack = getActiveSequencerTrackId();
    notifyCompressorWindowTrackChanged(activeTrack);
    notifyCompressorWindowValuesChanged(activeTrack);
    notifySidechainWindowTrackListChanged();
    notifySidechainWindowTrackChanged(activeTrack);
    notifyEqWindowTrackChanged(activeTrack);
    notifyDelayWindowTrackChanged(activeTrack);
}

void notifyEffectsWindowActiveTrackChanged(int trackId)
{
    if (gEffectsWindow && IsWindow(gEffectsWindow))
    {
        PostMessageW(gEffectsWindow, WM_EFFECTS_SELECT_TRACK, static_cast<WPARAM>(trackId), 0);
    }
    notifyCompressorWindowTrackChanged(trackId);
    notifySidechainWindowTrackChanged(trackId);
    notifyEqWindowTrackChanged(trackId);
    notifyDelayWindowTrackChanged(trackId);
}

LRESULT CALLBACK EffectsWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    EffectsWindowState* state = getEffectsWindowState(hwnd);

    switch (msg)
    {
    case WM_CREATE:
    {
        auto* newState = new EffectsWindowState();
        newState->selectedTrackId = selectedTrackId > 0 ? selectedTrackId : getActiveSequencerTrackId();
        SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

        HINSTANCE instance = GetModuleHandle(nullptr);
        DWORD listStyle = WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS;
        newState->trackList = CreateWindowExW(WS_EX_CLIENTEDGE,
                                              L"LISTBOX",
                                              L"",
                                              listStyle,
                                              0,
                                              0,
                                              100,
                                              100,
                                              hwnd,
                                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEffectsTrackListId)),
                                              instance,
                                              nullptr);

        DWORD staticStyle = WS_CHILD | WS_VISIBLE;
        newState->trackLabel = CreateWindowExW(0, L"STATIC", L"", staticStyle, 0, 0, 100, 20, hwnd, nullptr, instance, nullptr);

        DWORD headerStyle = WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON;

        auto createCollapsibleSlider = [&](const wchar_t* labelText,
                                           INT_PTR toggleId,
                                           INT_PTR sliderId,
                                           HWND& headerOut,
                                           HWND& sliderOut,
                                           HWND& valueOut,
                                           bool& expandedFlag,
                                           int rangeMin,
                                           int rangeMax,
                                           int ticFreq)
        {
            headerOut = CreateWindowExW(0,
                                         L"BUTTON",
                                         L"",
                                         headerStyle,
                                         0,
                                         0,
                                         100,
                                         20,
                                         hwnd,
                                         reinterpret_cast<HMENU>(toggleId),
                                         instance,
                                         nullptr);
            valueOut = CreateWindowExW(0,
                                       L"STATIC",
                                       L"-",
                                       staticStyle | SS_RIGHT,
                                       0,
                                       0,
                                       80,
                                       20,
                                       hwnd,
                                       nullptr,
                                       instance,
                                       nullptr);
            sliderOut = CreateWindowExW(0,
                                        TRACKBAR_CLASSW,
                                        L"",
                                        WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                        0,
                                        0,
                                        100,
                                        30,
                                        hwnd,
                                        reinterpret_cast<HMENU>(sliderId),
                                        instance,
                                        nullptr);
            SendMessageW(sliderOut, TBM_SETRANGE, TRUE, MAKELPARAM(rangeMin, rangeMax));
            SendMessageW(sliderOut, TBM_SETTICFREQ, ticFreq, 0);
            expandedFlag = true;
            effectsWindowUpdateSectionHeader(headerOut, labelText, expandedFlag);
        };

        createCollapsibleSlider(L"Volume",
                                kEffectsVolumeToggleId,
                                kEffectsVolumeSliderId,
                                newState->volumeHeader,
                                newState->volumeSlider,
                                newState->volumeValueLabel,
                                newState->volumeExpanded,
                                0,
                                100,
                                10);
        createCollapsibleSlider(L"Pan",
                                kEffectsPanToggleId,
                                kEffectsPanSliderId,
                                newState->panHeader,
                                newState->panSlider,
                                newState->panValueLabel,
                                newState->panExpanded,
                                0,
                                200,
                                20);

        newState->effectList = CreateWindowExW(WS_EX_CLIENTEDGE,
                                               WC_LISTVIEW,
                                               L"",
                                               WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                               0,
                                               0,
                                               100,
                                               100,
                                               hwnd,
                                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(kEffectsListViewId)),
                                               instance,
                                               nullptr);

        if (newState->effectList)
        {
            ListView_SetExtendedListViewStyle(newState->effectList,
                                              LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

            LVCOLUMNW column {0};
            column.mask = LVCF_FMT | LVCF_WIDTH | LVCF_TEXT;
            column.fmt = LVCFMT_LEFT;
            column.cx = 200;
            column.pszText = const_cast<LPWSTR>(L"Effect");
            ListView_InsertColumn(newState->effectList, 0, &column);

            column.fmt = LVCFMT_CENTER;
            column.cx = 100;
            column.pszText = const_cast<LPWSTR>(L"Container");
            ListView_InsertColumn(newState->effectList, 1, &column);

            newState->effectEntries = {
                {EffectListItemType::Eq, L"Equalizer"},
                {EffectListItemType::Delay, L"Delay"},
                {EffectListItemType::Compressor, L"Compressor"},
                {EffectListItemType::Sidechain, L"Sidechain"},
            };

            LVITEMW item {0};
            item.mask = LVIF_TEXT | LVIF_PARAM;
            for (size_t i = 0; i < newState->effectEntries.size(); ++i)
            {
                const auto& entry = newState->effectEntries[i];
                item.iItem = static_cast<int>(i);
                item.iSubItem = 0;
                item.pszText = const_cast<LPWSTR>(entry.name.c_str());
                item.lParam = static_cast<LPARAM>(static_cast<int>(entry.type));
                int index = ListView_InsertItem(newState->effectList, &item);
                if (index >= 0)
                {
                    const wchar_t* containerText = L"";
                    switch (entry.type)
                    {
                    case EffectListItemType::Eq:
                    case EffectListItemType::Delay:
                    case EffectListItemType::Compressor:
                    case EffectListItemType::Sidechain:
                        containerText = L"Open";
                        break;
                    }
                    ListView_SetItemText(newState->effectList, index, 1, const_cast<LPWSTR>(containerText));
                }
            }
        }

        HFONT font = static_cast<HFONT>(GetStockObject(DEFAULT_GUI_FONT));
        effectsWindowApplyFont(*newState, font);

        RECT client {0, 0, 0, 0};
        GetClientRect(hwnd, &client);
        effectsWindowLayout(hwnd, newState, client.right - client.left, client.bottom - client.top);
        effectsWindowPopulateTrackList(hwnd, newState);
        effectsWindowSyncControls(hwnd, newState);
        effectsWindowEnsureSelectionVisible(hwnd, newState);
        return 0;
    }
    case WM_SIZE:
    {
        if (state)
        {
            effectsWindowLayout(hwnd, state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;
    }
    case WM_COMMAND:
    {
        if (!state)
            break;

        auto handleToggle = [&](int controlId, HWND header, const wchar_t* labelText, bool& expanded, HWND slider)
        {
            if (LOWORD(wParam) != controlId || HIWORD(wParam) != BN_CLICKED)
                return false;

            expanded = !expanded;
            effectsWindowUpdateSectionHeader(header, labelText, expanded);
            if (slider)
                ShowWindow(slider, expanded ? SW_SHOW : SW_HIDE);

            RECT client {0, 0, 0, 0};
            GetClientRect(hwnd, &client);
            effectsWindowLayout(hwnd, state, client.right - client.left, client.bottom - client.top);
            return true;
        };

        if (handleToggle(kEffectsVolumeToggleId, state->volumeHeader, L"Volume", state->volumeExpanded, state->volumeSlider))
            return 0;
        if (handleToggle(kEffectsPanToggleId, state->panHeader, L"Pan", state->panExpanded, state->panSlider))
            return 0;

        if (LOWORD(wParam) == kEffectsTrackListId && HIWORD(wParam) == LBN_SELCHANGE && state->trackList)
        {
            int selection = static_cast<int>(SendMessageW(state->trackList, LB_GETCURSEL, 0, 0));
            if (selection != LB_ERR)
            {
                int trackId = static_cast<int>(SendMessageW(state->trackList, LB_GETITEMDATA, selection, 0));
                if (trackId > 0)
                {
                    state->selectedTrackId = trackId;
                    effectsWindowSyncControls(hwnd, state);
                    notifyCompressorWindowTrackChanged(trackId);
                    notifyEqWindowTrackChanged(trackId);
                    notifyDelayWindowTrackChanged(trackId);
                    if (selectedTrackId != trackId)
                    {
                        selectedTrackId = trackId;
                        setActiveSequencerTrackId(trackId);
                        currentStepPage = 0;
                        if (gMainWindow && IsWindow(gMainWindow))
                        {
                            InvalidateRect(gMainWindow, nullptr, FALSE);
                        }
                        invalidatePianoRollWindow();
                    }
                }
            }
            return 0;
        }
        break;
    }
    case WM_HSCROLL:
    {
        if (!state)
            break;

        HWND control = reinterpret_cast<HWND>(lParam);
        if (!control)
            control = GetFocus();

        if (!control || state->selectedTrackId <= 0)
            return 0;

        int trackId = state->selectedTrackId;

        if (control == state->volumeSlider)
        {
            int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            float newVolume = std::clamp(pos / 100.0f, kMixerVolumeMin, kMixerVolumeMax);
            trackSetVolume(trackId, newVolume);
            effectsWindowSyncControls(hwnd, state);
            if (gMainWindow && IsWindow(gMainWindow))
                InvalidateRect(gMainWindow, nullptr, FALSE);
            return 0;
        }

        if (control == state->panSlider)
        {
            int pos = static_cast<int>(SendMessageW(control, TBM_GETPOS, 0, 0));
            float newPan = std::clamp(static_cast<float>(pos) / 100.0f + kMixerPanMin, kMixerPanMin, kMixerPanMax);
            trackSetPan(trackId, newPan);
            effectsWindowSyncControls(hwnd, state);
            if (gMainWindow && IsWindow(gMainWindow))
                InvalidateRect(gMainWindow, nullptr, FALSE);
            return 0;
        }

        return 0;
    }
    case WM_NOTIFY:
    {
        if (!state)
            break;

        auto* header = reinterpret_cast<LPNMHDR>(lParam);
        if (header && header->idFrom == kEffectsListViewId && state->effectList && header->hwndFrom == state->effectList)
        {
            if (header->code == LVN_ITEMCHANGED)
            {
                auto* info = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if (!state->effectListUpdating && info && info->iItem >= 0 &&
                    static_cast<size_t>(info->iItem) < state->effectEntries.size() &&
                    (info->uChanged & LVIF_STATE) != 0)
                {
                    int trackId = state->selectedTrackId;
                    if (trackId > 0)
                    {
                        bool checked = ListView_GetCheckState(state->effectList, info->iItem) != FALSE;
                        switch (state->effectEntries[info->iItem].type)
                        {
                        case EffectListItemType::Eq:
                            trackSetEqEnabled(trackId, checked);
                            notifyEqWindowValuesChanged(trackId);
                            break;
                        case EffectListItemType::Delay:
                            trackSetDelayEnabled(trackId, checked);
                            notifyDelayWindowValuesChanged(trackId);
                            break;
                        case EffectListItemType::Compressor:
                            trackSetCompressorEnabled(trackId, checked);
                            notifyCompressorWindowValuesChanged(trackId);
                            break;
                        case EffectListItemType::Sidechain:
                            trackSetSidechainEnabled(trackId, checked);
                            notifySidechainWindowValuesChanged(trackId);
                            break;
                        }
                        effectsWindowSyncControls(hwnd, state);
                        if (gMainWindow && IsWindow(gMainWindow))
                            InvalidateRect(gMainWindow, nullptr, FALSE);
                    }
                }
                return 0;
            }

            if (header->code == NM_CLICK)
            {
                auto* click = reinterpret_cast<LPNMITEMACTIVATE>(lParam);
                if (click && click->iItem >= 0 && click->iSubItem == 1 &&
                    static_cast<size_t>(click->iItem) < state->effectEntries.size())
                {
                    int trackId = state->selectedTrackId;
                    if (trackId > 0)
                    {
                        switch (state->effectEntries[click->iItem].type)
                        {
                        case EffectListItemType::Eq:
                            openEqWindow(hwnd, trackId);
                            break;
                        case EffectListItemType::Delay:
                            openDelayWindow(hwnd, trackId);
                            break;
                        case EffectListItemType::Compressor:
                            openCompressorWindow(hwnd, trackId);
                            break;
                        case EffectListItemType::Sidechain:
                            openSidechainWindow(hwnd, trackId);
                            break;
                        }
                    }
                }
                return 0;
            }
        }
        break;
    }
    case WM_EFFECTS_REFRESH_VALUES:
    {
        if (state)
        {
            int trackId = static_cast<int>(wParam);
            if (trackId == 0 || trackId == state->selectedTrackId)
            {
                effectsWindowSyncControls(hwnd, state);
            }
        }
        return 0;
    }
    case WM_EFFECTS_RELOAD_TRACKS:
    {
        if (state)
        {
            effectsWindowPopulateTrackList(hwnd, state);
            effectsWindowEnsureSelectionVisible(hwnd, state);
            effectsWindowSyncControls(hwnd, state);
        }
        return 0;
    }
    case WM_EFFECTS_SELECT_TRACK:
    {
        if (state)
        {
            int trackId = static_cast<int>(wParam);
            state->selectedTrackId = trackId;
            effectsWindowPopulateTrackList(hwnd, state);
            effectsWindowEnsureSelectionVisible(hwnd, state);
            effectsWindowSyncControls(hwnd, state);
        }
        return 0;
    }
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
        if (hwnd == gEffectsWindow)
        {
            gEffectsWindow = nullptr;
            if (gMainWindow && IsWindow(gMainWindow))
            {
                InvalidateRect(gMainWindow, nullptr, FALSE);
            }
        }
        requestMainMenuRefresh();
        return 0;
    }
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
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

void beginSliderDrag(HWND hwnd, SliderDragTarget target, int trackId)
{
    gSliderDrag.target = target;
    gSliderDrag.trackId = trackId;
    if (hwnd && GetCapture() != hwnd)
    {
        SetCapture(hwnd);
    }
}

void endSliderDrag(HWND hwnd)
{
    if (gSliderDrag.target == SliderDragTarget::None)
        return;

    gSliderDrag.target = SliderDragTarget::None;
    gSliderDrag.trackId = 0;
    if (hwnd && GetCapture() == hwnd)
    {
        ReleaseCapture();
    }
}

void updateSliderDrag(HWND hwnd, int x)
{
    if (gSliderDrag.target == SliderDragTarget::None)
        return;

    int trackId = gSliderDrag.trackId;
    if (trackId <= 0)
    {
        endSliderDrag(hwnd);
        return;
    }

    auto applySliderChange = [&](const SliderControlRects& slider, float minValue, float maxValue, auto&& setter) {
        if (slider.track.right <= slider.track.left)
        {
            endSliderDrag(hwnd);
            return false;
        }

        float newValue = sliderValueFromPosition(slider, x, minValue, maxValue);
        setter(newValue);
        InvalidateRect(hwnd, nullptr, FALSE);
        return true;
    };

    switch (gSliderDrag.target)
    {
    case SliderDragTarget::SynthFormant:
        applySliderChange(gSynthFormantSliderControl, kSynthFormantMin, kSynthFormantMax,
                          [trackId](float value) { trackSetSynthFormant(trackId, value); });
        break;
    case SliderDragTarget::SynthFeedback:
        applySliderChange(gSynthFeedbackSliderControl, kSynthFeedbackMin, kSynthFeedbackMax,
                          [trackId](float value) { trackSetSynthFeedback(trackId, value); });
        break;
    case SliderDragTarget::SynthPitch:
        applySliderChange(gSynthPitchSliderControl, kSynthPitchMin, kSynthPitchMax,
                          [trackId](float value) { trackSetSynthPitch(trackId, value); });
        break;
    case SliderDragTarget::SynthPitchRange:
        applySliderChange(gSynthPitchRangeSliderControl, kSynthPitchRangeMin, kSynthPitchRangeMax,
                          [trackId](float value) { trackSetSynthPitchRange(trackId, value); });
        break;
    case SliderDragTarget::SynthAttack:
        applySliderChange(gSynthAttackSliderControl, kSynthAttackMin, kSynthAttackMax,
                          [trackId](float value) { trackSetSynthAttack(trackId, value); });
        break;
    case SliderDragTarget::SynthDecay:
        applySliderChange(gSynthDecaySliderControl, kSynthDecayMin, kSynthDecayMax,
                          [trackId](float value) { trackSetSynthDecay(trackId, value); });
        break;
    case SliderDragTarget::SynthSustain:
        applySliderChange(gSynthSustainSliderControl, kSynthSustainMin, kSynthSustainMax,
                          [trackId](float value) { trackSetSynthSustain(trackId, value); });
        break;
    case SliderDragTarget::SynthRelease:
        applySliderChange(gSynthReleaseSliderControl, kSynthReleaseMin, kSynthReleaseMax,
                          [trackId](float value) { trackSetSynthRelease(trackId, value); });
        break;
    case SliderDragTarget::SampleAttack:
        applySliderChange(gSampleAttackSliderControl, kSampleAttackMin, kSampleAttackMax,
                          [trackId](float value) { trackSetSampleAttack(trackId, value); });
        break;
    case SliderDragTarget::SampleRelease:
        applySliderChange(gSampleReleaseSliderControl, kSampleReleaseMin, kSampleReleaseMax,
                          [trackId](float value) { trackSetSampleRelease(trackId, value); });
        break;
    case SliderDragTarget::None:
    default:
        break;
    }
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

    int spacing = std::min(6, std::max(2, height / 10));
    int labelHeight = std::min(18, std::max(12, height / 4));
    int valueHeight = std::min(18, std::max(12, height / 4));
    int minimumTrackHeight = 8;

    int available = height - labelHeight - valueHeight - spacing * 2;
    if (available < minimumTrackHeight)
    {
        int deficit = minimumTrackHeight - available;
        int labelReduction = std::min(deficit / 2, std::max(labelHeight - 10, 0));
        labelHeight -= labelReduction;
        deficit -= labelReduction;
        int valueReduction = std::min(deficit, std::max(valueHeight - 10, 0));
        valueHeight -= valueReduction;
        available = height - labelHeight - valueHeight - spacing * 2;
        if (available < minimumTrackHeight)
            available = minimumTrackHeight;
    }

    RECT labelRect = area;
    labelRect.bottom = std::min(labelRect.top + labelHeight, area.bottom);
    drawText(surface, labelRect, label, RGB(220, 220, 220), DT_LEFT | DT_VCENTER | DT_SINGLELINE);

    RECT valueRect = area;
    valueRect.top = std::max<LONG>(valueRect.bottom - valueHeight, area.top);
    drawText(surface, valueRect, valueText.c_str(), RGB(200, 200, 200), DT_RIGHT | DT_VCENTER | DT_SINGLELINE);

    RECT trackRect = area;
    trackRect.top = std::min<LONG>(labelRect.bottom + spacing, area.bottom);
    trackRect.bottom = std::max<LONG>(valueRect.top - spacing, trackRect.top + minimumTrackHeight);

    int horizontalPadding = std::min(10, std::max(4, width / 6));
    trackRect.left += horizontalPadding;
    trackRect.right -= horizontalPadding;
    if (trackRect.right <= trackRect.left)
    {
        int mid = (trackRect.left + trackRect.right) / 2;
        trackRect.left = mid - 10;
        trackRect.right = mid + 10;
    }

    int trackHeight = std::max(static_cast<int>(trackRect.bottom - trackRect.top), minimumTrackHeight);
    int trackWidth = std::max(static_cast<int>(trackRect.right - trackRect.left), 1);
    trackRect.bottom = trackRect.top + trackHeight;
    LICE_FillRect(&surface, trackRect.left, trackRect.top, trackWidth, trackHeight, LICE_ColorFromCOLORREF(RGB(55, 55, 55)));
    LICE_DrawRect(&surface, trackRect.left, trackRect.top, trackWidth, trackHeight, LICE_ColorFromCOLORREF(RGB(90, 90, 90)));

    double clampedNorm = std::clamp(normalizedValue, 0.0, 1.0);
    int handleWidth = 12;
    int handleRange = std::max(trackWidth - handleWidth, 1);
    int handleX = trackRect.left + static_cast<int>(std::round(clampedNorm * handleRange));
    RECT handleRect {handleX, trackRect.top - 4, handleX + handleWidth, trackRect.bottom + 4};
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

void drawSynthTrackControls(LICE_SysBitmap& surface, const RECT& client, const Track* activeTrack)
{
    gSynthFormantSliderControl = {};
    gSynthFeedbackSliderControl = {};
    gSynthPitchSliderControl = {};
    gSynthPitchRangeSliderControl = {};
    gSynthAttackSliderControl = {};
    gSynthDecaySliderControl = {};
    gSynthSustainSliderControl = {};
    gSynthReleaseSliderControl = {};

    if (!activeTrack || activeTrack->type != TrackType::Synth)
        return;

    const RECT& firstStep = stepRects.front();
    const RECT& lastStep = stepRects.back();
    int areaLeft = firstStep.left;
    int areaRight = lastStep.right;
    if (areaRight <= areaLeft)
        return;

    int topSpacing = 12;
    int rowSpacing = 12;
    int rows = 2;
    int slidersPerRow = 4;
    int sliderSpacing = 12;
    int sliderHeight = 70;
    int areaTop = firstStep.bottom + topSpacing;
    int areaBottom = areaTop + rows * sliderHeight + (rows - 1) * rowSpacing;
    if (areaBottom > client.bottom)
    {
        areaBottom = client.bottom;
        int availableHeight = areaBottom - areaTop;
        int minimumHeight = rows * 32 + (rows - 1) * rowSpacing;
        if (availableHeight < minimumHeight)
        {
            areaTop = std::max<int>(areaBottom - minimumHeight, static_cast<int>(firstStep.bottom + 2));
            availableHeight = areaBottom - areaTop;
        }
        sliderHeight = (availableHeight - (rows - 1) * rowSpacing) / rows;
        if (sliderHeight <= 0)
            return;
    }
    if (areaBottom <= areaTop)
        return;

    int totalWidth = areaRight - areaLeft;
    int totalSpacing = sliderSpacing * (slidersPerRow - 1);
    int sliderWidth = totalWidth - totalSpacing;
    if (sliderWidth <= 0)
        return;
    sliderWidth /= slidersPerRow;
    if (sliderWidth <= 0)
        return;

    auto makeSliderRect = [&](int row, int index) {
        int left = areaLeft + index * (sliderWidth + sliderSpacing);
        int top = areaTop + row * (sliderHeight + rowSpacing);
        RECT rect {left, top, left + sliderWidth, top + sliderHeight};
        if (rect.right > areaRight)
            rect.right = areaRight;
        if (rect.bottom > areaBottom)
            rect.bottom = areaBottom;
        return rect;
    };

    double formantNorm = computeNormalized(activeTrack->formant, kSynthFormantMin, kSynthFormantMax);
    RECT formantRect = makeSliderRect(0, 0);
    drawSliderControl(surface, gSynthFormantSliderControl, formantRect, formantNorm,
                      "Formant", formatNormalizedValue(activeTrack->formant));

    double feedbackNorm = computeNormalized(activeTrack->feedback, kSynthFeedbackMin, kSynthFeedbackMax);
    RECT feedbackRect = makeSliderRect(0, 1);
    drawSliderControl(surface, gSynthFeedbackSliderControl, feedbackRect, feedbackNorm,
                      "Feedback", formatNormalizedValue(activeTrack->feedback));

    double pitchNorm = computeNormalized(activeTrack->pitch, kSynthPitchMin, kSynthPitchMax);
    RECT pitchRect = makeSliderRect(0, 2);
    drawSliderControl(surface, gSynthPitchSliderControl, pitchRect, pitchNorm,
                      "Pitch", formatPitchValue(activeTrack->pitch));

    double pitchRangeNorm = computeNormalized(activeTrack->pitchRange, kSynthPitchRangeMin, kSynthPitchRangeMax);
    RECT pitchRangeRect = makeSliderRect(0, 3);
    drawSliderControl(surface, gSynthPitchRangeSliderControl, pitchRangeRect, pitchRangeNorm,
                      "Pitch Range", formatPitchRangeValue(activeTrack->pitchRange));

    double attackNorm = computeNormalized(activeTrack->synthAttack, kSynthAttackMin, kSynthAttackMax);
    RECT attackRect = makeSliderRect(1, 0);
    drawSliderControl(surface, gSynthAttackSliderControl, attackRect, attackNorm,
                      "Attack", formatSecondsValue(activeTrack->synthAttack));

    double decayNorm = computeNormalized(activeTrack->synthDecay, kSynthDecayMin, kSynthDecayMax);
    RECT decayRect = makeSliderRect(1, 1);
    drawSliderControl(surface, gSynthDecaySliderControl, decayRect, decayNorm,
                      "Decay", formatSecondsValue(activeTrack->synthDecay));

    double sustainNorm = computeNormalized(activeTrack->synthSustain, kSynthSustainMin, kSynthSustainMax);
    RECT sustainRect = makeSliderRect(1, 2);
    drawSliderControl(surface, gSynthSustainSliderControl, sustainRect, sustainNorm,
                      "Sustain", formatNormalizedValue(activeTrack->synthSustain));

    double releaseNorm = computeNormalized(activeTrack->synthRelease, kSynthReleaseMin, kSynthReleaseMax);
    RECT releaseRect = makeSliderRect(1, 3);
    drawSliderControl(surface, gSynthReleaseSliderControl, releaseRect, releaseNorm,
                      "Release", formatSecondsValue(activeTrack->synthRelease));
}

void drawSampleTrackControls(LICE_SysBitmap& surface, const RECT& client, const Track* activeTrack)
{
    gSampleAttackSliderControl = {};
    gSampleReleaseSliderControl = {};

    if (!activeTrack || activeTrack->type != TrackType::Sample)
        return;

    const RECT& firstStep = stepRects.front();
    const RECT& lastStep = stepRects.back();
    int areaLeft = firstStep.left;
    int areaRight = lastStep.right;
    if (areaRight <= areaLeft)
        return;

    int topSpacing = 12;
    int sliderSpacing = 12;
    int sliderHeight = 70;
    int areaTop = firstStep.bottom + topSpacing;
    int areaBottom = areaTop + sliderHeight;
    if (areaBottom > client.bottom)
    {
        areaBottom = client.bottom;
        if (areaBottom - areaTop < 32)
            areaTop = std::max<int>(areaBottom - 32, static_cast<int>(firstStep.bottom + 2));
    }
    if (areaBottom <= areaTop)
        return;

    int totalWidth = areaRight - areaLeft;
    int sliderCount = 2;
    int totalSpacing = sliderSpacing * (sliderCount - 1);
    int sliderWidth = totalWidth - totalSpacing;
    if (sliderWidth <= 0)
        return;
    sliderWidth /= sliderCount;
    if (sliderWidth <= 0)
        return;

    auto makeSliderRect = [&](int index) {
        int left = areaLeft + index * (sliderWidth + sliderSpacing);
        RECT rect {left, areaTop, left + sliderWidth, areaBottom};
        if (rect.right > areaRight)
            rect.right = areaRight;
        return rect;
    };

    double attackNorm = computeNormalized(activeTrack->sampleAttack, kSampleAttackMin, kSampleAttackMax);
    RECT attackRect = makeSliderRect(0);
    drawSliderControl(surface, gSampleAttackSliderControl, attackRect, attackNorm,
                      "Attack", formatSecondsValue(activeTrack->sampleAttack));

    double releaseNorm = computeNormalized(activeTrack->sampleRelease, kSampleReleaseMin, kSampleReleaseMax);
    RECT releaseRect = makeSliderRect(1);
    drawSliderControl(surface, gSampleReleaseSliderControl, releaseRect, releaseNorm,
                      "Release", formatSecondsValue(activeTrack->sampleRelease));
}

void renderUI(LICE_SysBitmap& surface, const RECT& client)
{
    LICE_Clear(&surface, LICE_ColorFromCOLORREF(RGB(20, 20, 20)));
    gAudioDeviceOptions.clear();
    gWaveOptions.clear();
    gTrackTypeDropdownOptions.clear();

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
        fallbackTrack.formant = trackGetSynthFormant(activeTrackId);
        fallbackTrack.feedback = trackGetSynthFeedback(activeTrackId);
        fallbackTrack.pitch = trackGetSynthPitch(activeTrackId);
        fallbackTrack.pitchRange = trackGetSynthPitchRange(activeTrackId);
        fallbackTrack.synthAttack = trackGetSynthAttack(activeTrackId);
        fallbackTrack.synthDecay = trackGetSynthDecay(activeTrackId);
        fallbackTrack.synthSustain = trackGetSynthSustain(activeTrackId);
        fallbackTrack.synthRelease = trackGetSynthRelease(activeTrackId);
        fallbackTrack.sampleAttack = trackGetSampleAttack(activeTrackId);
        fallbackTrack.sampleRelease = trackGetSampleRelease(activeTrackId);
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
            const int optionCount = static_cast<int>(kSynthWaveOptions.size());
            RECT optionRect = waveSelectButton;
            optionRect.top = computeDropdownStartTop(waveSelectButton, optionCount, kWaveDropdownOptionHeight,
                                                     kWaveDropdownSpacing, client);
            optionRect.bottom = optionRect.top + kWaveDropdownOptionHeight;

            for (SynthWaveType option : kSynthWaveOptions)
            {
                WaveDropdownOption dropdownOption{};
                dropdownOption.rect = optionRect;
                dropdownOption.type = option;
                dropdownOption.isSelected = option == activeWaveType;
                gWaveOptions.push_back(dropdownOption);

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

    bool effectsOpen = gEffectsWindow && IsWindow(gEffectsWindow);
    COLORREF effectsFill = effectsOpen ? RGB(0, 90, 160) : RGB(50, 50, 50);
    COLORREF effectsOutline = effectsOpen ? RGB(20, 20, 20) : RGB(120, 120, 120);
    drawButton(surface, effectsToggleButton, effectsFill, effectsOutline,
               effectsOpen ? "Hide Effects" : "Show Effects");

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
        const int optionCount = 1 + static_cast<int>(gCachedAudioDevices.size());
        RECT optionRect = audioDeviceButton;
        optionRect.top = computeDropdownStartTop(audioDeviceButton, optionCount, kAudioDeviceDropdownOptionHeight,
                                                 kAudioDeviceDropdownSpacing, client);
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
            const int optionCount = static_cast<int>(kTrackTypeOptions.size());
            RECT optionRect = typeRect;
            optionRect.top = computeDropdownStartTop(typeRect, optionCount, kTrackTypeDropdownOptionHeight,
                                                     kTrackTypeDropdownSpacing, client);
            optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
            for (TrackType option : kTrackTypeOptions)
            {
                TrackTypeDropdownOption pending{};
                pending.trackId = track.id;
                pending.type = option;
                pending.rect = optionRect;
                pending.isSelected = (option == track.type);
                gTrackTypeDropdownOptions.push_back(pending);

                optionRect.top = optionRect.bottom + kTrackTypeDropdownSpacing;
                optionRect.bottom = optionRect.top + kTrackTypeDropdownOptionHeight;
            }
        }
    }

    drawSequencer(surface, activeTrackId);

    drawSynthTrackControls(surface, client, activeTrackPtr);
    drawSampleTrackControls(surface, client, activeTrackPtr);

    for (const auto& option : gAudioDeviceOptions)
    {
        COLORREF optionFill = option.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
        COLORREF optionOutline = option.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
        std::string optionLabelUtf8 = FromWideString(option.label);
        if (optionLabelUtf8.empty())
            optionLabelUtf8 = "Audio Device";
        drawButton(surface, option.rect, optionFill, optionOutline, optionLabelUtf8.c_str());
    }

    for (const auto& option : gWaveOptions)
    {
        COLORREF optionFill = option.isSelected ? RGB(0, 120, 200) : RGB(50, 50, 50);
        COLORREF optionOutline = option.isSelected ? RGB(20, 20, 20) : RGB(120, 120, 120);
        std::string optionLabel = synthWaveTypeToString(option.type);
        drawButton(surface, option.rect, optionFill, optionOutline, optionLabel.c_str());
    }

    for (const auto& option : gTrackTypeDropdownOptions)
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
        {
            gViewMenu = nullptr;
            HMENU menuBar = CreateMenu();
            if (menuBar)
            {
                HMENU fileMenu = CreatePopupMenu();
                if (fileMenu)
                {
                    AppendMenuW(fileMenu, MF_STRING, kMenuCommandLoadProject, L"&Load Project...");
                    AppendMenuW(fileMenu, MF_STRING, kMenuCommandSaveProject, L"&Save Project...");
                    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(fileMenu), L"&File");
                }

                HMENU viewMenu = CreatePopupMenu();
                if (viewMenu)
                {
                    gViewMenu = viewMenu;
                    AppendMenuW(viewMenu, MF_STRING, kMenuCommandTogglePianoRoll, L"&Piano Roll");
                    AppendMenuW(viewMenu, MF_STRING, kMenuCommandToggleEffects, L"Track &Effects");
                    AppendMenuW(viewMenu, MF_STRING, kMenuCommandToggleWaveform, L"&Waveform Visualizer");
                    AppendMenuW(menuBar, MF_POPUP, reinterpret_cast<UINT_PTR>(viewMenu), L"&View");
                    updateViewMenuChecks();
                }
                else
                {
                    gViewMenu = nullptr;
                }
                SetMenu(hwnd, menuBar);
                DrawMenuBar(hwnd);
            }
        }
        return 0;
    case WM_COMMAND:
    {
        switch (LOWORD(wParam))
        {
        case kMenuCommandLoadProject:
            showLoadProjectDialog(hwnd);
            return 0;
        case kMenuCommandSaveProject:
            showSaveProjectDialog(hwnd);
            return 0;
        case kMenuCommandTogglePianoRoll:
            togglePianoRollWindow(hwnd);
            return 0;
        case kMenuCommandToggleEffects:
            toggleEffectsWindow(hwnd);
            return 0;
        case kMenuCommandToggleWaveform:
            toggleWaveformWindow(hwnd);
            return 0;
        default:
            break;
        }
        break;
    }
    case WM_LBUTTONDOWN:
    {
        int x = LOWORD(lParam);
        int y = HIWORD(lParam);
        endSliderDrag(hwnd);
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

            RECT dropdownBounds;
            if (computeDropdownBounds(gAudioDeviceOptions, dropdownBounds) && pointInRect(dropdownBounds, x, y))
            {
                return 0;
            }

            if (!pointInRect(audioDeviceButton, x, y))
            {
                audioDeviceDropdownOpen = false;
                waveDropdownOpen = false;
                waveDropdownTrackId = 0;
                InvalidateRect(hwnd, nullptr, FALSE);
                return 0;
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

        if (pointInRect(effectsToggleButton, x, y))
        {
            toggleEffectsWindow(hwnd);
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

            RECT dropdownBounds;
            if (computeDropdownBounds(gWaveOptions, dropdownBounds) && pointInRect(dropdownBounds, x, y))
            {
                return 0;
            }
        }

        if (openTrackTypeTrackId != 0)
        {
            for (const auto& option : gTrackTypeDropdownOptions)
            {
                if (option.trackId == openTrackTypeTrackId && pointInRect(option.rect, x, y))
                {
                    trackSetType(option.trackId, option.type);
                    if (option.type != TrackType::Synth && waveDropdownTrackId == option.trackId)
                    {
                        waveDropdownOpen = false;
                        waveDropdownTrackId = 0;
                    }
                    openTrackTypeTrackId = 0;
                    InvalidateRect(hwnd, nullptr, FALSE);
                    return 0;
                }
            }

            RECT dropdownBounds;
            if (computeDropdownBoundsIf(gTrackTypeDropdownOptions, dropdownBounds,
                                        [trackId = openTrackTypeTrackId](const TrackTypeDropdownOption& option) {
                                            return option.trackId == trackId;
                                        }) &&
                pointInRect(dropdownBounds, x, y))
            {
                return 0;
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
            if (showWaveSelector)
            {
                if (pointInRect(gSynthFormantSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthFormant, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthFeedbackSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthFeedback, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthPitchSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthPitch, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthPitchRangeSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthPitchRange, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthAttackSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthAttack, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthDecaySliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthDecay, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthSustainSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthSustain, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSynthReleaseSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SynthRelease, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }
            }
            else if (showSampleLoader)
            {
                if (pointInRect(gSampleAttackSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SampleAttack, activeTrackId);
                    updateSliderDrag(hwnd, x);
                    return 0;
                }

                if (pointInRect(gSampleReleaseSliderControl.control, x, y))
                {
                    openTrackTypeTrackId = 0;
                    waveDropdownOpen = false;
                    audioDeviceDropdownOpen = false;
                    beginSliderDrag(hwnd, SliderDragTarget::SampleRelease, activeTrackId);
                    updateSliderDrag(hwnd, x);
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
                    if (selectedTrackId > 0)
                        notifyEffectsWindowActiveTrackChanged(selectedTrackId);
                }
                return 0;
            }
        }

        if (previousDropdownTrack != 0 && openTrackTypeTrackId == previousDropdownTrack)
        {
            openTrackTypeTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        }

        if (waveDropdownWasOpen && waveDropdownOpen && waveDropdownTrackId == previousWaveDropdownTrack)
        {
            waveDropdownOpen = false;
            waveDropdownTrackId = 0;
            InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
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
            notifyEffectsWindowTrackListChanged();
            if (selectedTrackId > 0)
                notifyEffectsWindowActiveTrackChanged(selectedTrackId);
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
    case WM_MOUSEMOVE:
    {
        if (gSliderDrag.target != SliderDragTarget::None)
        {
            int x = GET_X_LPARAM(lParam);
            updateSliderDrag(hwnd, x);
            return 0;
        }
        break;
    }
    case WM_LBUTTONUP:
        if (gSliderDrag.target != SliderDragTarget::None)
        {
            endSliderDrag(hwnd);
            return 0;
        }
        break;
    case WM_CAPTURECHANGED:
        if (gSliderDrag.target != SliderDragTarget::None && reinterpret_cast<HWND>(lParam) != hwnd)
        {
            gSliderDrag.target = SliderDragTarget::None;
            gSliderDrag.trackId = 0;
        }
        break;
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
        gViewMenu = nullptr;
        closePianoRollWindow();
        closeEffectsWindow();
        closeWaveformWindow();
        closeCompressorWindow();
        if (gEqWindow && IsWindow(gEqWindow))
            DestroyWindow(gEqWindow);
        if (gDelayWindow && IsWindow(gDelayWindow))
            DestroyWindow(gDelayWindow);
        if (gSidechainWindow && IsWindow(gSidechainWindow))
            DestroyWindow(gSidechainWindow);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void initGUI()
{
    INITCOMMONCONTROLSEX icc {0};
    icc.dwSize = sizeof(icc);
    icc.dwICC = ICC_BAR_CLASSES | ICC_LISTVIEW_CLASSES;
    InitCommonControlsEx(&icc);

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

