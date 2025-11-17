#include "core/track_type_sample.h"
#include "core/tracks_internal.h"

#include <algorithm>
#include <memory>
#include <shared_mutex>
#include <utility>

using namespace track_internal;

float trackGetSampleAttack(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSampleAttack;

    float value = track->sampleAttack.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSampleEnvelopeTime, kMaxSampleEnvelopeTime);
}

void trackSetSampleAttack(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSampleEnvelopeTime, kMaxSampleEnvelopeTime);
    track->sampleAttack.store(clamped, std::memory_order_relaxed);
}

float trackGetSampleRelease(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSampleRelease;

    float value = track->sampleRelease.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSampleEnvelopeTime, kMaxSampleEnvelopeTime);
}

void trackSetSampleRelease(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSampleEnvelopeTime, kMaxSampleEnvelopeTime);
    track->sampleRelease.store(clamped, std::memory_order_relaxed);
}

std::shared_ptr<const SampleBuffer> trackGetSampleBuffer(int trackId)
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    for (const auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            return track->sampleBuffer;
        }
    }
    return {};
}

void trackSetSampleBuffer(int trackId, std::shared_ptr<const SampleBuffer> buffer)
{
    std::unique_lock<std::shared_mutex> lock(gTrackMutex);
    for (auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            track->sampleBuffer = std::move(buffer);
            return;
        }
    }
}

