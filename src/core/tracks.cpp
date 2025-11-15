#include "core/tracks.h"
#include "core/sample_loader.h"
#include "core/sequencer.h"

#include "hosting/VST3Host.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
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
constexpr float kMinDelayTimeMs = 10.0f;
constexpr float kMaxDelayTimeMs = 2000.0f;
constexpr float kDefaultDelayTimeMs = 350.0f;
constexpr float kMinDelayFeedback = 0.0f;
constexpr float kMaxDelayFeedback = 0.95f;
constexpr float kDefaultDelayFeedback = 0.35f;
constexpr float kMinDelayMix = 0.0f;
constexpr float kMaxDelayMix = 1.0f;
constexpr float kDefaultDelayMix = 0.4f;
constexpr float kMinCompressorThresholdDb = -60.0f;
constexpr float kMaxCompressorThresholdDb = 0.0f;
constexpr float kDefaultCompressorThresholdDb = -12.0f;
constexpr float kMinCompressorRatio = 1.0f;
constexpr float kMaxCompressorRatio = 20.0f;
constexpr float kDefaultCompressorRatio = 4.0f;
constexpr float kMinCompressorAttack = 0.001f;
constexpr float kMaxCompressorAttack = 1.0f;
constexpr float kDefaultCompressorAttack = 0.01f;
constexpr float kMinCompressorRelease = 0.01f;
constexpr float kMaxCompressorRelease = 4.0f;
constexpr float kDefaultCompressorRelease = 0.2f;
constexpr float kMinSidechainAmount = 0.0f;
constexpr float kMaxSidechainAmount = 1.0f;
constexpr float kDefaultSidechainAmount = 1.0f;
constexpr float kDefaultSidechainAttack = 0.01f;
constexpr float kDefaultSidechainRelease = 0.3f;
constexpr int kDefaultSidechainSourceTrack = -1;
constexpr float kMinFormant = 0.0f;
constexpr float kMaxFormant = 1.0f;
constexpr float kDefaultFormant = 0.5f;
constexpr float kMinResonance = 0.0f;
constexpr float kMaxResonance = 1.0f;
constexpr float kDefaultResonance = 0.2f;
constexpr float kMinFeedback = 0.0f;
constexpr float kMaxFeedback = 1.0f;
constexpr float kDefaultFeedback = 0.0f;
constexpr float kMinPitch = -24.0f;
constexpr float kMaxPitch = 24.0f;
constexpr float kDefaultPitch = 0.0f;
constexpr float kMinPitchRange = 1.0f;
constexpr float kMaxPitchRange = 24.0f;
constexpr float kDefaultPitchRange = 12.0f;
constexpr float kMinSynthEnvelopeTime = 0.0f;
constexpr float kMaxSynthEnvelopeTime = 4.0f;
constexpr float kMinSynthSustain = 0.0f;
constexpr float kMaxSynthSustain = 1.0f;
constexpr float kDefaultSynthAttack = 0.01f;
constexpr float kDefaultSynthDecay = 0.2f;
constexpr float kDefaultSynthSustain = 0.8f;
constexpr float kDefaultSynthRelease = 0.3f;
constexpr float kMinSampleEnvelopeTime = 0.0f;
constexpr float kMaxSampleEnvelopeTime = 4.0f;
constexpr float kDefaultSampleAttack = 0.005f;
constexpr float kDefaultSampleRelease = 0.3f;
constexpr float kMinLfoRateHz = 0.05f;
constexpr float kMaxLfoRateHz = 20.0f;
constexpr float kDefaultLfoDeform = 0.0f;
constexpr std::array<float, 3> kDefaultLfoRatesHz = {0.5f, 1.0f, 2.0f};
constexpr std::array<LfoShape, 3> kDefaultLfoShapes = {LfoShape::Sine, LfoShape::Sine, LfoShape::Sine};
constexpr int kMinMidiNote = 0;
constexpr int kMaxMidiNote = 127;
constexpr int kDefaultMidiNote = 69; // A4
constexpr int kMinMidiChannel = 1;
constexpr int kMaxMidiChannel = 16;
constexpr int kDefaultMidiChannel = 1;
constexpr int kDefaultMidiPort = -1;

int clampMidiNote(int note)
{
    return std::clamp(note, kMinMidiNote, kMaxMidiNote);
}

