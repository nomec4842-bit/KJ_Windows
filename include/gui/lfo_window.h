#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

constexpr UINT WM_LFO_REFRESH_VALUES = WM_APP + 180;
constexpr UINT WM_LFO_SET_TRACK = WM_APP + 181;

void openLfoWindow(HWND parent, int trackId);
void notifyLfoWindowTrackChanged(int trackId);
void notifyLfoWindowValuesChanged(int trackId);
void closeLfoWindow();
