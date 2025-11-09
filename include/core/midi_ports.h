#pragma once

#include <string>
#include <vector>

struct MidiOutPort
{
    int id = -1;
    std::wstring name;
};

std::vector<MidiOutPort> getAvailableMidiOutPorts();
