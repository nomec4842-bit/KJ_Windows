#include "gui/mod_matrix_window.h"

#include "core/mod_matrix.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "gui/gui_main.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef _WIN32_IE
#define _WIN32_IE 0x0501
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif
#include <windows.h>
#include <commctrl.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <cwctype>
#include <iomanip>
#include <memory>
#include <optional>
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
constexpr int kAmountEditId = 2009;

HMENU makeControlId(int id)
{
    return reinterpret_cast<HMENU>(static_cast<intptr_t>(id));
}

constexpr int kSliderResolution = 1000;

constexpr std::array<const wchar_t*, 6> kModSources = {
    L"LFO 1",
    L"LFO 2",
    L"LFO 3",
    L"Envelope 1",
    L"Macro 1",
    L"Macro 2"
};


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
    HWND amountEdit = nullptr;
    int selectedAssignmentId = 0;
};

INITCOMMONCONTROLSEX gModMatrixInitControls = {};

std::wstring toWide(const std::string& text)
{
    return std::wstring(text.begin(), text.end());
}

void repopulateAssignmentList(ModMatrixWindowState* state);

bool trackExists(int trackId)
{
    if (trackId <= 0)
        return false;
    auto tracks = getTracks();
    return std::any_of(tracks.begin(), tracks.end(), [trackId](const Track& track) { return track.id == trackId; });
}

std::optional<TrackType> getTrackTypeForTrack(int trackId)
{
    if (!trackExists(trackId))
        return std::nullopt;
    return trackGetType(trackId);
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
    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
    if (!info)
        return L"-";

    float value = modMatrixNormalizedToValue(assignment.normalizedAmount, *info);
    float percentage = modMatrixClampNormalized(assignment.normalizedAmount) * 100.0f;

    std::wstringstream ss;
    ss << std::showpos << std::fixed << std::setprecision(2) << value;
    ss << L" (" << std::setprecision(0) << percentage << L"%)";
    ss << std::noshowpos;
    return ss.str();
}

