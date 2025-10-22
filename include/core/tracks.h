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

bool trackGetStepState(int trackId, int stepIndex);
void trackSetStepState(int trackId, int stepIndex, bool enabled);
void trackToggleStepState(int trackId, int stepIndex);
int trackGetStepCount(int trackId);
void trackSetStepCount(int trackId, int count);
