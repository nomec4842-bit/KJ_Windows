// Legacy synth track implementation (unused)
// Preserved for reference as synth tracks have been removed.

#include "core/track_type_synth.h"
#include "core/tracks_internal.h"

#include <algorithm>
#include <cmath>

using namespace track_internal;

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