float clampLfoRate(float value)
{
    return std::clamp(value, kMinLfoRateHz, kMaxLfoRateHz);
}

const char* lfoShapeToString(LfoShape shape)
{
    switch (shape)
    {
    case LfoShape::Sine:
        return "sine";
    case LfoShape::Triangle:
        return "triangle";
    case LfoShape::Saw:
        return "saw";
    case LfoShape::Square:
        return "square";
    }

    return "sine";
}

LfoShape lfoShapeFromString(const std::string& text)
{
    if (text == "triangle")
        return LfoShape::Triangle;
    if (text == "saw")
        return LfoShape::Saw;
    if (text == "square")
        return LfoShape::Square;
    return LfoShape::Sine;
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
        track.eqEnabled = true;
        track.delayEnabled = false;
        track.delayTimeMs = kDefaultDelayTimeMs;
        track.delayFeedback = kDefaultDelayFeedback;
        track.delayMix = kDefaultDelayMix;
        track.compressorEnabled = false;
        track.compressorThresholdDb = kDefaultCompressorThresholdDb;
        track.compressorRatio = kDefaultCompressorRatio;
        track.compressorAttack = kDefaultCompressorAttack;
        track.compressorRelease = kDefaultCompressorRelease;
        track.sidechainEnabled = false;
        track.sidechainSourceTrackId = kDefaultSidechainSourceTrack;
        track.sidechainAmount = kDefaultSidechainAmount;
        track.sidechainAttack = kDefaultSidechainAttack;
        track.sidechainRelease = kDefaultSidechainRelease;
        track.formant = kDefaultFormant;
        track.resonance = kDefaultResonance;
        track.feedback = kDefaultFeedback;
        track.pitch = kDefaultPitch;
        track.pitchRange = kDefaultPitchRange;
        track.synthAttack = kDefaultSynthAttack;
        track.synthDecay = kDefaultSynthDecay;
        track.synthSustain = kDefaultSynthSustain;
        track.synthRelease = kDefaultSynthRelease;
        track.synthPhaseSync = false;
        track.sampleAttack = kDefaultSampleAttack;
        track.sampleRelease = kDefaultSampleRelease;
        for (size_t i = 0; i < track.lfoSettings.size(); ++i)
        {
            track.lfoSettings[i].rateHz = kDefaultLfoRatesHz[i];
            track.lfoSettings[i].shape = kDefaultLfoShapes[i];
            track.lfoSettings[i].deform = kDefaultLfoDeform;
        }
        track.midiChannel = kDefaultMidiChannel;
        track.midiPort = kDefaultMidiPort;
        track.midiPortName.clear();
        vstHost.reset();
        track.vstHost = vstHost;

        stepCount.store(kSequencerStepsPerPage, std::memory_order_relaxed);
        for (size_t i = 0; i < lfoRateHz.size(); ++i)
        {
            lfoRateHz[i].store(kDefaultLfoRatesHz[i], std::memory_order_relaxed);
            lfoShape[i].store(kDefaultLfoShapes[i], std::memory_order_relaxed);
            lfoDeform[i].store(kDefaultLfoDeform, std::memory_order_relaxed);
        }
        for (int i = 0; i < kMaxSequencerSteps; ++i)
        {
            bool enabled = (i < kSequencerStepsPerPage) ? (i % 4 == 0) : false;
            steps[i].store(enabled, std::memory_order_relaxed);
            notes[i].store(kDefaultMidiNote, std::memory_order_relaxed);
            if (enabled)
            {
                StepNoteEntry entry{};
                entry.midiNote = kDefaultMidiNote;
                entry.velocity = kTrackStepVelocityMax;
                entry.sustain = false;
                stepNotes[i].push_back(entry);
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
    std::atomic<bool> eqEnabled{true};
    std::atomic<bool> delayEnabled{false};
    std::atomic<float> delayTimeMs{kDefaultDelayTimeMs};
    std::atomic<float> delayFeedback{kDefaultDelayFeedback};
    std::atomic<float> delayMix{kDefaultDelayMix};
    std::atomic<bool> compressorEnabled{false};
    std::atomic<float> compressorThresholdDb{kDefaultCompressorThresholdDb};
    std::atomic<float> compressorRatio{kDefaultCompressorRatio};
    std::atomic<float> compressorAttack{kDefaultCompressorAttack};
    std::atomic<float> compressorRelease{kDefaultCompressorRelease};
    std::atomic<bool> sidechainEnabled{false};
    std::atomic<int> sidechainSourceTrackId{kDefaultSidechainSourceTrack};
    std::atomic<float> sidechainAmount{kDefaultSidechainAmount};
    std::atomic<float> sidechainAttack{kDefaultSidechainAttack};
    std::atomic<float> sidechainRelease{kDefaultSidechainRelease};
    std::atomic<float> formant{kDefaultFormant};
    std::atomic<float> resonance{kDefaultResonance};
    std::atomic<float> feedback{kDefaultFeedback};
    std::atomic<float> pitch{kDefaultPitch};
    std::atomic<float> pitchRange{kDefaultPitchRange};
    std::atomic<float> synthAttack{kDefaultSynthAttack};
    std::atomic<float> synthDecay{kDefaultSynthDecay};
    std::atomic<float> synthSustain{kDefaultSynthSustain};
    std::atomic<float> synthRelease{kDefaultSynthRelease};
    std::atomic<bool> synthPhaseSync{false};
    std::atomic<float> sampleAttack{kDefaultSampleAttack};
    std::atomic<float> sampleRelease{kDefaultSampleRelease};
    std::array<std::atomic<float>, kDefaultLfoRatesHz.size()> lfoRateHz;
    std::array<std::atomic<LfoShape>, kDefaultLfoShapes.size()> lfoShape;
    std::array<std::atomic<float>, kDefaultLfoRatesHz.size()> lfoDeform;
    std::array<std::atomic<bool>, kMaxSequencerSteps> steps{};
    std::array<std::atomic<int>, kMaxSequencerSteps> notes{};
    struct StepNoteEntry
    {
        int midiNote = kDefaultMidiNote;
        float velocity = kTrackStepVelocityMax;
        bool sustain = false;
    };
    std::array<std::vector<StepNoteEntry>, kMaxSequencerSteps> stepNotes{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepVelocity{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepPan{};
    std::array<std::atomic<float>, kMaxSequencerSteps> stepPitch{};
    std::atomic<int> stepCount{1};
    std::atomic<int> maxInitializedStepCount{kSequencerStepsPerPage};
    std::shared_ptr<const SampleBuffer> sampleBuffer;
    std::shared_ptr<kj::VST3Host> vstHost;
    std::mutex noteMutex;
    std::atomic<int> midiChannel{kDefaultMidiChannel};
    std::atomic<int> midiPort{kDefaultMidiPort};
    std::wstring midiPortName;
    std::mutex midiPortMutex;
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
    baseTrack.eqEnabled = true;
    baseTrack.delayEnabled = false;
    baseTrack.delayTimeMs = kDefaultDelayTimeMs;
    baseTrack.delayFeedback = kDefaultDelayFeedback;
    baseTrack.delayMix = kDefaultDelayMix;
    baseTrack.compressorEnabled = false;
    baseTrack.compressorThresholdDb = kDefaultCompressorThresholdDb;
    baseTrack.compressorRatio = kDefaultCompressorRatio;
    baseTrack.compressorAttack = kDefaultCompressorAttack;
    baseTrack.compressorRelease = kDefaultCompressorRelease;
    baseTrack.sidechainEnabled = false;
    baseTrack.sidechainSourceTrackId = kDefaultSidechainSourceTrack;
    baseTrack.sidechainAmount = kDefaultSidechainAmount;
    baseTrack.sidechainAttack = kDefaultSidechainAttack;
    baseTrack.sidechainRelease = kDefaultSidechainRelease;
    baseTrack.formant = kDefaultFormant;
    baseTrack.resonance = kDefaultResonance;
    baseTrack.feedback = kDefaultFeedback;
    baseTrack.pitch = kDefaultPitch;
    baseTrack.pitchRange = kDefaultPitchRange;
    baseTrack.synthAttack = kDefaultSynthAttack;
    baseTrack.synthDecay = kDefaultSynthDecay;
    baseTrack.synthSustain = kDefaultSynthSustain;
    baseTrack.synthRelease = kDefaultSynthRelease;
    baseTrack.sampleAttack = kDefaultSampleAttack;
    baseTrack.sampleRelease = kDefaultSampleRelease;
    baseTrack.midiChannel = kDefaultMidiChannel;
    baseTrack.midiPort = kDefaultMidiPort;
    baseTrack.midiPortName.clear();
    baseTrack.vstHost.reset();
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
    result.vstHost = trackData->vstHost;
    result.midiPort = trackData->midiPort.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(trackData->midiPortMutex);
        result.midiPortName = trackData->midiPortName;
    }
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
        info.eqEnabled = track->eqEnabled.load(std::memory_order_relaxed);
        info.delayEnabled = track->delayEnabled.load(std::memory_order_relaxed);
        info.delayTimeMs = track->delayTimeMs.load(std::memory_order_relaxed);
        info.delayFeedback = track->delayFeedback.load(std::memory_order_relaxed);
        info.delayMix = track->delayMix.load(std::memory_order_relaxed);
        info.compressorEnabled = track->compressorEnabled.load(std::memory_order_relaxed);
        info.compressorThresholdDb = track->compressorThresholdDb.load(std::memory_order_relaxed);
        info.compressorRatio = track->compressorRatio.load(std::memory_order_relaxed);
        info.compressorAttack = track->compressorAttack.load(std::memory_order_relaxed);
        info.compressorRelease = track->compressorRelease.load(std::memory_order_relaxed);
        info.sidechainEnabled = track->sidechainEnabled.load(std::memory_order_relaxed);
        info.sidechainSourceTrackId = track->sidechainSourceTrackId.load(std::memory_order_relaxed);
        info.sidechainAmount = track->sidechainAmount.load(std::memory_order_relaxed);
        info.sidechainAttack = track->sidechainAttack.load(std::memory_order_relaxed);
        info.sidechainRelease = track->sidechainRelease.load(std::memory_order_relaxed);
        info.formant = track->formant.load(std::memory_order_relaxed);
        info.resonance = track->resonance.load(std::memory_order_relaxed);
        info.feedback = track->feedback.load(std::memory_order_relaxed);
        info.pitch = track->pitch.load(std::memory_order_relaxed);
        info.pitchRange = track->pitchRange.load(std::memory_order_relaxed);
        info.synthAttack = track->synthAttack.load(std::memory_order_relaxed);
        info.synthDecay = track->synthDecay.load(std::memory_order_relaxed);
        info.synthSustain = track->synthSustain.load(std::memory_order_relaxed);
        info.synthRelease = track->synthRelease.load(std::memory_order_relaxed);
        info.synthPhaseSync = track->synthPhaseSync.load(std::memory_order_relaxed);
        info.sampleAttack = track->sampleAttack.load(std::memory_order_relaxed);
        info.sampleRelease = track->sampleRelease.load(std::memory_order_relaxed);
        for (size_t i = 0; i < info.lfoSettings.size(); ++i)
        {
            info.lfoSettings[i].rateHz = track->lfoRateHz[i].load(std::memory_order_relaxed);
            info.lfoSettings[i].shape = track->lfoShape[i].load(std::memory_order_relaxed);
            info.lfoSettings[i].deform = track->lfoDeform[i].load(std::memory_order_relaxed);
        }
        info.midiChannel = track->midiChannel.load(std::memory_order_relaxed);
        info.midiPort = track->midiPort.load(std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(track->midiPortMutex);
            info.midiPortName = track->midiPortName;
        }
        info.vstHost = track->vstHost;
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
            TrackData::StepNoteEntry entry{};
            entry.midiNote = clampMidiNote(note);
            entry.velocity = track->stepVelocity[stepIndex].load(std::memory_order_relaxed);
            entry.sustain = false;
            notes.push_back(entry);
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
            note = notes.front().midiNote;
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
        TrackData::StepNoteEntry entry{};
        entry.midiNote = clamped;
        entry.velocity = track->stepVelocity[stepIndex].load(std::memory_order_relaxed);
        entry.sustain = false;
        notes.push_back(entry);
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

    std::vector<int> result;
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        const auto& entries = track->stepNotes[stepIndex];
        result.reserve(entries.size());
        for (const auto& entry : entries)
        {
            int clamped = clampMidiNote(entry.midiNote);
            if (clamped >= kMinMidiNote && clamped <= kMaxMidiNote)
            {
                result.push_back(clamped);
            }
        }
    }
    result.erase(std::remove_if(result.begin(), result.end(), [](int value) {
        return value < kMinMidiNote || value > kMaxMidiNote;
    }), result.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
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
        auto it = std::find_if(notes.begin(), notes.end(), [clamped](const TrackData::StepNoteEntry& entry) {
            return entry.midiNote == clamped;
        });
        if (it != notes.end())
        {
            notes.erase(it);
        }
        else
        {
            TrackData::StepNoteEntry entry{};
            entry.midiNote = clamped;
            entry.velocity = track->stepVelocity[stepIndex].load(std::memory_order_relaxed);
            entry.sustain = false;
            notes.push_back(entry);
            std::sort(notes.begin(), notes.end(), [](const TrackData::StepNoteEntry& a, const TrackData::StepNoteEntry& b) {
                return a.midiNote < b.midiNote;
            });
        }

        bool enabled = !notes.empty();
        if (enabled)
        {
            track->notes[stepIndex].store(notes.front().midiNote, std::memory_order_relaxed);
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
    {
        std::lock_guard<std::mutex> lock(track->noteMutex);
        auto& notes = track->stepNotes[stepIndex];
        for (auto& entry : notes)
        {
            entry.velocity = clamped;
        }
    }
}

float trackGetStepNoteVelocity(int trackId, int stepIndex, int midiNote)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return kTrackStepVelocityMax;

    auto track = findTrackData(trackId);
    if (!track)
        return kTrackStepVelocityMax;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return kTrackStepVelocityMax;

    int clampedNote = clampMidiNote(midiNote);
    float fallback = track->stepVelocity[stepIndex].load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(track->noteMutex);
    const auto& notes = track->stepNotes[stepIndex];
    auto it = std::find_if(notes.begin(), notes.end(), [clampedNote](const TrackData::StepNoteEntry& entry) {
        return entry.midiNote == clampedNote;
    });
    if (it != notes.end())
    {
        return std::clamp(it->velocity, kTrackStepVelocityMin, kTrackStepVelocityMax);
    }
    return std::clamp(fallback, kTrackStepVelocityMin, kTrackStepVelocityMax);
}

std::vector<StepNoteInfo> trackGetStepNoteInfo(int trackId, int stepIndex)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return {};

    auto track = findTrackData(trackId);
    if (!track)
        return {};

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return {};

    std::vector<StepNoteInfo> result;
    std::lock_guard<std::mutex> lock(track->noteMutex);
    const auto& notes = track->stepNotes[stepIndex];
    result.reserve(notes.size());
    for (const auto& entry : notes)
    {
        StepNoteInfo info{};
        info.midiNote = clampMidiNote(entry.midiNote);
        info.velocity = std::clamp(entry.velocity, kTrackStepVelocityMin, kTrackStepVelocityMax);
        info.sustain = entry.sustain;
        if (info.midiNote >= kMinMidiNote && info.midiNote <= kMaxMidiNote)
        {
            result.push_back(info);
        }
    }
    std::sort(result.begin(), result.end(), [](const StepNoteInfo& a, const StepNoteInfo& b) {
        if (a.midiNote != b.midiNote)
            return a.midiNote < b.midiNote;
        if (a.sustain != b.sustain)
            return a.sustain < b.sustain;
        return a.velocity < b.velocity;
    });
    result.erase(std::unique(result.begin(), result.end(), [](const StepNoteInfo& a, const StepNoteInfo& b) {
        return a.midiNote == b.midiNote;
    }), result.end());
    return result;
}

bool trackGetStepNoteSustain(int trackId, int stepIndex, int midiNote)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return false;

    auto track = findTrackData(trackId);
    if (!track)
        return false;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return false;

    int clampedNote = clampMidiNote(midiNote);
    std::lock_guard<std::mutex> lock(track->noteMutex);
    const auto& notes = track->stepNotes[stepIndex];
    auto it = std::find_if(notes.begin(), notes.end(), [clampedNote](const TrackData::StepNoteEntry& entry) {
        return entry.midiNote == clampedNote;
    });
    if (it != notes.end())
        return it->sustain;
    return false;
}

void trackSetStepNoteSustain(int trackId, int stepIndex, int midiNote, bool sustain)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    int clampedNote = clampMidiNote(midiNote);
    std::lock_guard<std::mutex> lock(track->noteMutex);
    auto& notes = track->stepNotes[stepIndex];
    auto it = std::find_if(notes.begin(), notes.end(), [clampedNote](const TrackData::StepNoteEntry& entry) {
        return entry.midiNote == clampedNote;
    });
    if (it != notes.end())
    {
        it->sustain = sustain;
    }
}

