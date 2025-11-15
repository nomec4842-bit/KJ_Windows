#include "gui/mod_matrix_window.h"

#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr wchar_t kModMatrixWindowClassName[] = L"KJModMatrixWindow";
constexpr int kModMatrixWindowWidth = 520;
constexpr int kModMatrixWindowHeight = 420;

constexpr UINT WM_MOD_MATRIX_REFRESH_TRACKS = WM_APP + 120;
constexpr UINT WM_MOD_MATRIX_REFRESH_VALUES = WM_APP + 121;

constexpr int kListViewId = 2001;
constexpr int kAddButtonId = 2002;
constexpr int kRemoveButtonId = 2003;
constexpr int kSourceComboId = 2004;
constexpr int kTrackComboId = 2005;
constexpr int kParameterComboId = 2006;
constexpr int kAmountLabelId = 2007;
constexpr int kAmountSliderId = 2008;

constexpr int kSliderResolution = 1000;

struct ModMatrixAssignment
{
    int id = 0;
    int sourceIndex = 0;
    int trackId = 0;
    int parameterIndex = 0;
    float normalizedAmount = 0.5f;
};

using ParameterGetter = float (*)(int);
using ParameterSetter = void (*)(int, float);

struct ModParameterInfo
{
    const wchar_t* label;
    ParameterGetter getter;
    ParameterSetter setter;
    float minValue;
    float maxValue;
};

constexpr std::array<const wchar_t*, 6> kModSources = {
    L"LFO 1",
    L"LFO 2",
    L"LFO 3",
    L"Envelope 1",
    L"Macro 1",
    L"Macro 2"
};

constexpr std::array<ModParameterInfo, 8> kModParameters = {
    ModParameterInfo{L"Volume", trackGetVolume, trackSetVolume, 0.0f, 1.0f},
    ModParameterInfo{L"Pan", trackGetPan, trackSetPan, -1.0f, 1.0f},
    ModParameterInfo{L"Synth Pitch", trackGetSynthPitch, trackSetSynthPitch, -12.0f, 12.0f},
    ModParameterInfo{L"Synth Formant", trackGetSynthFormant, trackSetSynthFormant, 0.0f, 1.0f},
    ModParameterInfo{L"Synth Resonance", trackGetSynthResonance, trackSetSynthResonance, 0.0f, 1.0f},
    ModParameterInfo{L"Delay Mix", trackGetDelayMix, trackSetDelayMix, 0.0f, 1.0f},
    ModParameterInfo{L"Compressor Threshold", trackGetCompressorThresholdDb, trackSetCompressorThresholdDb, -60.0f, 0.0f},
    ModParameterInfo{L"Compressor Ratio", trackGetCompressorRatio, trackSetCompressorRatio, 1.0f, 20.0f},
};

std::vector<ModMatrixAssignment> gAssignments;
int gNextAssignmentId = 1;

HWND gModMatrixWindow = nullptr;
bool gModMatrixWindowClassRegistered = false;

struct ModMatrixWindowState
{
    HWND listView = nullptr;
    HWND addButton = nullptr;
    HWND removeButton = nullptr;
    HWND sourceCombo = nullptr;
    HWND trackCombo = nullptr;
    HWND parameterCombo = nullptr;
    HWND amountLabel = nullptr;
    HWND amountSlider = nullptr;
    int selectedAssignmentId = 0;
};

INITCOMMONCONTROLSEX gModMatrixInitControls = {sizeof(INITCOMMONCONTROLSEX), ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES};

float clampNormalized(float value)
{
    return std::clamp(value, 0.0f, 1.0f);
}

float normalizedToValue(float normalized, const ModParameterInfo& info)
{
    float clamped = clampNormalized(normalized);
    return info.minValue + clamped * (info.maxValue - info.minValue);
}

float valueToNormalized(float value, const ModParameterInfo& info)
{
    float range = info.maxValue - info.minValue;
    if (range <= 0.0001f)
        return 0.0f;
    float normalized = (value - info.minValue) / range;
    return clampNormalized(normalized);
}

