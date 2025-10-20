#include "core/sequencer.h"

#include <algorithm>

std::array<std::atomic<bool>, kNumSequencerSteps> sequencerSteps;
std::atomic<int> sequencerCurrentStep{0};
std::atomic<int> sequencerBPM{120};
std::atomic<bool> sequencerResetRequested{false};

void initSequencer() {
    for (int i = 0; i < kNumSequencerSteps; ++i) {
        sequencerSteps[i].store(i % 4 == 0, std::memory_order_relaxed);
    }
    sequencerCurrentStep.store(0, std::memory_order_relaxed);
    sequencerBPM.store(120, std::memory_order_relaxed);
    sequencerResetRequested.store(true, std::memory_order_relaxed);
}

void toggleSequencerStep(int index) {
    if (index < 0 || index >= kNumSequencerSteps) return;
    bool current = sequencerSteps[index].load(std::memory_order_relaxed);
    sequencerSteps[index].store(!current, std::memory_order_relaxed);
}

void requestSequencerReset() {
    sequencerResetRequested.store(true, std::memory_order_relaxed);
}
