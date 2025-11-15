#pragma once

#include "core/tracks.h"

#include <cstdint>

enum class ModMatrixParameter
{
    Volume,
    Pan,
    SynthPitch,
    SynthFormant,
    SynthResonance,
    SynthFeedback,
    SynthPitchRange,
    SynthAttack,
    SynthDecay,
    SynthSustain,
    SynthRelease,
    SampleAttack,
    SampleRelease,
    DelayMix,
    CompressorThreshold,
    CompressorRatio,
};

using ModParameterGetter = float (*)(int);
using ModParameterSetter = void (*)(int, float);

struct ModParameterInfo
{
    const wchar_t* label;
    ModParameterGetter getter;
    ModParameterSetter setter;
    float minValue;
    float maxValue;
    uint32_t trackTypeMask;
};

constexpr uint32_t modMatrixTrackTypeToMask(TrackType type)
{
    return 1u << static_cast<uint32_t>(type);
}

const ModParameterInfo* modMatrixGetParameterInfo(int index);
int modMatrixGetParameterCount();
int modMatrixGetParameterIndex(ModMatrixParameter parameter);
bool modMatrixParameterSupportsTrackType(const ModParameterInfo& info, TrackType trackType);

float modMatrixClampNormalized(float normalized);
float modMatrixNormalizedToValue(float normalized, const ModParameterInfo& info);
float modMatrixValueToNormalized(float value, const ModParameterInfo& info);