std::wstring toWide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

bool trackExists(int trackId)
{
    if (trackId <= 0)
        return false;
    auto tracks = getTracks();
    return std::any_of(tracks.begin(), tracks.end(), [trackId](const Track& track) { return track.id == trackId; });
}

const ModParameterInfo* getParameterInfo(int index)
{
    if (index < 0 || index >= static_cast<int>(kModParameters.size()))
        return nullptr;
    return &kModParameters[static_cast<size_t>(index)];
}

ModMatrixAssignment* findAssignment(int id)
{
    for (auto& assignment : gAssignments)
    {
        if (assignment.id == id)
            return &assignment;
    }
    return nullptr;
}

std::wstring getSourceLabel(int index)
{
    if (index < 0 || index >= static_cast<int>(kModSources.size()))
        return L"Unknown";
    return kModSources[static_cast<size_t>(index)];
}

std::wstring getTrackLabel(int trackId)
{
    if (trackId <= 0)
        return L"None";
    auto tracks = getTracks();
    for (const auto& track : tracks)
    {
        if (track.id == trackId)
        {
            std::wstring name = toWide(track.name);
            if (name.empty())
            {
                std::wstringstream ss;
                ss << L"Track " << track.id;
                return ss.str();
            }
            return name;
        }
    }
    std::wstringstream ss;
    ss << L"Missing (" << trackId << L")";
    return ss.str();
}

std::wstring formatAmountText(const ModMatrixAssignment& assignment)
{
    const ModParameterInfo* info = getParameterInfo(assignment.parameterIndex);
    if (!info)
        return L"-";

    float value = normalizedToValue(assignment.normalizedAmount, *info);
    float percentage = clampNormalized(assignment.normalizedAmount) * 100.0f;

    std::wstringstream ss;
    ss << std::fixed << std::setprecision(2) << value << L" (" << std::setprecision(0) << percentage << L"%)";
    return ss.str();
}

void applyAssignment(const ModMatrixAssignment& assignment)
{
    if (assignment.trackId <= 0 || !trackExists(assignment.trackId))
        return;

    const ModParameterInfo* info = getParameterInfo(assignment.parameterIndex);
    if (!info || !info->setter)
        return;

    float value = normalizedToValue(assignment.normalizedAmount, *info);
    info->setter(assignment.trackId, value);
}

void syncAssignmentFromTrack(ModMatrixAssignment& assignment)
{
    if (assignment.trackId <= 0 || !trackExists(assignment.trackId))
        return;

    const ModParameterInfo* info = getParameterInfo(assignment.parameterIndex);
    if (!info || !info->getter)
        return;

    float current = info->getter(assignment.trackId);
    assignment.normalizedAmount = valueToNormalized(current, *info);
}

void populateSourceCombo(HWND combo)
{
    if (!combo)
        return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < kModSources.size(); ++i)
    {
        const wchar_t* label = kModSources[i];
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        if (index >= 0)
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
}

void populateParameterCombo(HWND combo)
{
    if (!combo)
        return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < kModParameters.size(); ++i)
    {
        const wchar_t* label = kModParameters[i].label;
        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        if (index >= 0)
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(i));
    }
}

void populateTrackCombo(HWND combo, int selectedTrackId)
{
    if (!combo)
        return;

    auto tracks = getTracks();

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    LRESULT noneIndex = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"None"));
    if (noneIndex >= 0)
        SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(noneIndex), 0);

    for (const auto& track : tracks)
    {
        std::wstring label = toWide(track.name);
        if (label.empty())
        {
            std::wstringstream ss;
            ss << L"Track " << track.id;
            label = ss.str();
        }

        LRESULT index = SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        if (index >= 0)
            SendMessageW(combo, CB_SETITEMDATA, static_cast<WPARAM>(index), static_cast<LPARAM>(track.id));
    }

    int itemCount = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < itemCount; ++i)
    {
        int trackId = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if (trackId == selectedTrackId)
        {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }

    SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(noneIndex >= 0 ? noneIndex : 0), 0);
}

