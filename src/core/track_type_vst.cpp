#include "core/track_type_vst.h"
#include "core/tracks_internal.h"

#include "hosting/VST3Host.h"

#include <iostream>
#include <memory>
#include <shared_mutex>

using namespace track_internal;

std::shared_ptr<kj::VST3Host> trackGetVstHost(int trackId)
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    for (const auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            return track->vstHost;
        }
    }
    return {};
}

std::shared_ptr<kj::VST3Host> trackEnsureVstHost(int trackId)
{
    std::unique_lock<std::shared_mutex> lock(gTrackMutex);
    for (auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            if (!track->vstHost)
            {
                track->vstHost = std::make_shared<kj::VST3Host>();
                std::cout << "VST track initialized" << std::endl;
            }
            if (track->vstHost)
                track->vstHost->setOwningTrackId(trackId);
            track->track.vstHost = track->vstHost;
            return track->vstHost;
        }
    }
    return {};
}

