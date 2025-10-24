#include "core/tracks.h"
#include "core/sample_loader.h"
#include "core/sequencer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace
{

constexpr float kMinVolume = 0.0f;
constexpr float kMaxVolume = 1.0f;
constexpr float kMinPan = -1.0f;
constexpr float kMaxPan = 1.0f;
constexpr float kMinEqGainDb = -12.0f;
constexpr float kMaxEqGainDb = 12.0f;

struct TrackData
{
    explicit TrackData(Track baseTrack)
        : track(std::move(baseTrack))
    {
        if (track.name.empty())
        {
            track.name = "Track " + std::to_string(track.id);
        }

        track.type = TrackType::Synth;
        track.synthWaveType = SynthWaveType::Sine;
        track.volume = 1.0f;
        track.pan = 0.0f;
        track.lowGainDb = 0.0f;
        track.midGainDb = 0.0f;
        track.highGainDb = 0.0f;

        stepCount.store(kSequencerStepsPerPage, std::memory_order_relaxed);
        for (int i = 0; i < kMaxSequencerSteps; ++i)
        {
            bool enabled = (i < kSequencerStepsPerPage) ? (i % 4 == 0) : false;
            steps[i].store(enabled, std::memory_order_relaxed);
        }
    }

    Track track;
    std::atomic<TrackType> type{TrackType::Synth};
    std::atomic<SynthWaveType> waveType{SynthWaveType::Sine};
    std::atomic<float> volume{1.0f};
    std::atomic<float> pan{0.0f};
    std::atomic<float> lowGainDb{0.0f};
    std::atomic<float> midGainDb{0.0f};
    std::atomic<float> highGainDb{0.0f};
    std::array<std::atomic<bool>, kMaxSequencerSteps> steps{};
    std::atomic<int> stepCount{1};
    std::shared_ptr<const SampleBuffer> sampleBuffer;
};

std::vector<std::shared_ptr<TrackData>> gTracks;
std::shared_mutex gTrackMutex;
int gNextTrackId = 1;

std::shared_ptr<TrackData> makeTrackData(const std::string& name)
{
    Track baseTrack;
    baseTrack.id = gNextTrackId++;
    baseTrack.name = name.empty() ? "Track " + std::to_string(baseTrack.id) : name;
    baseTrack.type = TrackType::Synth;
    baseTrack.synthWaveType = SynthWaveType::Sine;
    baseTrack.volume = 1.0f;
    baseTrack.pan = 0.0f;
    baseTrack.lowGainDb = 0.0f;
    baseTrack.midGainDb = 0.0f;
    baseTrack.highGainDb = 0.0f;
    return std::make_shared<TrackData>(std::move(baseTrack));
}

std::shared_ptr<TrackData> findTrackData(int trackId)
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    for (auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            return track;
        }
    }
    return {};
}

} // namespace

void initTracks()
{
    std::unique_lock<std::shared_mutex> lock(gTrackMutex);
    gTracks.clear();
    gNextTrackId = 1;
    gTracks.push_back(makeTrackData({}));
}

Track addTrack(const std::string& name)
{
    std::shared_ptr<TrackData> trackData;
    {
        std::unique_lock<std::shared_mutex> lock(gTrackMutex);
        trackData = makeTrackData(name);
        gTracks.push_back(trackData);
    }
    if (!trackData)
    {
        return {};
    }

    Track result = trackData->track;
    result.type = trackData->type.load(std::memory_order_relaxed);
    result.synthWaveType = trackData->waveType.load(std::memory_order_relaxed);
    return result;
}

std::vector<Track> getTracks()
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    std::vector<Track> result;
    result.reserve(gTracks.size());
    for (const auto& track : gTracks)
    {
        Track info = track->track;
        info.type = track->type.load(std::memory_order_relaxed);
        info.synthWaveType = track->waveType.load(std::memory_order_relaxed);
        info.volume = track->volume.load(std::memory_order_relaxed);
        info.pan = track->pan.load(std::memory_order_relaxed);
        info.lowGainDb = track->lowGainDb.load(std::memory_order_relaxed);
        info.midGainDb = track->midGainDb.load(std::memory_order_relaxed);
        info.highGainDb = track->highGainDb.load(std::memory_order_relaxed);
        result.push_back(std::move(info));
    }
    return result;
}

