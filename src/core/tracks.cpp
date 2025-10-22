#include "core/tracks.h"

#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{
std::vector<Track> gTracks;
std::mutex gTrackMutex;
int gNextTrackId = 1;
}

void initTracks()
{
    std::lock_guard<std::mutex> lock(gTrackMutex);
    gTracks.clear();
    gNextTrackId = 1;
    Track track;
    track.id = gNextTrackId++;
    track.name = "Track " + std::to_string(track.id);
    gTracks.push_back(std::move(track));
}

Track addTrack(const std::string& name)
{
    std::lock_guard<std::mutex> lock(gTrackMutex);
    Track track;
    track.id = gNextTrackId++;
    track.name = name.empty() ? "Track " + std::to_string(track.id) : name;
    gTracks.push_back(track);
    return track;
}

std::vector<Track> getTracks()
{
    std::lock_guard<std::mutex> lock(gTrackMutex);
    return gTracks;
}

size_t getTrackCount()
{
    std::lock_guard<std::mutex> lock(gTrackMutex);
    return gTracks.size();
}