void trackSetStepNoteVelocity(int trackId, int stepIndex, int midiNote, float value)
{
    if (stepIndex < 0 || stepIndex >= kMaxSequencerSteps)
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    int stepCount = track->stepCount.load(std::memory_order_relaxed);
    if (stepIndex >= stepCount)
        return;

    int clampedNote = clampMidiNote(midiNote);
    float clampedValue = std::clamp(value, kTrackStepVelocityMin, kTrackStepVelocityMax);
    std::lock_guard<std::mutex> lock(track->noteMutex);
    auto& notes = track->stepNotes[stepIndex];
    auto it = std::find_if(notes.begin(), notes.end(), [clampedNote](const TrackData::StepNoteEntry& entry) {
        return entry.midiNote == clampedNote;
    });
    if (it != notes.end())
    {
        it->velocity = clampedValue;
        if (notes.size() == 1)
        {
            track->stepVelocity[stepIndex].store(clampedValue, std::memory_order_relaxed);
        }
    }
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
        int maxInitialized = track->maxInitializedStepCount.load(std::memory_order_relaxed);
        if (clamped > maxInitialized)
        {
            std::lock_guard<std::mutex> lock(track->noteMutex);
            for (int i = maxInitialized; i < clamped; ++i)
            {
                track->steps[i].store(false, std::memory_order_relaxed);
                track->notes[i].store(kDefaultMidiNote, std::memory_order_relaxed);
                track->stepNotes[i].clear();
                track->stepVelocity[i].store(kTrackStepVelocityMax, std::memory_order_relaxed);
                track->stepPan[i].store(0.0f, std::memory_order_relaxed);
                track->stepPitch[i].store(0.0f, std::memory_order_relaxed);
            }
            track->maxInitializedStepCount.store(clamped, std::memory_order_relaxed);
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
    std::unique_lock<std::shared_mutex> lock(gTrackMutex);
    for (auto& track : gTracks)
    {
        if (track->track.id == trackId)
        {
            track->type.store(type, std::memory_order_relaxed);
            track->track.type = type;

            if (type == TrackType::VST)
            {
                if (!track->vstHost)
                {
                    track->vstHost = std::make_shared<kj::VST3Host>();
                    std::cout << "VST track initialized" << std::endl;
                }
                track->track.vstHost = track->vstHost;
            }
            else
            {
                if (track->vstHost)
                {
                    track->vstHost->unload();
                }
                track->track.vstHost.reset();
            }
            return;
        }
    }
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
            track->track.vstHost = track->vstHost;
            return track->vstHost;
        }
    }
    return {};
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

