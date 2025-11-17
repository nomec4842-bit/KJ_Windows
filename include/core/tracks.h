#pragma once

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

struct SampleBuffer;

namespace kj
{
class VST3Host;
}

enum class TrackType
{
    Synth,
    Sample,
    MidiOut,
    VST,
};

enum class SynthWaveType
{
    Sine,
    Square,
    Saw,
    Triangle,
};

enum class LfoShape
{
    Sine,
    Triangle,
    Saw,
    Square,
};

struct LfoSettings
{
    float rateHz = 1.0f;
    LfoShape shape = LfoShape::Sine;
    float deform = 0.0f;
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
    bool eqEnabled = true;
    bool delayEnabled = false;
    float delayTimeMs = 350.0f;
    float delayFeedback = 0.35f;
    float delayMix = 0.4f;
    bool compressorEnabled = false;
    float compressorThresholdDb = -12.0f;
    float compressorRatio = 4.0f;
    float compressorAttack = 0.01f;
    float compressorRelease = 0.2f;
    bool sidechainEnabled = false;
    int sidechainSourceTrackId = -1;
    float sidechainAmount = 1.0f;
    float sidechainAttack = 0.01f;
    float sidechainRelease = 0.3f;
    float formant = 0.5f;
    float resonance = 0.2f;
    float feedback = 0.0f;
    float pitch = 0.0f;
    float pitchRange = 12.0f;
    float synthAttack = 0.01f;
    float synthDecay = 0.2f;
    float synthSustain = 0.8f;
    float synthRelease = 0.3f;
    bool synthPhaseSync = false;
    float sampleAttack = 0.005f;
    float sampleRelease = 0.3f;
    std::array<LfoSettings, 3> lfoSettings{};
    int midiChannel = 1;
    int midiPort = -1;
    std::wstring midiPortName;
    std::shared_ptr<kj::VST3Host> vstHost;
};

constexpr float kTrackStepVelocityMin = 0.0f;
constexpr float kTrackStepVelocityMax = 1.0f;
constexpr float kTrackStepPanMin = -1.0f;
constexpr float kTrackStepPanMax = 1.0f;
constexpr float kTrackStepPitchMin = -12.0f;
constexpr float kTrackStepPitchMax = 12.0f;

struct StepNoteInfo
{
    int midiNote = 0;
    float velocity = kTrackStepVelocityMax;
    bool sustain = false;
};

void initTracks();
Track addTrack(const std::string& name = {});
std::vector<Track> getTracks();
size_t getTrackCount();

void trackSetName(int trackId, const std::string& name);

TrackType trackGetType(int trackId);
void trackSetType(int trackId, TrackType type);

float trackGetVolume(int trackId);
void trackSetVolume(int trackId, float volume);

float trackGetPan(int trackId);
void trackSetPan(int trackId, float pan);

float trackGetEqLowGain(int trackId);
float trackGetEqMidGain(int trackId);
float trackGetEqHighGain(int trackId);

bool trackGetEqEnabled(int trackId);

void trackSetEqLowGain(int trackId, float gainDb);
void trackSetEqMidGain(int trackId, float gainDb);
void trackSetEqHighGain(int trackId, float gainDb);
void trackSetEqEnabled(int trackId, bool enabled);

bool trackGetDelayEnabled(int trackId);
void trackSetDelayEnabled(int trackId, bool enabled);

float trackGetDelayTimeMs(int trackId);
void trackSetDelayTimeMs(int trackId, float value);

float trackGetDelayFeedback(int trackId);
void trackSetDelayFeedback(int trackId, float value);

float trackGetDelayMix(int trackId);
void trackSetDelayMix(int trackId, float value);

bool trackGetCompressorEnabled(int trackId);
void trackSetCompressorEnabled(int trackId, bool enabled);

float trackGetCompressorThresholdDb(int trackId);
void trackSetCompressorThresholdDb(int trackId, float value);

float trackGetCompressorRatio(int trackId);
void trackSetCompressorRatio(int trackId, float value);

float trackGetCompressorAttack(int trackId);
void trackSetCompressorAttack(int trackId, float value);

float trackGetCompressorRelease(int trackId);
void trackSetCompressorRelease(int trackId, float value);

bool trackGetSidechainEnabled(int trackId);
void trackSetSidechainEnabled(int trackId, bool enabled);

int trackGetSidechainSourceTrack(int trackId);
void trackSetSidechainSourceTrack(int trackId, int sourceTrackId);

float trackGetSidechainAmount(int trackId);
void trackSetSidechainAmount(int trackId, float value);

float trackGetSidechainAttack(int trackId);
void trackSetSidechainAttack(int trackId, float value);

float trackGetSidechainRelease(int trackId);
void trackSetSidechainRelease(int trackId, float value);

bool trackGetStepState(int trackId, int stepIndex);
void trackSetStepState(int trackId, int stepIndex, bool enabled);
void trackToggleStepState(int trackId, int stepIndex);
int trackGetStepCount(int trackId);
void trackSetStepCount(int trackId, int count);

int trackGetStepNote(int trackId, int stepIndex);
void trackSetStepNote(int trackId, int stepIndex, int midiNote);
std::vector<int> trackGetStepNotes(int trackId, int stepIndex);
void trackToggleStepNote(int trackId, int stepIndex, int midiNote);

bool trackGetStepNoteSustain(int trackId, int stepIndex, int midiNote);
void trackSetStepNoteSustain(int trackId, int stepIndex, int midiNote, bool sustain);

float trackGetStepVelocity(int trackId, int stepIndex);
void trackSetStepVelocity(int trackId, int stepIndex, float value);
float trackGetStepNoteVelocity(int trackId, int stepIndex, int midiNote);
std::vector<StepNoteInfo> trackGetStepNoteInfo(int trackId, int stepIndex);
void trackSetStepNoteVelocity(int trackId, int stepIndex, int midiNote, float value);

float trackGetStepPan(int trackId, int stepIndex);
void trackSetStepPan(int trackId, int stepIndex, float value);

float trackGetStepPitchOffset(int trackId, int stepIndex);
void trackSetStepPitchOffset(int trackId, int stepIndex, float value);
