#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct SampleBuffer;

enum class TrackType
{
    Synth,
    Sample,
};

enum class SynthWaveType
{
    Sine,
    Square,
    Saw,
    Triangle,
};

struct Track
{
    int id;
    std::string name;
    TrackType type = TrackType::Synth;
    SynthWaveType synthWaveType = SynthWaveType::Sine;
    float volume = 1.0f;
    float pan = 0.0f;
    float lowGainDb = 0.0f;
    float midGainDb = 0.0f;
    float highGainDb = 0.0f;
};

void initTracks();
Track addTrack(const std::string& name = {});
std::vector<Track> getTracks();
size_t getTrackCount();

TrackType trackGetType(int trackId);
void trackSetType(int trackId, TrackType type);

SynthWaveType trackGetSynthWaveType(int trackId);
void trackSetSynthWaveType(int trackId, SynthWaveType type);

float trackGetVolume(int trackId);
void trackSetVolume(int trackId, float volume);

float trackGetPan(int trackId);
void trackSetPan(int trackId, float pan);

float trackGetEqLowGain(int trackId);
float trackGetEqMidGain(int trackId);
float trackGetEqHighGain(int trackId);

void trackSetEqLowGain(int trackId, float gainDb);
void trackSetEqMidGain(int trackId, float gainDb);
void trackSetEqHighGain(int trackId, float gainDb);

bool trackGetStepState(int trackId, int stepIndex);
void trackSetStepState(int trackId, int stepIndex, bool enabled);
void trackToggleStepState(int trackId, int stepIndex);
int trackGetStepCount(int trackId);
void trackSetStepCount(int trackId, int count);

int trackGetStepNote(int trackId, int stepIndex);
void trackSetStepNote(int trackId, int stepIndex, int midiNote);

std::shared_ptr<const SampleBuffer> trackGetSampleBuffer(int trackId);
void trackSetSampleBuffer(int trackId, std::shared_ptr<const SampleBuffer> buffer);
