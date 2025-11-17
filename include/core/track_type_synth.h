#pragma once

#include "core/tracks.h"

#include <string>

SynthWaveType trackGetSynthWaveType(int trackId);
void trackSetSynthWaveType(int trackId, SynthWaveType type);

float trackGetSynthFormant(int trackId);
void trackSetSynthFormant(int trackId, float value);

float trackGetSynthResonance(int trackId);
void trackSetSynthResonance(int trackId, float value);

float trackGetSynthFeedback(int trackId);
void trackSetSynthFeedback(int trackId, float value);

float trackGetSynthPitch(int trackId);
void trackSetSynthPitch(int trackId, float value);

float trackGetSynthPitchRange(int trackId);
void trackSetSynthPitchRange(int trackId, float value);

float trackGetSynthAttack(int trackId);
void trackSetSynthAttack(int trackId, float value);

float trackGetSynthDecay(int trackId);
void trackSetSynthDecay(int trackId, float value);

float trackGetSynthSustain(int trackId);
void trackSetSynthSustain(int trackId, float value);

float trackGetSynthRelease(int trackId);
void trackSetSynthRelease(int trackId, float value);

bool trackGetSynthPhaseSync(int trackId);
void trackSetSynthPhaseSync(int trackId, bool enabled);

float trackGetLfoRate(int trackId, int index);
void trackSetLfoRate(int trackId, int index, float value);

LfoShape trackGetLfoShape(int trackId, int index);
void trackSetLfoShape(int trackId, int index, LfoShape shape);

float trackGetLfoDeform(int trackId, int index);
void trackSetLfoDeform(int trackId, int index, float value);

const char* lfoShapeToString(LfoShape shape);
LfoShape lfoShapeFromString(const std::string& text);

