#pragma once

#include "core/tracks.h"

#include <string>

int trackGetMidiChannel(int trackId);
void trackSetMidiChannel(int trackId, int channel);

int trackGetMidiPort(int trackId);
std::wstring trackGetMidiPortName(int trackId);
void trackSetMidiPort(int trackId, int portId, const std::wstring& portName);

