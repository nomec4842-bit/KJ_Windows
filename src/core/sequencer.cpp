#include "core/sequencer.h"
#include "core/tracks.h"

#include <algorithm>

std::atomic<int> sequencerCurrentStep{0};
std::atomic<int> sequencerBPM{120};
std::atomic<bool> sequencerResetRequested{false};

namespace
{

std::atomic<int> gActiveTrackId{0};

} // namespace

void initSequencer()
{
    sequencerCurrentStep.store(0, std::memory_order_relaxed);
    sequencerBPM.store(120, std::memory_order_relaxed);
    sequencerResetRequested.store(true, std::memory_order_relaxed);

    int activeTrackId = 0;
    auto tracks = getTracks();
    if (!tracks.empty())
    {
        activeTrackId = tracks.front().id;
        for (const auto& track : tracks)
        {
            int count = trackGetStepCount(track.id);
            if (count <= 0)
            {
                trackSetStepCount(track.id, kSequencerStepsPerPage);
            }
        }
    }

    gActiveTrackId.store(activeTrackId, std::memory_order_relaxed);
}

void toggleSequencerStep(int trackId, int index)
{
    if (trackId <= 0)
        return;
    trackToggleStepState(trackId, index);
}

bool getTrackStepState(int trackId, int index)
{
    if (trackId <= 0)
        return false;
    return trackGetStepState(trackId, index);
}

void requestSequencerReset()
{
    sequencerResetRequested.store(true, std::memory_order_relaxed);
}

void setSequencerStepCount(int trackId, int count)
{
    if (trackId <= 0)
        return;

    int previous = trackGetStepCount(trackId);
    trackSetStepCount(trackId, count);
    int current = trackGetStepCount(trackId);

    if (trackId == gActiveTrackId.load(std::memory_order_relaxed))
    {
        int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
        if (currentStep >= current)
        {
            sequencerCurrentStep.store(0, std::memory_order_relaxed);
        }
        if (current != previous)
        {
            sequencerResetRequested.store(true, std::memory_order_relaxed);
        }
    }
}

int getSequencerStepCount(int trackId)
{
    if (trackId <= 0)
        return kSequencerStepsPerPage;

    int count = trackGetStepCount(trackId);
    if (count <= 0)
        return kSequencerStepsPerPage;
    return std::clamp(count, 1, kMaxSequencerSteps);
}

int getActiveSequencerTrackId()
{
    return gActiveTrackId.load(std::memory_order_relaxed);
}

void setActiveSequencerTrackId(int trackId)
{
    int validatedId = trackGetStepCount(trackId) > 0 ? trackId : 0;
    gActiveTrackId.store(validatedId, std::memory_order_relaxed);

    int stepCount = getSequencerStepCount(validatedId);
    int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
    if (currentStep >= stepCount)
    {
        sequencerCurrentStep.store(0, std::memory_order_relaxed);
    }

    sequencerResetRequested.store(true, std::memory_order_relaxed);
}