int getComboSelectionData(HWND combo)
{
    if (!combo)
        return -1;
    LRESULT index = SendMessageW(combo, CB_GETCURSEL, 0, 0);
    if (index < 0)
        return -1;
    return static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(index), 0));
}

void setComboSelectionByData(HWND combo, int data)
{
    if (!combo)
        return;

    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        int itemData = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if (itemData == data)
        {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return;
        }
    }
}

void setSliderFromAssignment(HWND slider, const ModMatrixAssignment& assignment)
{
    if (!slider)
        return;

    int position = static_cast<int>(std::round(clampNormalized(assignment.normalizedAmount) * kSliderResolution));
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderResolution));
    SendMessageW(slider, TBM_SETPOS, TRUE, position);
}

void updateAmountLabel(HWND label, const ModMatrixAssignment& assignment)
{
    if (!label)
        return;

    const ModParameterInfo* info = getParameterInfo(assignment.parameterIndex);
    std::wstringstream ss;
    ss << L"Mod Amount: ";
    if (info)
    {
        float value = normalizedToValue(assignment.normalizedAmount, *info);
        ss << std::fixed << std::setprecision(2) << value;
        ss << L" (" << std::setprecision(0) << clampNormalized(assignment.normalizedAmount) * 100.0f << L"%)";
    }
    else
    {
        ss << L"-";
    }

    std::wstring text = ss.str();
    SetWindowTextW(label, text.c_str());
}

void refreshAssignmentRowText(HWND listView, int rowIndex, const ModMatrixAssignment& assignment)
{
    if (!listView)
        return;

    std::wstring source = getSourceLabel(assignment.sourceIndex);
    std::wstring track = getTrackLabel(assignment.trackId);
    const ModParameterInfo* info = getParameterInfo(assignment.parameterIndex);
    std::wstring parameter = info ? info->label : L"Unknown";
    std::wstring amount = formatAmountText(assignment);

    ListView_SetItemText(listView, rowIndex, 0, const_cast<wchar_t*>(source.c_str()));
    ListView_SetItemText(listView, rowIndex, 1, const_cast<wchar_t*>(track.c_str()));
    ListView_SetItemText(listView, rowIndex, 2, const_cast<wchar_t*>(parameter.c_str()));
    ListView_SetItemText(listView, rowIndex, 3, const_cast<wchar_t*>(amount.c_str()));
}

