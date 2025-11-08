#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

constexpr UINT WM_COMPRESSOR_REFRESH_VALUES = WM_APP + 40;
constexpr UINT WM_COMPRESSOR_SET_TRACK = WM_APP + 41;

void openCompressorWindow(HWND parent, int trackId);
void notifyCompressorWindowTrackChanged(int trackId);
void notifyCompressorWindowValuesChanged(int trackId);
void closeCompressorWindow();
