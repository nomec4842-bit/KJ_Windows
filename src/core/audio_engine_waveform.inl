std::vector<float> getMasterWaveformSnapshot(std::size_t sampleCount)
{
    const std::size_t capacity = masterWaveformBuffer.size();
    if (capacity == 0)
        return {};

    std::size_t writeIndex = masterWaveformWriteIndex.load(std::memory_order_acquire);
    bool filled = masterWaveformFilled.load(std::memory_order_acquire);
    std::size_t available = filled ? capacity : writeIndex;
    if (available == 0)
        return {};

    sampleCount = std::min(sampleCount, available);
    std::vector<float> result(sampleCount);
    std::size_t startIndex = (writeIndex + capacity - sampleCount) % capacity;
    for (std::size_t i = 0; i < sampleCount; ++i)
    {
        std::size_t index = (startIndex + i) % capacity;
        result[i] = masterWaveformBuffer[index];
    }
    return result;
}

std::size_t getMasterWaveformCapacity()
{
    return masterWaveformBuffer.size();
}
