#include "core/midi_output.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <cstdint>
#include <mutex>
#include <unordered_map>

namespace
{
    std::mutex gMidiMutex;
    std::unordered_map<int, HMIDIOUT> gMidiOutPorts;

    HMIDIOUT ensurePortLocked(int portId)
    {
        if (portId < 0)
            return nullptr;

        auto it = gMidiOutPorts.find(portId);
        if (it != gMidiOutPorts.end())
            return it->second;

        HMIDIOUT handle = nullptr;
        MMRESULT result = midiOutOpen(&handle, static_cast<UINT>(portId), 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR || !handle)
            return nullptr;

        gMidiOutPorts.emplace(portId, handle);
        return handle;
    }

    void sendShortMessage(int portId, DWORD message)
    {
        if (portId < 0)
            return;

        std::lock_guard<std::mutex> lock(gMidiMutex);
        HMIDIOUT handle = ensurePortLocked(portId);
        if (!handle)
            return;

        midiOutShortMsg(handle, message);
    }

    DWORD makeShortMessage(int status, int data1, int data2)
    {
        status = std::clamp(status, 0, 0xFF);
        data1 = std::clamp(data1, 0, 0x7F);
        data2 = std::clamp(data2, 0, 0x7F);
        return static_cast<DWORD>(status | (data1 << 8) | (data2 << 16));
    }
}

void midiOutputSendNoteOn(int portId, int channel, int note, int velocity)
{
    channel = std::clamp(channel, 0, 15);
    note = std::clamp(note, 0, 127);
    velocity = std::clamp(velocity, 0, 127);
    int status = 0x90 | channel;
    sendShortMessage(portId, makeShortMessage(status, note, velocity));
}

void midiOutputSendNoteOff(int portId, int channel, int note, int velocity)
{
    channel = std::clamp(channel, 0, 15);
    note = std::clamp(note, 0, 127);
    velocity = std::clamp(velocity, 0, 127);
    int status = 0x80 | channel;
    sendShortMessage(portId, makeShortMessage(status, note, velocity));
}
