#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

void toggleModMatrixWindow(HWND parent);
void closeModMatrixWindow();
bool isModMatrixWindowOpen();
void notifyModMatrixWindowTrackListChanged();
void notifyModMatrixWindowValuesChanged(int trackId);
