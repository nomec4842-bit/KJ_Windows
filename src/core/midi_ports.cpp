#include "core/midi_ports.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <utility>
#include <vector>

std::vector<MidiOutPort> getAvailableMidiOutPorts()
{
    UINT deviceCount = midiOutGetNumDevs();
    std::vector<MidiOutPort> ports;
    ports.reserve(deviceCount);
    for (UINT deviceIndex = 0; deviceIndex < deviceCount; ++deviceIndex)
    {
        MIDIOUTCAPSW caps {};
        MMRESULT result = midiOutGetDevCapsW(deviceIndex, &caps, sizeof(caps));
        if (result == MMSYSERR_NOERROR)
        {
            MidiOutPort port;
            port.id = static_cast<int>(deviceIndex);
            port.name.assign(caps.szPname);
            ports.push_back(std::move(port));
        }
    }
    return ports;
}
