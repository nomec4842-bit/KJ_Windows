#pragma once

#include "core/sequencer.h"
#include "core/tracks.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <utility>
#include <vector>

namespace track_internal
{

inline constexpr float kMinVolume = 0.0f;
inline constexpr float kMaxVolume = 1.0f;
inline constexpr float kMinPan = -1.0f;
inline constexpr float kMaxPan = 1.0f;
inline constexpr float kMinEqGainDb = -12.0f;
inline constexpr float kMaxEqGainDb = 12.0f;
inline constexpr float kMinDelayTimeMs = 10.0f;
inline constexpr float kMaxDelayTimeMs = 2000.0f;
inline constexpr float kDefaultDelayTimeMs = 350.0f;
inline constexpr float kMinDelayFeedback = 0.0f;
inline constexpr float kMaxDelayFeedback = 0.95f;
inline constexpr float kDefaultDelayFeedback = 0.35f;
inline constexpr float kMinDelayMix = 0.0f;
inline constexpr float kMaxDelayMix = 1.0f;
inline constexpr float kDefaultDelayMix = 0.4f;
inline constexpr float kMinCompressorThresholdDb = -60.0f;
inline constexpr float kMaxCompressorThresholdDb = 0.0f;
inline constexpr float kDefaultCompressorThresholdDb = -12.0f;
inline constexpr float kMinCompressorRatio = 1.0f;
inline constexpr float kMaxCompressorRatio = 20.0f;
inline constexpr float kDefaultCompressorRatio = 4.0f;
inline constexpr float kMinCompressorAttack = 0.001f;
inline constexpr float kMaxCompressorAttack = 1.0f;
inline constexpr float kDefaultCompressorAttack = 0.01f;
inline constexpr float kMinCompressorRelease = 0.01f;
inline constexpr float kMaxCompressorRelease = 4.0f;
inline constexpr float kDefaultCompressorRelease = 0.2f;
inline constexpr float kMinSidechainAmount = 0.0f;
inline constexpr float kMaxSidechainAmount = 1.0f;
inline constexpr float kDefaultSidechainAmount = 1.0f;
inline constexpr float kDefaultSidechainAttack = 0.01f;
inline constexpr float kDefaultSidechainRelease = 0.3f;
inline constexpr int kDefaultSidechainSourceTrack = -1;
inline constexpr float kMinFormant = 0.0f;
inline constexpr float kMaxFormant = 1.0f;
inline constexpr float kDefaultFormant = 0.5f;
inline constexpr float kMinResonance = 0.0f;
inline constexpr float kMaxResonance = 1.0f;
inline constexpr float kDefaultResonance = 0.2f;
inline constexpr float kMinFeedback = 0.0f;
inline constexpr float kMaxFeedback = 1.0f;
inline constexpr float kDefaultFeedback = 0.0f;
inline constexpr float kMinPitch = -24.0f;
inline constexpr float kMaxPitch = 24.0f;
inline constexpr float kDefaultPitch = 0.0f;
inline constexpr float kMinPitchRange = 1.0f;
inline constexpr float kMaxPitchRange = 24.0f;
inline constexpr float kDefaultPitchRange = 12.0f;
inline constexpr float kMinSynthEnvelopeTime = 0.0f;
inline constexpr float kMaxSynthEnvelopeTime = 4.0f;
inline constexpr float kMinSynthSustain = 0.0f;
inline constexpr float kMaxSynthSustain = 1.0f;
inline constexpr float kDefaultSynthAttack = 0.01f;
inline constexpr float kDefaultSynthDecay = 0.2f;
inline constexpr float kDefaultSynthSustain = 0.8f;
inline constexpr float kDefaultSynthRelease = 0.3f;
inline constexpr float kMinSampleEnvelopeTime = 0.0f;
inline constexpr float kMaxSampleEnvelopeTime = 4.0f;
inline constexpr float kDefaultSampleAttack = 0.005f;
inline constexpr float kDefaultSampleRelease = 0.3f;
inline constexpr float kMinLfoRateHz = 0.05f;
inline constexpr float kMaxLfoRateHz = 20.0f;
inline constexpr float kDefaultLfoDeform = 0.0f;
inline constexpr std::array<float, 3> kDefaultLfoRatesHz = {0.5f, 1.0f, 2.0f};
inline constexpr std::array<LfoShape, 3> kDefaultLfoShapes = {LfoShape::Sine, LfoShape::Sine, LfoShape::Sine};
inline constexpr int kMinMidiNote = 0;
inline constexpr int kMaxMidiNote = 127;
inline constexpr int kDefaultMidiNote = 69; // A4
inline constexpr int kMinMidiChannel = 1;
inline constexpr int kMaxMidiChannel = 16;
inline constexpr int kDefaultMidiChannel = 1;
inline constexpr int kDefaultMidiPort = -1;

inline int clampMidiNote(int note)
{
    return std::clamp(note, kMinMidiNote, kMaxMidiNote);
}

inline float clampLfoRate(float value)
{
    return std::clamp(value, kMinLfoRateHz, kMaxLfoRateHz);
}

struct TrackData
{
    explicit TrackData(Track baseTrack);

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

inline TrackData::TrackData(Track baseTrack)
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

std::shared_ptr<TrackData> makeTrackData(const std::string& name);
std::shared_ptr<TrackData> findTrackData(int trackId);

extern std::vector<std::shared_ptr<TrackData>> gTracks;
extern std::shared_mutex gTrackMutex;
extern int gNextTrackId;

} // namespace track_internal

