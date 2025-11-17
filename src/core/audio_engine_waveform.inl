std::vector<float> getMasterWaveformSnapshot(std::size_t sampleCount)
{
    std::lock_guard<std::mutex> lock(masterWaveformMutex);
    const std::size_t capacity = masterWaveformBuffer.size();
    if (capacity == 0)
        return {};

    std::size_t available = masterWaveformFilled ? capacity : masterWaveformWriteIndex;
    if (available == 0)
        return {};

    sampleCount = std::min(sampleCount, available);
    std::vector<float> result(sampleCount);
    std::size_t startIndex = (masterWaveformWriteIndex + capacity - sampleCount) % capacity;
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        std::size_t index = (startIndex + i) % capacity;
        result[i] = masterWaveformBuffer[index];
    }
    return result;
}

std::size_t getMasterWaveformCapacity()
{
    std::lock_guard<std::mutex> lock(masterWaveformMutex);
    return masterWaveformBuffer.size();
}
