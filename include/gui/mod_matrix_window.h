#pragma once

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
#include "core/mod_matrix_parameters.h"

#include <windows.h>
#include <commctrl.h>

void toggleModMatrixWindow(HWND parent);
void openModMatrixWindow(HWND parent);
void closeModMatrixWindow();
bool isModMatrixWindowOpen();
void notifyModMatrixWindowTrackListChanged();
void notifyModMatrixWindowValuesChanged(int trackId);
void focusModMatrixTarget(ModMatrixParameter parameter, int trackId);
