#include "core/track_type_sample.h"
#include "core/tracks.h"

// Legacy sampler track logic has been moved to track_type_sample_legacy_unused.cpp
// leaving this active implementation as a no-op while sampler tracks are removed.

float trackGetSampleAttack(int)
{
    return 0.005f;
}

void trackSetSampleAttack(int, float)
{
}

float trackGetSampleRelease(int)
{
    return 0.3f;
}

void trackSetSampleRelease(int, float)
{
}

std::shared_ptr<const SampleBuffer> trackGetSampleBuffer(int)
{
    return {};
}

void trackSetSampleBuffer(int, std::shared_ptr<const SampleBuffer>)
{
}
