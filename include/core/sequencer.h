#pragma once

#include <atomic>

constexpr int kSequencerStepsPerPage = 16;
constexpr int kMaxSequencerSteps = 1024;

enum class SequencerResetReason
{
    Manual,
    TrackSelection,
    StepCountChange,
};

extern std::atomic<int> sequencerCurrentStep;
extern std::atomic<int> sequencerBPM;
extern std::atomic<bool> sequencerResetRequested;
extern std::atomic<SequencerResetReason> sequencerResetReason;

void initSequencer();
void toggleSequencerStep(int trackId, int index);
bool getTrackStepState(int trackId, int index);
void requestSequencerReset(SequencerResetReason reason = SequencerResetReason::Manual);
void setSequencerStepCount(int trackId, int count);
int getSequencerStepCount(int trackId);
int getActiveSequencerTrackId();
void setActiveSequencerTrackId(int trackId);