bool trackGetEqEnabled(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return true;

    return track->eqEnabled.load(std::memory_order_relaxed);
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

void trackSetEqEnabled(int trackId, bool enabled)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->eqEnabled.store(enabled, std::memory_order_relaxed);
}

bool trackGetDelayEnabled(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return false;

    return track->delayEnabled.load(std::memory_order_relaxed);
}

void trackSetDelayEnabled(int trackId, bool enabled)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->delayEnabled.store(enabled, std::memory_order_relaxed);
}

float trackGetDelayTimeMs(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultDelayTimeMs;

    float value = track->delayTimeMs.load(std::memory_order_relaxed);
    return std::clamp(value, kMinDelayTimeMs, kMaxDelayTimeMs);
}

void trackSetDelayTimeMs(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinDelayTimeMs, kMaxDelayTimeMs);
    track->delayTimeMs.store(clamped, std::memory_order_relaxed);
}

float trackGetDelayFeedback(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultDelayFeedback;

    float value = track->delayFeedback.load(std::memory_order_relaxed);
    return std::clamp(value, kMinDelayFeedback, kMaxDelayFeedback);
}

void trackSetDelayFeedback(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinDelayFeedback, kMaxDelayFeedback);
    track->delayFeedback.store(clamped, std::memory_order_relaxed);
}