void syncAssignmentFromTrack(ModMatrixAssignment& assignment)
{
    if (assignment.trackId <= 0 || !trackExists(assignment.trackId))
        return;

    assignment.normalizedAmount = 0.0f;
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

void populateParameterCombo(HWND combo, std::optional<TrackType> trackType = std::nullopt)
{
    if (!combo)
        return;

    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int parameterCount = modMatrixGetParameterCount();
    for (int i = 0; i < parameterCount; ++i)
    {
        const ModParameterInfo* info = modMatrixGetParameterInfo(i);
        if (!info)
            continue;

        if (trackType)
        {
            if (!modMatrixParameterSupportsTrackType(*info, *trackType))
                continue;
        }

        const wchar_t* label = info->label;
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

bool setComboSelectionByData(HWND combo, int data)
{
    if (!combo)
        return false;

    int count = static_cast<int>(SendMessageW(combo, CB_GETCOUNT, 0, 0));
    for (int i = 0; i < count; ++i)
    {
        int itemData = static_cast<int>(SendMessageW(combo, CB_GETITEMDATA, static_cast<WPARAM>(i), 0));
        if (itemData == data)
        {
            SendMessageW(combo, CB_SETCURSEL, static_cast<WPARAM>(i), 0);
            return true;
        }
    }

    return false;
}

void setSliderFromAssignment(HWND slider, const ModMatrixAssignment& assignment)
{
    if (!slider)
        return;

    float normalized = (modMatrixClampNormalized(assignment.normalizedAmount) + 1.0f) * 0.5f;
    int position = static_cast<int>(std::round(normalized * kSliderResolution));
    SendMessageW(slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderResolution));
    SendMessageW(slider, TBM_SETPOS, TRUE, position);
}

void updateAmountLabel(HWND label, const ModMatrixAssignment& assignment)
{
    if (!label)
        return;

    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
    std::wstringstream ss;
    ss << L"Mod Amount: ";
    if (info)
    {
        float value = modMatrixNormalizedToValue(assignment.normalizedAmount, *info);
        float percent = modMatrixClampNormalized(assignment.normalizedAmount) * 100.0f;
        ss << std::showpos << std::fixed << std::setprecision(2) << value;
        ss << L" (" << std::setprecision(0) << percent << L"%)";
        ss << std::noshowpos;
    }
    else
    {
        ss << L"-";
    }

    std::wstring text = ss.str();
    SetWindowTextW(label, text.c_str());
}

void setEditFromAssignment(HWND edit, const ModMatrixAssignment& assignment)
{
    if (!edit)
        return;

    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
    if (!info)
    {
        SetWindowTextW(edit, L"");
        return;
    }

    float value = modMatrixNormalizedToValue(assignment.normalizedAmount, *info);
    std::wstringstream ss;
    ss << std::showpos << std::fixed << std::setprecision(3) << value;
    std::wstring text = ss.str();
    SetWindowTextW(edit, text.c_str());
}

std::wstring trimWhitespace(const std::wstring& text)
{
    size_t start = 0;
    size_t end = text.size();
    while (start < text.size() && std::iswspace(text[start]))
        ++start;
    while (end > start && std::iswspace(text[end - 1]))
        --end;
    return text.substr(start, end - start);
}

void refreshAssignmentRowText(HWND listView, int rowIndex, const ModMatrixAssignment& assignment)
{
    if (!listView)
        return;

    std::wstring source = getSourceLabel(assignment.sourceIndex);
    std::wstring track = getTrackLabel(assignment.trackId);
    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
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

    auto assignments = modMatrixGetAssignments();

    for (size_t i = 0; i < assignments.size(); ++i)
    {
        const auto& assignment = assignments[i];
        std::wstring source = getSourceLabel(assignment.sourceIndex);

        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(source.c_str());
        item.lParam = assignment.id;

        int inserted = ListView_InsertItem(state->listView, &item);
        if (inserted >= 0)
        {
            refreshAssignmentRowText(state->listView, inserted, assignment);
        }
    }

    if (assignments.empty())
    {
        state->selectedAssignmentId = 0;
        return;
    }

    bool selectionMatched = false;
    int itemCount = ListView_GetItemCount(state->listView);
    for (int row = 0; row < itemCount; ++row)
    {
        LVITEMW item{};
        item.mask = LVIF_PARAM;
        item.iItem = row;
        if (ListView_GetItem(state->listView, &item) && item.lParam == state->selectedAssignmentId)
        {
            ListView_SetItemState(state->listView, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
            selectionMatched = true;
            break;
        }
    }

    if (!selectionMatched)
    {
        state->selectedAssignmentId = assignments.front().id;
        ListView_SetItemState(state->listView, 0, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    }
}

bool updateAssignmentAmountFromEdit(ModMatrixWindowState* state)
{
    if (!state || !state->amountEdit)
        return false;

    auto assignment = modMatrixGetAssignment(state->selectedAssignmentId);
    if (!assignment)
        return false;

    const ModParameterInfo* info = modMatrixGetParameterInfo(assignment->parameterIndex);
    if (!info)
    {
        SetWindowTextW(state->amountEdit, L"");
        return false;
    }

    int length = GetWindowTextLengthW(state->amountEdit);
    std::wstring buffer(static_cast<size_t>(length) + 1, L'\0');
    if (length > 0)
        GetWindowTextW(state->amountEdit, buffer.data(), length + 1);
    buffer.resize(std::wcslen(buffer.c_str()));

    std::wstring text = trimWhitespace(buffer);
    if (text.empty())
    {
        assignment->normalizedAmount = 0.0f;
    }
    else
    {
        bool isPercent = false;
        if (!text.empty() && text.back() == L'%')
        {
            isPercent = true;
            text.pop_back();
            text = trimWhitespace(text);
        }

        wchar_t* endPtr = nullptr;
        float value = std::wcstof(text.c_str(), &endPtr);
        if (endPtr == text.c_str())
        {
            setEditFromAssignment(state->amountEdit, *assignment);
            return false;
        }

        if (isPercent)
        {
            float normalized = modMatrixClampNormalized(value / 100.0f);
            assignment->normalizedAmount = normalized;
        }
        else
        {
            float normalized = modMatrixValueToNormalized(value, *info);
            assignment->normalizedAmount = modMatrixClampNormalized(normalized);
        }
    }

    modMatrixUpdateAssignment(*assignment);
    setSliderFromAssignment(state->amountSlider, *assignment);
    updateAmountLabel(state->amountLabel, *assignment);
    setEditFromAssignment(state->amountEdit, *assignment);
    repopulateAssignmentList(state);
    return true;
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
        state->amountEdit,
        state->removeButton,
    };

    for (HWND control : controls)
    {
        if (control)
            EnableWindow(control, enable ? TRUE : FALSE);
    }
}

void loadAssignmentIntoControls(ModMatrixWindowState* state, int assignmentId)
{
    if (!state)
        return;

    auto assignment = modMatrixGetAssignment(assignmentId);
    if (!assignment)
    {
        enableAssignmentControls(state, false);
        SetWindowTextW(state->amountLabel, L"Mod Amount:");
        if (state->amountEdit)
            SetWindowTextW(state->amountEdit, L"");
        populateTrackCombo(state->trackCombo, 0);
        return;
    }

    enableAssignmentControls(state, true);
    setComboSelectionByData(state->sourceCombo, assignment->sourceIndex);
    populateTrackCombo(state->trackCombo, assignment->trackId);
    auto trackType = getTrackTypeForTrack(assignment->trackId);
    populateParameterCombo(state->parameterCombo, trackType);

    bool parameterSelectionSet = setComboSelectionByData(state->parameterCombo, assignment->parameterIndex);
    bool parameterChanged = false;
    if (!parameterSelectionSet)
    {
        SendMessageW(state->parameterCombo, CB_SETCURSEL, 0, 0);
        int fallbackParameter = getComboSelectionData(state->parameterCombo);
        if (fallbackParameter >= 0 && fallbackParameter != assignment->parameterIndex)
        {
            assignment->parameterIndex = fallbackParameter;
            parameterChanged = true;
        }
    }

    if (parameterChanged)
    {
        syncAssignmentFromTrack(*assignment);
        modMatrixUpdateAssignment(*assignment);

        if (state->listView)
        {
            int selectedRow = ListView_GetNextItem(state->listView, -1, LVNI_SELECTED);
            if (selectedRow >= 0)
                refreshAssignmentRowText(state->listView, selectedRow, *assignment);
        }
    }

    setSliderFromAssignment(state->amountSlider, *assignment);
    updateAmountLabel(state->amountLabel, *assignment);
    setEditFromAssignment(state->amountEdit, *assignment);
}

ModMatrixWindowState* getWindowState(HWND hwnd)
{
    return reinterpret_cast<ModMatrixWindowState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void addAssignment(ModMatrixWindowState* state)
{
    ModMatrixAssignment assignment = modMatrixCreateAssignment();
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

    modMatrixUpdateAssignment(assignment);

    if (state)
        state->selectedAssignmentId = assignment.id;
}

void removeAssignment(ModMatrixWindowState* state, int assignmentId)
{
    if (assignmentId <= 0)
        return;

    if (!modMatrixRemoveAssignment(assignmentId))
        return;

    if (state)
    {
        auto assignments = modMatrixGetAssignments();
        if (!assignments.empty())
            state->selectedAssignmentId = assignments.front().id;
        else
            state->selectedAssignmentId = 0;
    }
}

void ensureModMatrixWindowClass()
{
    if (gModMatrixWindowClassRegistered)
        return;

    gModMatrixInitControls.dwSize = sizeof(gModMatrixInitControls);
    gModMatrixInitControls.dwICC = ICC_LISTVIEW_CLASSES | ICC_BAR_CLASSES;

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
                                                makeControlId(kListViewId),
                                                createStruct->hInstance,
                                                nullptr);
            if (newState->listView)
            {
                ListView_SetExtendedListViewStyle(newState->listView, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);

                LVCOLUMNW column{};
                column.mask = LVCF_WIDTH | LVCF_TEXT | LVCF_SUBITEM;

                column.cx = 120;
                column.pszText = const_cast<wchar_t*>(L"Source");
                ListView_InsertColumn(newState->listView, 0, &column);

                column.cx = 150;
                column.pszText = const_cast<wchar_t*>(L"Target");
                ListView_InsertColumn(newState->listView, 1, &column);

                column.cx = 140;
                column.pszText = const_cast<wchar_t*>(L"Parameter");
                ListView_InsertColumn(newState->listView, 2, &column);

                column.cx = 120;
                column.pszText = const_cast<wchar_t*>(L"Amount");
                ListView_InsertColumn(newState->listView, 3, &column);
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
                                                  makeControlId(kAddButtonId),
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
                                                     makeControlId(kRemoveButtonId),
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
                                                    makeControlId(kSourceComboId),
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
                                                   makeControlId(kTrackComboId),
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
                                                       makeControlId(kParameterComboId),
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
                                                    makeControlId(kAmountLabelId),
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
                                                     makeControlId(kAmountSliderId),
                                                     createStruct->hInstance,
                                                     nullptr);
            if (newState->amountSlider)
            {
                SendMessageW(newState->amountSlider, TBM_SETRANGE, TRUE, MAKELPARAM(0, kSliderResolution));
                SendMessageW(newState->amountSlider, TBM_SETTICFREQ, 100, 0);
            }

            newState->amountEdit = CreateWindowExW(0,
                                                   L"EDIT",
                                                   L"",
                                                   WS_CHILD | WS_VISIBLE | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,
                                                   0,
                                                   0,
                                                   0,
                                                   0,
                                                   hwnd,
                                                   makeControlId(kAmountEditId),
                                                   createStruct->hInstance,
                                                   nullptr);
            if (newState->amountEdit)
                SendMessageW(newState->amountEdit, EM_SETLIMITTEXT, 32, 0);

            auto configureComboDropdown = [](HWND combo) {
                if (!combo)
                    return;
#ifdef CB_SETMINVISIBLE
                SendMessageW(combo, CB_SETMINVISIBLE, 8, 0);
#endif
            };

            configureComboDropdown(newState->sourceCombo);
            configureComboDropdown(newState->trackCombo);
            configureComboDropdown(newState->parameterCombo);

            populateSourceCombo(newState->sourceCombo);
            populateParameterCombo(newState->parameterCombo);
            populateTrackCombo(newState->trackCombo, 0);

            auto existingAssignments = modMatrixGetAssignments();
            if (existingAssignments.empty())
            {
                addAssignment(newState);
            }
            else
            {
                newState->selectedAssignmentId = existingAssignments.front().id;
            }

            repopulateAssignmentList(newState);
            loadAssignmentIntoControls(newState, newState->selectedAssignmentId);

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
            const int editHeight = 28;

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

            if (state->amountEdit)
                MoveWindow(state->amountEdit, padding, formY, controlWidth, editHeight, TRUE);
            formY += editHeight + buttonSpacing;

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
                loadAssignmentIntoControls(state, state->selectedAssignmentId);
                return 0;
            case kRemoveButtonId:
                removeAssignment(state, state->selectedAssignmentId);
                repopulateAssignmentList(state);
                loadAssignmentIntoControls(state, state->selectedAssignmentId);
                return 0;
            case kSourceComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->sourceCombo);
                    auto assignment = modMatrixGetAssignment(state->selectedAssignmentId);
                    if (assignment && data >= 0)
                    {
                        assignment->sourceIndex = data;
                        modMatrixUpdateAssignment(*assignment);
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            case kTrackComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->trackCombo);
                    auto assignment = modMatrixGetAssignment(state->selectedAssignmentId);
                    if (assignment)
                    {
                        assignment->trackId = data;
                        auto trackType = getTrackTypeForTrack(data);
                        populateParameterCombo(state->parameterCombo, trackType);

                        bool parameterSelectionSet = setComboSelectionByData(state->parameterCombo, assignment->parameterIndex);
                        if (!parameterSelectionSet)
                        {
                            SendMessageW(state->parameterCombo, CB_SETCURSEL, 0, 0);
                            int fallbackParameter = getComboSelectionData(state->parameterCombo);
                            if (fallbackParameter >= 0)
                                assignment->parameterIndex = fallbackParameter;
                        }

                        syncAssignmentFromTrack(*assignment);
                        modMatrixUpdateAssignment(*assignment);
                        setSliderFromAssignment(state->amountSlider, *assignment);
                        updateAmountLabel(state->amountLabel, *assignment);
                        setEditFromAssignment(state->amountEdit, *assignment);
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            case kParameterComboId:
                if (code == CBN_SELCHANGE)
                {
                    int data = getComboSelectionData(state->parameterCombo);
                    auto assignment = modMatrixGetAssignment(state->selectedAssignmentId);
                    if (assignment && data >= 0)
                    {
                        assignment->parameterIndex = data;
                        syncAssignmentFromTrack(*assignment);
                        modMatrixUpdateAssignment(*assignment);
                        setSliderFromAssignment(state->amountSlider, *assignment);
                        updateAmountLabel(state->amountLabel, *assignment);
                        setEditFromAssignment(state->amountEdit, *assignment);
                        repopulateAssignmentList(state);
                    }
                }
                return 0;
            case kAmountEditId:
                if (code == EN_KILLFOCUS)
                {
                    updateAssignmentAmountFromEdit(state);
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
                            loadAssignmentIntoControls(state, state->selectedAssignmentId);
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
                auto assignment = modMatrixGetAssignment(state->selectedAssignmentId);
                if (!assignment)
                    return 0;

                int position = static_cast<int>(SendMessageW(state->amountSlider, TBM_GETPOS, 0, 0));
                float sliderNormalized = static_cast<float>(position) / static_cast<float>(kSliderResolution);
                assignment->normalizedAmount = modMatrixClampNormalized(sliderNormalized * 2.0f - 1.0f);
                modMatrixUpdateAssignment(*assignment);
                modMatrixApplyAssignment(*assignment);
                updateAmountLabel(state->amountLabel, *assignment);
                setEditFromAssignment(state->amountEdit, *assignment);

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

            auto selection = modMatrixGetAssignment(state->selectedAssignmentId);
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
            auto assignments = modMatrixGetAssignments();
            for (auto& assignment : assignments)
            {
                if (trackId == 0 || assignment.trackId == trackId)
                {
                    syncAssignmentFromTrack(assignment);
                    modMatrixUpdateAssignment(assignment);
                }
            }

            auto selected = modMatrixGetAssignment(state->selectedAssignmentId);
            if (selected)
            {
                setSliderFromAssignment(state->amountSlider, *selected);
                updateAmountLabel(state->amountLabel, *selected);
                setEditFromAssignment(state->amountEdit, *selected);
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

void openModMatrixWindow(HWND parent)
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        SetForegroundWindow(gModMatrixWindow);
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

void toggleModMatrixWindow(HWND parent)
{
    if (gModMatrixWindow && IsWindow(gModMatrixWindow))
    {
        closeModMatrixWindow();
        requestMainMenuRefresh();
        return;
    }

    openModMatrixWindow(parent);
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

void focusModMatrixTarget(ModMatrixParameter parameter, int trackId)
{
    if (trackId <= 0)
        return;

    if (!gModMatrixWindow || !IsWindow(gModMatrixWindow))
        return;

    ModMatrixWindowState* state = getWindowState(gModMatrixWindow);
    if (!state)
        return;

    int parameterIndex = modMatrixGetParameterIndex(parameter);
    if (parameterIndex < 0)
        return;

    auto assignments = modMatrixGetAssignments();
    auto existing = std::find_if(assignments.begin(), assignments.end(), [&](const ModMatrixAssignment& assignment) {
        return assignment.trackId == trackId && assignment.parameterIndex == parameterIndex;
    });

    int assignmentId = 0;
    if (existing != assignments.end())
    {
        assignmentId = existing->id;
    }
    else
    {
        ModMatrixAssignment assignment = modMatrixCreateAssignment();
        assignment.sourceIndex = 0;
        assignment.trackId = trackId;
        assignment.parameterIndex = parameterIndex;
        syncAssignmentFromTrack(assignment);
        modMatrixUpdateAssignment(assignment);
        assignmentId = assignment.id;
    }

    state->selectedAssignmentId = assignmentId;
    repopulateAssignmentList(state);
    loadAssignmentIntoControls(state, state->selectedAssignmentId);

    if (state->listView)
    {
        int itemCount = ListView_GetItemCount(state->listView);
        for (int row = 0; row < itemCount; ++row)
        {
            LVITEMW item{};
            item.mask = LVIF_PARAM;
            item.iItem = row;
            if (ListView_GetItem(state->listView, &item) && static_cast<int>(item.lParam) == assignmentId)
            {
                ListView_SetItemState(state->listView, row, LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
                ListView_EnsureVisible(state->listView, row, FALSE);
                break;
            }
        }
    }

    SetForegroundWindow(gModMatrixWindow);
}

