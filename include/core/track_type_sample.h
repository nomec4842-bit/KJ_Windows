#pragma once

#include "core/tracks.h"

#include <memory>

float trackGetSampleAttack(int trackId);
void trackSetSampleAttack(int trackId, float value);

float trackGetSampleRelease(int trackId);
void trackSetSampleRelease(int trackId, float value);

std::shared_ptr<const SampleBuffer> trackGetSampleBuffer(int trackId);
void trackSetSampleBuffer(int trackId, std::shared_ptr<const SampleBuffer> buffer);