float trackGetDelayMix(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultDelayMix;

    float value = track->delayMix.load(std::memory_order_relaxed);
    return std::clamp(value, kMinDelayMix, kMaxDelayMix);
}

void trackSetDelayMix(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinDelayMix, kMaxDelayMix);
    track->delayMix.store(clamped, std::memory_order_relaxed);
}

bool trackGetCompressorEnabled(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return false;

    return track->compressorEnabled.load(std::memory_order_relaxed);
}

void trackSetCompressorEnabled(int trackId, bool enabled)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->compressorEnabled.store(enabled, std::memory_order_relaxed);
}

float trackGetCompressorThresholdDb(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultCompressorThresholdDb;

    float value = track->compressorThresholdDb.load(std::memory_order_relaxed);
    return std::clamp(value, kMinCompressorThresholdDb, kMaxCompressorThresholdDb);
}

void trackSetCompressorThresholdDb(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinCompressorThresholdDb, kMaxCompressorThresholdDb);
    track->compressorThresholdDb.store(clamped, std::memory_order_relaxed);
}

float trackGetCompressorRatio(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultCompressorRatio;

    float value = track->compressorRatio.load(std::memory_order_relaxed);
    return std::clamp(value, kMinCompressorRatio, kMaxCompressorRatio);
}

