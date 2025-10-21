#pragma once

#include <array>
#include <atomic>

constexpr int kSequencerStepsPerPage = 16;
constexpr int kMaxSequencerSteps = 1024;

extern std::array<std::atomic<bool>, kMaxSequencerSteps> sequencerSteps;
extern std::atomic<int> sequencerCurrentStep;
extern std::atomic<int> sequencerBPM;
extern std::atomic<bool> sequencerResetRequested;
extern std::atomic<int> sequencerStepCount;

void initSequencer();
void toggleSequencerStep(int index);
void requestSequencerReset();
void setSequencerStepCount(int count);
int getSequencerStepCount();
