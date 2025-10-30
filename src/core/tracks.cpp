#include "core/tracks.h"
#include "core/sample_loader.h"
#include "core/sequencer.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
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
constexpr float kMinFormant = 0.0f;
constexpr float kMaxFormant = 1.0f;
constexpr float kDefaultFormant = 0.5f;
constexpr float kMinFeedback = 0.0f;
constexpr float kMaxFeedback = 1.0f;
constexpr float kDefaultFeedback = 0.0f;
constexpr float kMinPitch = -24.0f;
constexpr float kMaxPitch = 24.0f;
constexpr float kDefaultPitch = 0.0f;
constexpr float kMinPitchRange = 1.0f;
constexpr float kMaxPitchRange = 24.0f;
constexpr float kDefaultPitchRange = 12.0f;
constexpr int kMinMidiNote = 0;
constexpr int kMaxMidiNote = 127;
constexpr int kDefaultMidiNote = 69; // A4

int clampMidiNote(int note)
{
    return std::clamp(note, kMinMidiNote, kMaxMidiNote);
}

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
        track.formant = kDefaultFormant;
        track.feedback = kDefaultFeedback;
        track.pitch = kDefaultPitch;
        track.pitchRange = kDefaultPitchRange;

        stepCount.store(kSequencerStepsPerPage, std::memory_order_relaxed);
        for (int i = 0; i < kMaxSequencerSteps; ++i)
        {
            bool enabled = (i < kSequencerStepsPerPage) ? (i % 4 == 0) : false;
            steps[i].store(enabled, std::memory_order_relaxed);
            notes[i].store(kDefaultMidiNote, std::memory_order_relaxed);
            if (enabled)
            {
                stepNotes[i].push_back(kDefaultMidiNote);
            }
            stepVelocity[i].store(kTrackStepVelocityMax, std::memory_order_relaxed);
            stepPan[i].store(0.0f, std::memory_order_relaxed);
            stepPitch[i].store(0.0f, std::memory_order_relaxed);
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
    std::atomic<float> formant{kDefaultFormant};
    std::atomic<float> feedback{kDefaultFeedback};
    std::atomic<float> pitch{kDefaultPitch};
    std::atomic<float> pitchRange{kDefaultPitchRange};
    std::array<std::atomic<bool>, kMaxSequencerSteps> steps{};
    std::array<std::atomic<int>, kMaxSequencerSteps> notes{};
    std::array<std::vector<int>, kMaxSequencerSteps> stepNotes{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepVelocity{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepPan{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepPitch{};
    std::atomic<int> stepCount{1};
    std::shared_ptr<const SampleBuffer> sampleBuffer;
    std::mutex noteMutex;
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
    baseTrack.formant = kDefaultFormant;
    baseTrack.feedback = kDefaultFeedback;
    baseTrack.pitch = kDefaultPitch;
    baseTrack.pitchRange = kDefaultPitchRange;
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
        info.formant = track->formant.load(std::memory_order_relaxed);
        info.feedback = track->feedback.load(std::memory_order_relaxed);
        info.pitch = track->pitch.load(std::memory_order_relaxed);
        info.pitchRange = track->pitchRange.load(std::memory_order_relaxed);
        result.push_back(std::move(info));
    }
    return result;
}

size_t getTrackCount()
{
    std::shared_lock<std::shared_mutex> lock(gTrackMutex);
    return gTracks.size();
}

void trackSetName(int trackId, const std::string& name)
{
    std::unique_lock<std::shared_mutex> lock(gTrackMutex);
    for (auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            track->track.name = name;
            if (track->track.name.empty())
            {
                track->track.name = "Track " + std::to_string(track->track.id);
            }
            return;
        }
    }
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

    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        auto& notes = track->stepNotes[stepIndex];
        if (!enabled)
        {
            notes.clear();
        }
        else if (notes.empty())
        {
            int note = track->notes[stepIndex].load(std::memory_order_relaxed);
            notes.push_back(clampMidiNote(note));
        }
    }

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
    trackSetStepState(trackId, stepIndex, !current);
}

int trackGetStepNote(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return kDefaultMidiNote;

    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultMidiNote;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return kDefaultMidiNote;

    int note = track->notes[stepIndex].load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        const auto& notes = track->stepNotes[stepIndex];
        if (!notes.empty())
        {
            note = notes.front();
        }
    }
    return clampMidiNote(note);
}

void trackSetStepNote(int trackId, int stepIndex, int midiNote)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    int clamped = clampMidiNote(midiNote);
    track->notes[stepIndex].store(clamped, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        auto& notes = track->stepNotes[stepIndex];
        notes.clear();
        notes.push_back(clamped);
    }
    track->steps[stepIndex].store(true, std::memory_order_relaxed);
}

std::vector<int> trackGetStepNotes(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return {};

    auto track = findTrackData(trackId);
    if (!track)
        return {};

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return {};

    std::lock_guard<std::mutex> lock(track->noteMutex);
    std::vector<int> notes = track->stepNotes[stepIndex];
    for (int& note : notes)
    {
        note = clampMidiNote(note);
    }
    notes.erase(std::remove_if(notes.begin(), notes.end(), [](int value) {
        return value < kMinMidiNote || value > kMaxMidiNote;
    }), notes.end());
    return notes;
}