void trackSetCompressorRatio(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinCompressorRatio, kMaxCompressorRatio);
    track->compressorRatio.store(clamped, std::memory_order_relaxed);
}

float trackGetCompressorAttack(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultCompressorAttack;

    float value = track->compressorAttack.load(std::memory_order_relaxed);
    return std::clamp(value, kMinCompressorAttack, kMaxCompressorAttack);
}

void trackSetCompressorAttack(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinCompressorAttack, kMaxCompressorAttack);
    track->compressorAttack.store(clamped, std::memory_order_relaxed);
}

float trackGetCompressorRelease(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultCompressorRelease;

    float value = track->compressorRelease.load(std::memory_order_relaxed);
    return std::clamp(value, kMinCompressorRelease, kMaxCompressorRelease);
}

void trackSetCompressorRelease(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinCompressorRelease, kMaxCompressorRelease);
    track->compressorRelease.store(clamped, std::memory_order_relaxed);
}

bool trackGetSidechainEnabled(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return false;

    return track->sidechainEnabled.load(std::memory_order_relaxed);
}

void trackSetSidechainEnabled(int trackId, bool enabled)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->sidechainEnabled.store(enabled, std::memory_order_relaxed);
}

int trackGetSidechainSourceTrack(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSidechainSourceTrack;

    return track->sidechainSourceTrackId.load(std::memory_order_relaxed);
}

