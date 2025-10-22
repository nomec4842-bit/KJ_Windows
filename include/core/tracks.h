#pragma once

#include <cstddef>
#include <string>
#include <vector>

struct Track
{
    int id;
    std::string name;
};

void initTracks();
Track addTrack(const std::string& name = {});
std::vector<Track> getTracks();
size_t getTrackCount();