void repopulateAssignmentList(ModMatrixWindowState* state)
{
    if (!state || !state->listView)
        return;

    ListView_DeleteAllItems(state->listView);

    for (size_t i = 0; i < gAssignments.size(); ++i)
    {
        const auto& assignment = gAssignments[i];
        std::wstring source = getSourceLabel(assignment.sourceIndex);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(source.c_str());
        item.lParam = assignment.id;

        int inserted = ListView_InsertItemW(state->listView, &item);
        if (inserted >= 0)
        {
            refreshAssignmentRowText(state->listView, inserted, assignment);
        }
    }

    if (!gAssignments.empty())
    {
        for (int row = 0; row < ListView_GetItemCount(state->listView); ++row)
        {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = row;
            if (ListView_GetItem(state->listView, &item))
            {
                if (item.lParam == state->selectedAssignmentId)
                {
                    ListView_SetItemState(state->listView, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                    return;
                }
            }
        }
        ListView_SetItemState(state->listView, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

void enableAssignmentControls(ModMatrixWindowState* state, bool enable)
{
    if (!state)
        return;

    const HWND controls[] = {
        state->sourceCombo,
        state->trackCombo,
        state->parameterCombo,
        state->amountLabel,
        state->amountSlider,
        state->removeButton,
    };

    for (HWND control : controls)
    {
        if (control)
            EnableWindow(control, enable ? TRUE : FALSE);
    }
}

void loadAssignmentIntoControls(ModMatrixWindowState* state, const ModMatrixAssignment* assignment)
{
    if (!state)
        return;

    if (!assignment)
    {
        enableAssignmentControls(state, false);
        SetWindowTextW(state->amountLabel, L"Mod Amount:");
        return;
    }

    enableAssignmentControls(state, true);
    setComboSelectionByData(state->sourceCombo, assignment->sourceIndex);
    populateTrackCombo(state->trackCombo, assignment->trackId);
    setComboSelectionByData(state->parameterCombo, assignment->parameterIndex);
    setSliderFromAssignment(state->amountSlider, *assignment);
    updateAmountLabel(state->amountLabel, *assignment);
}

ModMatrixWindowState* getWindowState(HWND hwnd)
{
    return reinterpret_cast<ModMatrixWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void addAssignment(ModMatrixWindowState* state)
{
    ModMatrixAssignment assignment;
    assignment.id = gNextAssignmentId++;
    assignment.sourceIndex = 0;
    assignment.parameterIndex = 0;
    assignment.trackId = getActiveSequencerTrackId();
    if (assignment.trackId <= 0)
    {
        auto tracks = getTracks();
        if (!tracks.empty())
            assignment.trackId = tracks.front().id;
    }

    if (assignment.trackId > 0)
        syncAssignmentFromTrack(assignment);

    gAssignments.push_back(assignment);

    if (state)
        state->selectedAssignmentId = assignment.id;
}

void removeAssignment(ModMatrixWindowState* state, int assignmentId)
{
    if (assignmentId <= 0)
        return;

    auto it = std::remove_if(gAssignments.begin(), gAssignments.end(), [assignmentId](const ModMatrixAssignment& value) {
        return value.id == assignmentId;
    });
    if (it != gAssignments.end())
    {
        gAssignments.erase(it, gAssignments.end());
        if (state)
        {
            if (!gAssignments.empty())
                state->selectedAssignmentId = gAssignments.front().id;
            else
                state->selectedAssignmentId = 0;
        }
    }
}

void ensureModMatrixWindowClass()
{
    if (gModMatrixWindowClassRegistered)
        return;

    InitCommonControlsEx(&gModMatrixInitControls);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = [](HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) -> LRESULT {
        ModMatrixWindowState* state = getWindowState(hwnd);

        switch (msg)
        {
        case WM_CREATE:
        {
            auto createStruct = reinterpret_cast<LPCREATESTRUCTW>(lParam);
            auto* newState = new ModMatrixWindowState();
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(newState));

            newState->listView = CreateWindowExW(WS_EX_CLIENTEDGE,
                                                WC_LISTVIEWW,
                                                L"",
                                                WS_CHILD | WS_VISIBLE | LVS_REPORT | LVS_SINGLESEL,
                                                0,
                                                0,
                                                0,
                                                0,
                                                hwnd,
                                                reinterpret_cast<HMENU>(kListViewId),
                                                createStruct->hInstance,
                                                nullptr);
            if (newState->listView)
            {
                ListView_SetExtendedListViewStyle(newState->listView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

                LVCOLUMNW column{};
                column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;

                column.cx = 120;
                column.pszText = const_cast<wchar_t*>(L"Source");
                ListView_InsertColumnW(newState->listView, 0, &column);

                column.cx = 150;
                column.pszText = const_cast<wchar_t*>(L"Target");
                ListView_InsertColumnW(newState->listView, 1, &column);

                column.cx = 140;
                column.pszText = const_cast<wchar_t*>(L"Parameter");
                ListView_InsertColumnW(newState->listView, 2, &column);

                column.cx = 120;
                column.pszText = const_cast<wchar_t*>(L"Amount");
                ListView_InsertColumnW(newState->listView, 3, &column);
            }

            newState->addButton = CreateWindowExW(0,
                                                  L"BUTTON",
                                                  L"Add Assignment",
                                                  WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                  0,
                                                  0,
                                                  0,
                                                  0,
                                                  hwnd,
                                                  reinterpret_cast<HMENU>(kAddButtonId),
                                                  createStruct->hInstance,
                                                  nullptr);

            newState->removeButton = CreateWindowExW(0,
                                                     L"BUTTON",
                                                     L"Remove Assignment",
                                                     WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     hwnd,
                                                     reinterpret_cast<HMENU>(kRemoveButtonId),
                                                     createStruct->hInstance,
                                                     nullptr);

            newState->sourceCombo = CreateWindowExW(0,
                                                    WC_COMBOBOXW,
                                                    L"",
                                                    WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    hwnd,
                                                    reinterpret_cast<HMENU>(kSourceComboId),
                                                    createStruct->hInstance,
                                                    nullptr);

            newState->trackCombo = CreateWindowExW(0,
                                                   WC_COMBOBOXW,
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   hwnd,
                                                   reinterpret_cast<HMENU>(kTrackComboId),
                                                   createStruct->hInstance,
                                                   nullptr);

            newState->parameterCombo = CreateWindowExW(0,
                                                       WC_COMBOBOXW,
                                                       L"",
                                                       WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL,
                                                       0,
                                                       0,
                                                       0,
                                                       0,
                                                       hwnd,
                                                       reinterpret_cast<HMENU>(kParameterComboId),
                                                       createStruct->hInstance,
                                                       nullptr);

            newState->amountLabel = CreateWindowExW(0,
                                                    L"STATIC",
                                                    L"Mod Amount:",
                                                    WS_CHILD | WS_VISIBLE | SS_LEFT,
                                                    0,
                                                    0,
                                                    0,
                                                    0,
                                                    hwnd,
                                                    reinterpret_cast<HMENU>(kAmountLabelId),
                                                    createStruct->hInstance,
                                                    nullptr);

            newState->amountSlider = CreateWindowExW(0,
                                                     TRACKBAR_CLASSW,
                                                     L"",
                                                     WS_CHILD | WS_VISIBLE | TBS_AUTOTICKS,
                                                     0,
                                                     0,
                                                     0,
                                                     0,
                                                     hwnd,
                                                     reinterpret_cast<HMENU>(kAmountSliderId),
                                                     createStruct->hInstance,
                                                     nullptr);
            if (newState->amountSlider)
            {
                SendMessageW(newState->amountSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderResolution));
                SendMessageW(newState->amountSlider, TBM_SETTICFREQ, 100, 0);
            }

            populateSourceCombo(newState->sourceCombo);
            populateParameterCombo(newState->parameterCombo);
            populateTrackCombo(newState->trackCombo, 0);

            if (gAssignments.empty())
            {
                addAssignment(newState);
            }

            repopulateAssignmentList(newState);
            loadAssignmentIntoControls(newState, findAssignment(newState->selectedAssignmentId));

            RECT rect{};
            GetClientRect(hwnd, &rect);
            SendMessageW(hwnd, WM_SIZE, 0, MAKELPARAM(rect.right - rect.left, rect.bottom - rect.top));
            return 0;
        }
        case WM_DESTROY:
        {
            if (state)
            {
                delete state;
                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            }
            if (hwnd == gModMatrixWindow)
            {
                gModMatrixWindow = nullptr;
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
            const int padding = 12;
            const int buttonHeight = 28;
            const int buttonSpacing = 8;
            const int comboHeight = 28;
            const int labelHeight = 20;
            const int sliderHeight = 32;

            int listViewHeight = std::max(140, height / 2);

            if (state->listView)
                MoveWindow(state->listView, padding, padding, std::max(0, width - padding * 2), listViewHeight, TRUE);

            int buttonY = padding + listViewHeight + buttonSpacing;
            int buttonWidth = 140;

            if (state->addButton)
                MoveWindow(state->addButton, padding, buttonY, buttonWidth, buttonHeight, TRUE);
            if (state->removeButton)
                MoveWindow(state->removeButton, padding + buttonWidth + buttonSpacing, buttonY, buttonWidth, buttonHeight, TRUE);

            int formY = buttonY + buttonHeight + buttonSpacing;

            int controlWidth = std::max(0, width - padding * 2);
            if (state->sourceCombo)
                MoveWindow(state->sourceCombo, padding, formY, controlWidth, comboHeight, TRUE);
            formY += comboHeight + buttonSpacing;

            if (state->trackCombo)
                MoveWindow(state->trackCombo, padding, formY, controlWidth, comboHeight, TRUE);
            formY += comboHeight + buttonSpacing;

            if (state->parameterCombo)
                MoveWindow(state->parameterCombo, padding, formY, controlWidth, comboHeight, TRUE);
            formY += comboHeight + buttonSpacing;

            if (state->amountLabel)
                MoveWindow(state->amountLabel, padding, formY, controlWidth, labelHeight, TRUE);
            formY += labelHeight + buttonSpacing;

            if (state->amountSlider)
                MoveWindow(state->amountSlider, padding, formY, controlWidth, sliderHeight, TRUE);

            return 0;
        }
        case WM_COMMAND:
        {
            if (!state)
                return 0;

            int controlId = LOWORD(wParam);
            int code = HIWORD(wParam);

            switch (controlId)
            {
            case kAddButtonId:
                addAssignment(state);
                repopulateAssignmentList(state);
                loadAssignmentIntoControls(state, findAssignment(state->selectedAssignmentId));
                return 0;
            case kRemoveButtonId:
                removeAssignment(state, state->selectedAssignmentId);
                repopulateAssignmentList(state);
                loadAssignmentIntoControls(state, findAssignment(state->selectedAssignmentId));
                return 0;
            case kSourceComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->sourceCombo);
                    ModMatrixAssignment* assignment = findAssignment(state->selectedAssignmentId);
                    if (assignment && data >= 0)
                    {
                        assignment->sourceIndex = data;
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            case kTrackComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->trackCombo);
                    ModMatrixAssignment* assignment = findAssignment(state->selectedAssignmentId);
                    if (assignment)
                    {
                        assignment->trackId = data;
                        syncAssignmentFromTrack(*assignment);
                        setSliderFromAssignment(state->amountSlider, *assignment);
                        updateAmountLabel(state->amountLabel, *assignment);
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            case kParameterComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->parameterCombo);
                    ModMatrixAssignment* assignment = findAssignment(state->selectedAssignmentId);
                    if (assignment && data >= 0)
                    {
                        assignment->parameterIndex = data;
                        syncAssignmentFromTrack(*assignment);
                        setSliderFromAssignment(state->amountSlider, *assignment);
                        updateAmountLabel(state->amountLabel, *assignment);
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            default:
                break;
            }
            return 0;
        }
        case WM_NOTIFY:
        {
            if (!state)
                return 0;

            auto* header = reinterpret_cast<LPNMHDR>(lParam);
            if (header->hwndFrom == state->listView && header->code == LVN_ITEMCHANGED)
            {
                auto* changed = reinterpret_cast<LPNMLISTVIEW>(lParam);
                if ((changed->uChanged & LVIF_STATE) != 0)
                {
                    if ((changed->uNewState & LVIS_SELECTED) != 0)
                    {
                        LVITEMW item{};
                        item.mask = LVIF_PARAM;
                        item.iItem = changed->iItem;
                        item.iSubItem = 0;
                        if (ListView_GetItem(state->listView, &item))
                        {
                            state->selectedAssignmentId = static_cast<int>(item.lParam);
                            loadAssignmentIntoControls(state, findAssignment(state->selectedAssignmentId));
                        }
                    }
                }
            }
            return 0;
        }
        case WM_HSCROLL:
        {
            if (!state)
                return 0;

            HWND slider = reinterpret_cast<HWND>(lParam);
            if (slider == state->amountSlider)
            {
                ModMatrixAssignment* assignment = findAssignment(state->selectedAssignmentId);
                if (!assignment)
                    return 0;

                int position = static_cast<int>(SendMessageW(state->amountSlider, TBM_GETPOS, 0, 0));
                assignment->normalizedAmount = clampNormalized(static_cast<float>(position) / static_cast<float>(kSliderResolution));
                applyAssignment(*assignment);
                updateAmountLabel(state->amountLabel, *assignment);

                int itemCount = ListView_GetItemCount(state->listView);
                for (int row = 0; row < itemCount; ++row)
                {
                    LVITEMW item{};
                    item.mask = LVIF_PARAM;
                    item.iItem = row;
                    if (ListView_GetItem(state->listView, &item) && static_cast<int>(item.lParam) == assignment->id)
                    {
                        refreshAssignmentRowText(state->listView, row, *assignment);
                        break;
                    }
                }
            }
            return 0;
        }
        case WM_MOD_MATRIX_REFRESH_TRACKS:
        {
            if (!state)
                return 0;

            ModMatrixAssignment* selection = findAssignment(state->selectedAssignmentId);
            int trackId = selection ? selection->trackId : 0;
            populateTrackCombo(state->trackCombo, trackId);
            repopulateAssignmentList(state);
            return 0;
        }
        case WM_MOD_MATRIX_REFRESH_VALUES:
        {
            if (!state)
                return 0;

            int trackId = static_cast<int>(wParam);
            for (auto& assignment : gAssignments)
            {
                if (trackId == 0 || assignment.trackId == trackId)
                {
                    syncAssignmentFromTrack(assignment);
                }
            }

            ModMatrixAssignment* selected = findAssignment(state->selectedAssignmentId);
            if (selected)
            {
                setSliderFromAssignment(state->amountSlider, *selected);
                updateAmountLabel(state->amountLabel, *selected);
            }

            repopulateAssignmentList(state);
            return 0;
        }
        default:
            break;
        }

        return DefWindowProcW(hwnd, msg, wParam, lParam);
    };

    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kModMatrixWindowClassName;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);

    if (RegisterClassW(&wc))
    {
        gModMatrixWindowClassRegistered = true;
    }
}

} // namespace

bool isModMatrixWindowOpen()
{
    return gModMatrixWindow && IsWindow(gModMatrixWindow);
}

void closeModMatrixWindow()
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        DestroyWindow(gModMatrixWindow);
        gModMatrixWindow = nullptr;
    }
}

void toggleModMatrixWindow(HWND parent)
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        closeModMatrixWindow();
        requestMainMenuRefresh();
        return;
    }

    ensureModMatrixWindowClass();
    if (!gModMatrixWindowClassRegistered)
        return;

    RECT parentRect{0, 0, 0, 0};
    if (parent && IsWindow(parent))
        GetWindowRect(parent, &parentRect);

    int x = CW_USEDEFAULT;
    int y = CW_USEDEFAULT;
    if (parentRect.right > parentRect.left && parentRect.bottom > parentRect.top)
    {
        x = parentRect.left + 80;
        y = parentRect.top + 80;
    }

    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW,
                                kModMatrixWindowClassName,
                                L"Modulation Matrix",
                                WS_OVERLAPPEDWINDOW,
                                x,
                                y,
                                kModMatrixWindowWidth,
                                kModMatrixWindowHeight,
                                parent,
                                nullptr,
                                GetModuleHandle(nullptr),
                                nullptr);
    if (hwnd)
    {
        gModMatrixWindow = hwnd;
        ShowWindow(hwnd, SW_SHOW);
        UpdateWindow(hwnd);
        requestMainMenuRefresh();
    }
}

void notifyModMatrixWindowTrackListChanged()
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        PostMessageW(gModMatrixWindow, WM_MOD_MATRIX_REFRESH_TRACKS, 0, 0);
    }
}

void notifyModMatrixWindowValuesChanged(int trackId)
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        PostMessageW(gModMatrixWindow, WM_MOD_MATRIX_REFRESH_VALUES, static_cast<WPARAM>(trackId), 0);
    }
}