void trackSetSidechainSourceTrack(int trackId, int sourceTrackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->sidechainSourceTrackId.store(sourceTrackId, std::memory_order_relaxed);
}

float trackGetSidechainAmount(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSidechainAmount;

    float value = track->sidechainAmount.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSidechainAmount, kMaxSidechainAmount);
}

void trackSetSidechainAmount(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSidechainAmount, kMaxSidechainAmount);
    track->sidechainAmount.store(clamped, std::memory_order_relaxed);
}

float trackGetSidechainAttack(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSidechainAttack;

    float value = track->sidechainAttack.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
}

void trackSetSidechainAttack(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
    track->sidechainAttack.store(clamped, std::memory_order_relaxed);
}

float trackGetSidechainRelease(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSidechainRelease;

    float value = track->sidechainRelease.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
}

void trackSetSidechainRelease(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
    track->sidechainRelease.store(clamped, std::memory_order_relaxed);
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

float trackGetSynthResonance(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultResonance;

    float value = track->resonance.load(std::memory_order_relaxed);
    return std::clamp(value, kMinResonance, kMaxResonance);
}

void trackSetSynthResonance(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinResonance, kMaxResonance);
    track->resonance.store(clamped, std::memory_order_relaxed);
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

float trackGetSynthAttack(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSynthAttack;

    float value = track->synthAttack.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
}

void trackSetSynthAttack(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
    track->synthAttack.store(clamped, std::memory_order_relaxed);
}

float trackGetSynthDecay(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSynthDecay;

    float value = track->synthDecay.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
}

void trackSetSynthDecay(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
    track->synthDecay.store(clamped, std::memory_order_relaxed);
}

float trackGetSynthSustain(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSynthSustain;

    float value = track->synthSustain.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthSustain, kMaxSynthSustain);
}

