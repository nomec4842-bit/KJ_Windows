#pragma once
#include <atomic>
#include <filesystem>
extern std::atomic<bool> isPlaying;
void initAudio();
void shutdownAudio();
bool loadSampleFile(const std::filesystem::path& path);
