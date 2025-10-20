#pragma once

#include <array>
#include <atomic>

constexpr int kNumSequencerSteps = 16;

extern std::array<std::atomic<bool>, kNumSequencerSteps> sequencerSteps;
extern std::atomic<int> sequencerCurrentStep;
extern std::atomic<int> sequencerBPM;
extern std::atomic<bool> sequencerResetRequested;

void initSequencer();
void toggleSequencerStep(int index);
void requestSequencerReset();