size_t getTrackCount()
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    return gTracks.size();
}

bool trackGetStepState(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return false;

    auto track = findTrackData(trackId);
    if (!track)
        return false;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return false;

    return track->steps[stepIndex].load(std::memory_order_relaxed);
}

void trackSetStepState(int trackId, int stepIndex, bool enabled)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    track->steps[stepIndex].store(enabled, std::memory_order_relaxed);
}

void trackToggleStepState(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    bool current = track->steps[stepIndex].load(std::memory_order_relaxed);
    track->steps[stepIndex].store(!current, std::memory_order_relaxed);
}

int trackGetStepCount(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 0;

    int count = track->stepCount.load(std::memory_order_relaxed);
    return std::clamp(count, 1, kMaxSequencerSteps);
}

void trackSetStepCount(int trackId, int count)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    int clamped = std::clamp(count, 1, kMaxSequencerSteps);
    int previous = track->stepCount.load(std::memory_order_relaxed);
    if (clamped == previous)
        return;

    if (clamped > previous)
    {
        for (int i = previous; i < clamped; ++i)
        {
            track->steps[i].store(false, std::memory_order_relaxed);
        }
    }
    else
    {
        for (int i = clamped; i < previous; ++i)
        {
            track->steps[i].store(false, std::memory_order_relaxed);
        }
    }

    track->stepCount.store(clamped, std::memory_order_relaxed);
}

TrackType trackGetType(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return TrackType::Synth;

    return track->type.load(std::memory_order_relaxed);
}

void trackSetType(int trackId, TrackType type)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->type.store(type, std::memory_order_relaxed);
}

SynthWaveType trackGetSynthWaveType(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return SynthWaveType::Sine;

    return track->waveType.load(std::memory_order_relaxed);
}

void trackSetSynthWaveType(int trackId, SynthWaveType type)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->waveType.store(type, std::memory_order_relaxed);
}

float trackGetVolume(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 1.0f;

    float volume = track->volume.load(std::memory_order_relaxed);
    return std::clamp(volume, kMinVolume, kMaxVolume);
}

void trackSetVolume(int trackId, float volume)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(volume, kMinVolume, kMaxVolume);
    track->volume.store(clamped, std::memory_order_relaxed);
}

float trackGetPan(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float pan = track->pan.load(std::memory_order_relaxed);
    return std::clamp(pan, kMinPan, kMaxPan);
}

void trackSetPan(int trackId, float pan)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(pan, kMinPan, kMaxPan);
    track->pan.store(clamped, std::memory_order_relaxed);
}

float trackGetEqLowGain(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float gain = track->lowGainDb.load(std::memory_order_relaxed);
    return std::clamp(gain, kMinEqGainDb, kMaxEqGainDb);
}

float trackGetEqMidGain(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float gain = track->midGainDb.load(std::memory_order_relaxed);
    return std::clamp(gain, kMinEqGainDb, kMaxEqGainDb);
}

float trackGetEqHighGain(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float gain = track->highGainDb.load(std::memory_order_relaxed);
    return std::clamp(gain, kMinEqGainDb, kMaxEqGainDb);
}

void trackSetEqLowGain(int trackId, float gainDb)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(gainDb, kMinEqGainDb, kMaxEqGainDb);
    track->lowGainDb.store(clamped, std::memory_order_relaxed);
}

void trackSetEqMidGain(int trackId, float gainDb)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(gainDb, kMinEqGainDb, kMaxEqGainDb);
    track->midGainDb.store(clamped, std::memory_order_relaxed);
}

void trackSetEqHighGain(int trackId, float gainDb)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(gainDb, kMinEqGainDb, kMaxEqGainDb);
    track->highGainDb.store(clamped, std::memory_order_relaxed);
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
