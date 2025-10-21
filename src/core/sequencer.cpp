#include "core/sequencer.h"

#include <algorithm>

std::array<std::atomic<bool>, kMaxSequencerSteps> sequencerSteps;
std::atomic<int> sequencerCurrentStep{0};
std::atomic<int> sequencerBPM{120};
std::atomic<bool> sequencerResetRequested{false};
std::atomic<int> sequencerStepCount{kSequencerStepsPerPage};

void initSequencer() {
    for (int i = 0; i < kMaxSequencerSteps; ++i) {
        bool enabled = (i < kSequencerStepsPerPage) ? (i % 4 == 0) : false;
        sequencerSteps[i].store(enabled, std::memory_order_relaxed);
    }
    sequencerCurrentStep.store(0, std::memory_order_relaxed);
    sequencerBPM.store(120, std::memory_order_relaxed);
    sequencerResetRequested.store(true, std::memory_order_relaxed);
    sequencerStepCount.store(kSequencerStepsPerPage, std::memory_order_relaxed);
}

void toggleSequencerStep(int index) {
    if (index < 0 || index >= sequencerStepCount.load(std::memory_order_relaxed)) return;
    bool current = sequencerSteps[index].load(std::memory_order_relaxed);
    sequencerSteps[index].store(!current, std::memory_order_relaxed);
}

void requestSequencerReset() {
    sequencerResetRequested.store(true, std::memory_order_relaxed);
}

void setSequencerStepCount(int count) {
    int clamped = std::clamp(count, 1, kMaxSequencerSteps);
    int currentCount = sequencerStepCount.load(std::memory_order_relaxed);
    if (clamped == currentCount) return;

    if (clamped < currentCount) {
        int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
        if (currentStep >= clamped) {
            sequencerCurrentStep.store(0, std::memory_order_relaxed);
        }
    }

    sequencerStepCount.store(clamped, std::memory_order_relaxed);
    sequencerResetRequested.store(true, std::memory_order_relaxed);
}

int getSequencerStepCount() {
    return std::clamp(sequencerStepCount.load(std::memory_order_relaxed), 1, kMaxSequencerSteps);
}
