#include "core/mod_matrix_parameters.h"

#include "core/tracks.h"

#include <algorithm>
#include <array>

namespace
{

constexpr uint32_t kTrackTypeMaskAll = modMatrixTrackTypeToMask(TrackType::Synth) |
                                        modMatrixTrackTypeToMask(TrackType::Sample) |
                                        modMatrixTrackTypeToMask(TrackType::MidiOut) |
                                        modMatrixTrackTypeToMask(TrackType::VST);
constexpr uint32_t kTrackTypeMaskSynth = modMatrixTrackTypeToMask(TrackType::Synth);
constexpr uint32_t kTrackTypeMaskSample = modMatrixTrackTypeToMask(TrackType::Sample);

struct ModParameterEntry
{
    ModMatrixParameter id;
    ModParameterInfo info;
};

constexpr std::array<ModParameterEntry, 16> kModParameters = {
    ModParameterEntry{ModMatrixParameter::Volume,
                      ModParameterInfo{L"Volume", trackGetVolume, trackSetVolume, 0.0f, 1.0f, kTrackTypeMaskAll}},
    ModParameterEntry{ModMatrixParameter::Pan,
                      ModParameterInfo{L"Pan", trackGetPan, trackSetPan, -1.0f, 1.0f, kTrackTypeMaskAll}},
    ModParameterEntry{ModMatrixParameter::SynthPitch,
                      ModParameterInfo{L"Synth Pitch", trackGetSynthPitch, trackSetSynthPitch, -12.0f, 12.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthFormant,
                      ModParameterInfo{L"Synth Formant", trackGetSynthFormant, trackSetSynthFormant, 0.0f, 1.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthResonance,
                      ModParameterInfo{L"Synth Resonance", trackGetSynthResonance, trackSetSynthResonance, 0.0f, 1.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthFeedback,
                      ModParameterInfo{L"Synth Feedback", trackGetSynthFeedback, trackSetSynthFeedback, 0.0f, 1.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthPitchRange,
                      ModParameterInfo{L"Synth Pitch Range", trackGetSynthPitchRange, trackSetSynthPitchRange, 1.0f, 24.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthAttack,
                      ModParameterInfo{L"Synth Attack", trackGetSynthAttack, trackSetSynthAttack, 0.0f, 4.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthDecay,
                      ModParameterInfo{L"Synth Decay", trackGetSynthDecay, trackSetSynthDecay, 0.0f, 4.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthSustain,
                      ModParameterInfo{L"Synth Sustain", trackGetSynthSustain, trackSetSynthSustain, 0.0f, 1.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SynthRelease,
                      ModParameterInfo{L"Synth Release", trackGetSynthRelease, trackSetSynthRelease, 0.0f, 4.0f,
                                       kTrackTypeMaskSynth}},
    ModParameterEntry{ModMatrixParameter::SampleAttack,
                      ModParameterInfo{L"Sample Attack", trackGetSampleAttack, trackSetSampleAttack, 0.0f, 4.0f,
                                       kTrackTypeMaskSample}},
    ModParameterEntry{ModMatrixParameter::SampleRelease,
                      ModParameterInfo{L"Sample Release", trackGetSampleRelease, trackSetSampleRelease, 0.0f, 4.0f,
                                       kTrackTypeMaskSample}},
    ModParameterEntry{ModMatrixParameter::DelayMix,
                      ModParameterInfo{L"Delay Mix", trackGetDelayMix, trackSetDelayMix, 0.0f, 1.0f, kTrackTypeMaskAll}},
    ModParameterEntry{ModMatrixParameter::CompressorThreshold,
                      ModParameterInfo{L"Compressor Threshold", trackGetCompressorThresholdDb, trackSetCompressorThresholdDb,
                                       -60.0f, 0.0f, kTrackTypeMaskAll}},
    ModParameterEntry{ModMatrixParameter::CompressorRatio,
                      ModParameterInfo{L"Compressor Ratio", trackGetCompressorRatio, trackSetCompressorRatio, 1.0f, 20.0f,
                                       kTrackTypeMaskAll}},
};

} // namespace

const ModParameterInfo* modMatrixGetParameterInfo(int index)
{
    if (index < 0 || index >= static_cast<int>(kModParameters.size()))
        return nullptr;
    return &kModParameters[static_cast<size_t>(index)].info;
}

int modMatrixGetParameterCount()
{
    return static_cast<int>(kModParameters.size());
}

int modMatrixGetParameterIndex(ModMatrixParameter parameter)
{
    for (size_t i = 0; i < kModParameters.size(); ++i)
    {
        if (kModParameters[i].id == parameter)
            return static_cast<int>(i);
    }
    return -1;
}

bool modMatrixParameterSupportsTrackType(const ModParameterInfo& info, TrackType trackType)
{
    uint32_t mask = modMatrixTrackTypeToMask(trackType);
    return (info.trackTypeMask & mask) != 0;
}

float modMatrixClampNormalized(float normalized)
{
    return std::clamp(normalized, -1.0f, 1.0f);
}

float modMatrixNormalizedToValue(float normalized, const ModParameterInfo& info)
{
    float depth = modMatrixClampNormalized(normalized);
    float range = info.maxValue - info.minValue;
    return depth * range;
}

float modMatrixValueToNormalized(float value, const ModParameterInfo& info)
{
    float range = info.maxValue - info.minValue;
    if (range <= 0.0f)
        return 0.0f;

    float clamped = std::clamp(value, -range, range);
    return modMatrixClampNormalized(clamped / range);
}

