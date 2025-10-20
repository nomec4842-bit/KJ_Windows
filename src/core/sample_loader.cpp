#include "core/sample_loader.h"

#include <cstring>
#include <fstream>
#include <string>

namespace {

uint16_t readLE16(const char* data) {
    return static_cast<uint16_t>(static_cast<unsigned char>(data[0]) |
                                 (static_cast<unsigned char>(data[1]) << 8));
}

uint32_t readLE32(const char* data) {
    return static_cast<uint32_t>(static_cast<unsigned char>(data[0]) |
                                 (static_cast<unsigned char>(data[1]) << 8) |
                                 (static_cast<unsigned char>(data[2]) << 16) |
                                 (static_cast<unsigned char>(data[3]) << 24));
}

bool readChunkHeader(std::ifstream& file, char (&id)[4], uint32_t& size) {
    file.read(id, 4);
    if (!file)
        return false;
    file.read(reinterpret_cast<char*>(&size), 4);
    if (!file)
        return false;
    return true;
}

} // namespace

bool loadSampleFromFile(const std::filesystem::path& path, SampleBuffer& outBuffer) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        return false;

    char riffId[4];
    file.read(riffId, 4);
    if (!file || std::strncmp(riffId, "RIFF", 4) != 0)
        return false;

    uint32_t riffSize = 0;
    file.read(reinterpret_cast<char*>(&riffSize), 4);
    (void)riffSize;

    char waveId[4];
    file.read(waveId, 4);
    if (!file || std::strncmp(waveId, "WAVE", 4) != 0)
        return false;

    bool fmtFound = false;
    bool dataFound = false;

    uint16_t audioFormat = 0;
    uint16_t numChannels = 0;
    uint32_t sampleRate = 0;
    uint16_t bitsPerSample = 0;

    std::vector<int16_t> sampleData;

    while (file && (!fmtFound || !dataFound)) {
        char chunkId[4];
        uint32_t chunkSize = 0;
        if (!readChunkHeader(file, chunkId, chunkSize))
            break;

        if (std::strncmp(chunkId, "fmt ", 4) == 0) {
            std::string fmtChunk(chunkSize, '\0');
            file.read(fmtChunk.data(), chunkSize);
            if (!file)
                return false;

            if (chunkSize < 16)
                return false;

            audioFormat = readLE16(fmtChunk.data());
            numChannels = readLE16(fmtChunk.data() + 2);
            sampleRate = readLE32(fmtChunk.data() + 4);
            bitsPerSample = readLE16(fmtChunk.data() + 14);

            fmtFound = true;

            if (chunkSize % 2 != 0)
                file.seekg(1, std::ios::cur);
        } else if (std::strncmp(chunkId, "data", 4) == 0) {
            if (chunkSize % sizeof(int16_t) != 0)
                return false;

            sampleData.resize(chunkSize / sizeof(int16_t));
            file.read(reinterpret_cast<char*>(sampleData.data()), chunkSize);
            if (!file)
                return false;

            dataFound = true;

            if (chunkSize % 2 != 0)
                file.seekg(1, std::ios::cur);
        } else {
            file.seekg(chunkSize, std::ios::cur);
            if (!file)
                return false;
            if (chunkSize % 2 != 0)
                file.seekg(1, std::ios::cur);
        }
    }

    if (!fmtFound || !dataFound)
        return false;

    if (audioFormat != 1 || bitsPerSample != 16 || numChannels <= 0)
        return false;

    outBuffer.samples = std::move(sampleData);
    outBuffer.channels = numChannels;
    outBuffer.sampleRate = static_cast<int>(sampleRate);

    return true;
}
