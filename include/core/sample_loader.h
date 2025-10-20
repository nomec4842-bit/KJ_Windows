#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

struct SampleBuffer {
    std::vector<int16_t> samples;
    int channels = 0;
    int sampleRate = 0;

    [[nodiscard]] size_t frameCount() const noexcept {
        return (channels > 0) ? samples.size() / static_cast<size_t>(channels) : 0;
    }
};

bool loadSampleFromFile(const std::filesystem::path& path, SampleBuffer& outBuffer);
