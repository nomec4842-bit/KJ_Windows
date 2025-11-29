#include "gui/gui_refresh.h"
#include "gui/menu_commands.h"
#include "gui/mod_matrix_window.h"
#include "gui/waveform_window.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern HWND gMainWindow;
extern HMENU gViewMenu;
extern HWND gPianoRollWindow;
extern HWND gEffectsWindow;

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

    UINT modMatrixState = isModMatrixWindowOpen() ? MF_CHECKED : MF_UNCHECKED;
    CheckMenuItem(gViewMenu, kMenuCommandToggleModMatrix, MF_BYCOMMAND | modMatrixState);
}

void requestMainMenuRefresh()
{
    if (!gMainWindow || !IsWindow(gMainWindow))
        return;

    if (gViewMenu && IsMenu(gViewMenu))
    {
        updateViewMenuChecks();
        DrawMenuBar(gMainWindow);
    }

    InvalidateRect(gMainWindow, nullptr, FALSE);
}
