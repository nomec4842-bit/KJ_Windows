#pragma once
#include <atomic>
extern std::atomic<bool> isPlaying;
void initAudio();
void shutdownAudio();
