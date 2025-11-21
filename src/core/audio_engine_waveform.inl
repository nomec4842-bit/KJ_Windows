std::vector<float> getMasterWaveformSnapshot(std::size_t sampleCount)
{
    // Waveform buffers operate under a single-producer/single-consumer contract
    // where the audio thread publishes a completed buffer index after filling it.
    // The reader always uses the latest published (inactive) buffer to avoid races.
    int readIndex = masterWaveformPublishIndex.load(std::memory_order_acquire);
    const auto& buffer = masterWaveformBuffers[readIndex];
    if (buffer.count == 0)
        return {};

    sampleCount = std::min(sampleCount, buffer.count);
    return std::vector<float>(buffer.data.begin(), buffer.data.begin() + sampleCount);
}

std::size_t getMasterWaveformCapacity()
{
    return kMasterWaveformBufferSize;
}
