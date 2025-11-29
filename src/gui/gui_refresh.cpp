#include "gui/gui_refresh.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

extern HWND gMainWindow;
extern HMENU gViewMenu;
void updateViewMenuChecks();

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
