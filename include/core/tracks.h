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
};

void initTracks();
Track addTrack(const std::string& name = {});
std::vector<Track> getTracks();
size_t getTrackCount();

TrackType trackGetType(int trackId);
void trackSetType(int trackId, TrackType type);

SynthWaveType trackGetSynthWaveType(int trackId);
void trackSetSynthWaveType(int trackId, SynthWaveType type);

bool trackGetStepState(int trackId, int stepIndex);
void trackSetStepState(int trackId, int stepIndex, bool enabled);
void trackToggleStepState(int trackId, int stepIndex);
int trackGetStepCount(int trackId);
void trackSetStepCount(int trackId, int count);

std::shared_ptr<const SampleBuffer> trackGetSampleBuffer(int trackId);
void trackSetSampleBuffer(int trackId, std::shared_ptr<const SampleBuffer> buffer);