void trackToggleStepNote(int trackId, int stepIndex, int midiNote)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    int clamped = clampMidiNote(midiNote);
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        auto& notes = track->stepNotes[stepIndex];
        auto it = std::find(notes.begin(), notes.end(), clamped);
        if (it != notes.end())
        {
            notes.erase(it);
        }
        else
        {
            notes.push_back(clamped);
            std::sort(notes.begin(), notes.end());
            notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
        }

        bool enabled = !notes.empty();
        if (enabled)
        {
            track->notes[stepIndex].store(notes.front(), std::memory_order_relaxed);
        }
        else
        {
            track->notes[stepIndex].store(kDefaultMidiNote, std::memory_order_relaxed);
        }
        track->steps[stepIndex].store(enabled, std::memory_order_relaxed);
    }
}

float trackGetStepVelocity(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return kTrackStepVelocityMax;

    auto track = findTrackData(trackId);
    if (!track)
        return kTrackStepVelocityMax;

    float value = track->stepVelocity[stepIndex].load(std::memory_order_relaxed);
    return std::clamp(value, kTrackStepVelocityMin, kTrackStepVelocityMax);
}

void trackSetStepVelocity(int trackId, int stepIndex, float value)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kTrackStepVelocityMin, kTrackStepVelocityMax);
    track->stepVelocity[stepIndex].store(clamped, std::memory_order_relaxed);
}

float trackGetStepPan(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return 0.0f;

    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float value = track->stepPan[stepIndex].load(std::memory_order_relaxed);
    return std::clamp(value, kTrackStepPanMin, kTrackStepPanMax);
}

void trackSetStepPan(int trackId, int stepIndex, float value)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kTrackStepPanMin, kTrackStepPanMax);
    track->stepPan[stepIndex].store(clamped, std::memory_order_relaxed);
}

float trackGetStepPitchOffset(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return 0.0f;

    auto track = findTrackData(trackId);
    if (!track)
        return 0.0f;

    float value = track->stepPitch[stepIndex].load(std::memory_order_relaxed);
    return std::clamp(value, kTrackStepPitchMin, kTrackStepPitchMax);
}

void trackSetStepPitchOffset(int trackId, int stepIndex, float value)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kTrackStepPitchMin, kTrackStepPitchMax);
    track->stepPitch[stepIndex].store(clamped, std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(track->noteMutex);
        for (int i = previous; i < clamped; ++i)
        {
            track->steps[i].store(false, std::memory_order_relaxed);
            track->notes[i].store(kDefaultMidiNote, std::memory_order_relaxed);
            track->stepNotes[i].clear();
            track->stepVelocity[i].store(kTrackStepVelocityMax, std::memory_order_relaxed);
            track->stepPan[i].store(0.0f, std::memory_order_relaxed);
            track->stepPitch[i].store(0.0f, std::memory_order_relaxed);
        }
    }
    else
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        for (int i = clamped; i < previous; ++i)
        {
            track->steps[i].store(false, std::memory_order_relaxed);
            track->notes[i].store(kDefaultMidiNote, std::memory_order_relaxed);
            track->stepNotes[i].clear();
            track->stepVelocity[i].store(kTrackStepVelocityMax, std::memory_order_relaxed);
            track->stepPan[i].store(0.0f, std::memory_order_relaxed);
            track->stepPitch[i].store(0.0f, std::memory_order_relaxed);
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

float trackGetSynthFormant(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultFormant;

    float value = track->formant.load(std::memory_order_relaxed);
    return std::clamp(value, kMinFormant, kMaxFormant);
}

void trackSetSynthFormant(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinFormant, kMaxFormant);
    track->formant.store(clamped, std::memory_order_relaxed);
}

float trackGetSynthFeedback(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultFeedback;

    float value = track->feedback.load(std::memory_order_relaxed);
    return std::clamp(value, kMinFeedback, kMaxFeedback);
}

void trackSetSynthFeedback(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinFeedback, kMaxFeedback);
    track->feedback.store(clamped, std::memory_order_relaxed);
}

float trackGetSynthPitch(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultPitch;

    float value = track->pitch.load(std::memory_order_relaxed);
    return std::clamp(value, kMinPitch, kMaxPitch);
}

void trackSetSynthPitch(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinPitch, kMaxPitch);
    float quantized = static_cast<float>(std::lround(clamped));
    track->pitch.store(quantized, std::memory_order_relaxed);
}

float trackGetSynthPitchRange(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultPitchRange;

    float value = track->pitchRange.load(std::memory_order_relaxed);
    return std::clamp(value, kMinPitchRange, kMaxPitchRange);
}

void trackSetSynthPitchRange(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinPitchRange, kMaxPitchRange);
    float quantized = static_cast<float>(std::lround(clamped));
    if (quantized < kMinPitchRange)
        quantized = kMinPitchRange;
    track->pitchRange.store(quantized, std::memory_order_relaxed);
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
