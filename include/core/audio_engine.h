#pragma once

#include <atomic>
#include <filesystem>
#include <string>
#include <vector>

extern std::atomic<bool> isPlaying;
void initAudio();
void shutdownAudio();
bool loadSampleFile(int trackId, const std::filesystem::path& path);

struct AudioOutputDevice {
    std::wstring id;
    std::wstring name;
};

std::vector<AudioOutputDevice> getAvailableAudioOutputDevices();
AudioOutputDevice getActiveAudioOutputDevice();
std::wstring getRequestedAudioOutputDeviceId();
bool setActiveAudioOutputDevice(const std::wstring& deviceId);