void trackSetSynthSustain(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthSustain, kMaxSynthSustain);
    track->synthSustain.store(clamped, std::memory_order_relaxed);
}

float trackGetSynthRelease(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultSynthRelease;

    float value = track->synthRelease.load(std::memory_order_relaxed);
    return std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
}

void trackSetSynthRelease(int trackId, float value)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, kMinSynthEnvelopeTime, kMaxSynthEnvelopeTime);
    track->synthRelease.store(clamped, std::memory_order_relaxed);
}

bool trackGetSynthPhaseSync(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return false;

    return track->synthPhaseSync.load(std::memory_order_relaxed);
}

void trackSetSynthPhaseSync(int trackId, bool enabled)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->synthPhaseSync.store(enabled, std::memory_order_relaxed);
    track->track.synthPhaseSync = enabled;
}

float trackGetLfoRate(int trackId, int index)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoRatesHz.size()))
        return kDefaultLfoRatesHz[0];

    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultLfoRatesHz[static_cast<size_t>(index)];

    float value = track->lfoRateHz[static_cast<size_t>(index)].load(std::memory_order_relaxed);
    return clampLfoRate(value);
}

void trackSetLfoRate(int trackId, int index, float value)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoRatesHz.size()))
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = clampLfoRate(value);
    track->lfoRateHz[static_cast<size_t>(index)].store(clamped, std::memory_order_relaxed);
    track->track.lfoSettings[static_cast<size_t>(index)].rateHz = clamped;
}

LfoShape trackGetLfoShape(int trackId, int index)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoShapes.size()))
        return kDefaultLfoShapes[0];

    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultLfoShapes[static_cast<size_t>(index)];

    return track->lfoShape[static_cast<size_t>(index)].load(std::memory_order_relaxed);
}

void trackSetLfoShape(int trackId, int index, LfoShape shape)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoShapes.size()))
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    track->lfoShape[static_cast<size_t>(index)].store(shape, std::memory_order_relaxed);
    track->track.lfoSettings[static_cast<size_t>(index)].shape = shape;
}

float trackGetLfoDeform(int trackId, int index)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoRatesHz.size()))
        return kDefaultLfoDeform;

    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultLfoDeform;

    float value = track->lfoDeform[static_cast<size_t>(index)].load(std::memory_order_relaxed);
    return std::clamp(value, 0.0f, 1.0f);
}

void trackSetLfoDeform(int trackId, int index, float value)
{
    if (index < 0 || index >= static_cast<int>(kDefaultLfoRatesHz.size()))
        return;

    auto track = findTrackData(trackId);
    if (!track)
        return;

    float clamped = std::clamp(value, 0.0f, 1.0f);
    track->lfoDeform[static_cast<size_t>(index)].store(clamped, std::memory_order_relaxed);
    track->track.lfoSettings[static_cast<size_t>(index)].deform = clamped;
}

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

int trackGetMidiChannel(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultMidiChannel;

    int channel = track->midiChannel.load(std::memory_order_relaxed);
    if (channel < kMinMidiChannel || channel > kMaxMidiChannel)
        channel = kDefaultMidiChannel;
    return channel;
}

void trackSetMidiChannel(int trackId, int channel)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    int clamped = std::clamp(channel, kMinMidiChannel, kMaxMidiChannel);
    track->midiChannel.store(clamped, std::memory_order_relaxed);
    track->track.midiChannel = clamped;
}

int trackGetMidiPort(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultMidiPort;

    return track->midiPort.load(std::memory_order_relaxed);
}

std::wstring trackGetMidiPortName(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return {};

    std::lock_guard<std::mutex> lock(track->midiPortMutex);
    return track->midiPortName;
}

void trackSetMidiPort(int trackId, int portId, const std::wstring& portName)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    int sanitized = portId;
    if (sanitized < kDefaultMidiPort)
        sanitized = kDefaultMidiPort;

    track->midiPort.store(sanitized, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(track->midiPortMutex);
        track->midiPortName = portName;
    }
    track->track.midiPort = sanitized;
    track->track.midiPortName = portName;
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
