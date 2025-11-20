#include "core/track_type_synth.h"
#include "core/tracks.h"

// Legacy synth track logic has been moved to track_type_synth_legacy_unused.cpp
// to keep the active implementation minimal now that synth tracks are removed.

SynthWaveType trackGetSynthWaveType(int)
{
    return SynthWaveType::Sine;
}

void trackSetSynthWaveType(int, SynthWaveType)
{
}

float trackGetSynthFormant(int)
{
    return 0.5f;
}

void trackSetSynthFormant(int, float)
{
}

float trackGetSynthResonance(int)
{
    return 0.2f;
}

void trackSetSynthResonance(int, float)
{
}

float trackGetSynthFeedback(int)
{
    return 0.0f;
}

void trackSetSynthFeedback(int, float)
{
}

float trackGetSynthPitch(int)
{
    return 0.0f;
}

void trackSetSynthPitch(int, float)
{
}

float trackGetSynthPitchRange(int)
{
    return 12.0f;
}

void trackSetSynthPitchRange(int, float)
{
}

float trackGetSynthAttack(int)
{
    return 0.01f;
}

void trackSetSynthAttack(int, float)
{
}

float trackGetSynthDecay(int)
{
    return 0.2f;
}

void trackSetSynthDecay(int, float)
{
}

float trackGetSynthSustain(int)
{
    return 0.8f;
}

void trackSetSynthSustain(int, float)
{
}

float trackGetSynthRelease(int)
{
    return 0.3f;
}

void trackSetSynthRelease(int, float)
{
}

bool trackGetSynthPhaseSync(int)
{
    return false;
}

void trackSetSynthPhaseSync(int, bool)
{
}

float trackGetLfoRate(int, int)
{
    return 1.0f;
}

void trackSetLfoRate(int, int, float)
{
}

LfoShape trackGetLfoShape(int, int)
{
    return LfoShape::Sine;
}

void trackSetLfoShape(int, int, LfoShape)
{
}

float trackGetLfoDeform(int, int)
{
    return 0.0f;
}

void trackSetLfoDeform(int, int, float)
{
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
