#pragma once

#include <atomic>

constexpr int kSequencerStepsPerPage = 16;
constexpr int kMaxSequencerSteps = 1024;

extern std::atomic<int> sequencerCurrentStep;
extern std::atomic<int> sequencerBPM;
extern std::atomic<bool> sequencerResetRequested;

void initSequencer();
void toggleSequencerStep(int trackId, int index);
bool getTrackStepState(int trackId, int index);
void requestSequencerReset();
void setSequencerStepCount(int trackId, int count);
int getSequencerStepCount(int trackId);
int getActiveSequencerTrackId();
void setActiveSequencerTrackId(int trackId);
