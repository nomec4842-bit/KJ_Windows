int trackGetMidiChannel(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultMidiChannel;

    int channel = track->midiChannel.load(std::memory_order_relaxed);
    if (channel < kMinMidiChannel || channel > kMaxMidiChannel)
        channel = kDefaultMidiChannel;
    return channel;
}

void trackSetMidiChannel(int trackId, int channel)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    int clamped = std::clamp(channel, kMinMidiChannel, kMaxMidiChannel);
    track->midiChannel.store(clamped, std::memory_order_relaxed);
    track->track.midiChannel = clamped;
}

int trackGetMidiPort(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return kDefaultMidiPort;

    return track->midiPort.load(std::memory_order_relaxed);
}

std::wstring trackGetMidiPortName(int trackId)
{
    auto track = findTrackData(trackId);
    if (!track)
        return {};

    std::lock_guard<std::mutex> lock(track->midiPortMutex);
    return track->midiPortName;
}

void trackSetMidiPort(int trackId, int portId, const std::wstring& portName)
{
    auto track = findTrackData(trackId);
    if (!track)
        return;

    int sanitized = portId;
    if (sanitized < kDefaultMidiPort)
        sanitized = kDefaultMidiPort;

    track->midiPort.store(sanitized, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(track->midiPortMutex);
        track->midiPortName = portName;
    }
    track->track.midiPort = sanitized;
    track->track.midiPortName = portName;
}
