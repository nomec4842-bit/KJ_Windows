std::vector<AudioOutputDevice> getAvailableAudioOutputDevices() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        shouldUninitialize = false;
    } else if (FAILED(hr)) {
        return {};
    }

    std::vector<AudioOutputDevice> result;
    auto devices = AudioDeviceHandler::enumerateRenderDevices();
    result.reserve(devices.size());
    for (auto& device : devices) {
        AudioOutputDevice info;
        info.id = std::move(device.id);
        info.name = std::move(device.name);
        result.push_back(std::move(info));
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return result;
}

AudioOutputDevice getActiveAudioOutputDevice() {
    const auto& snapshot = getDeviceSnapshot();
    AudioOutputDevice info;
    info.id = snapshot.activeId;
    info.name = snapshot.activeName;
    return info;
}

std::wstring getRequestedAudioOutputDeviceId() {
    int index = gRequestedDeviceIndex.load(std::memory_order_acquire);
    return gRequestedDeviceIds[index];
}

bool setActiveAudioOutputDevice(const std::wstring& deviceId) {
    const auto& snapshot = getDeviceSnapshot();
    if (deviceId == snapshot.activeId || deviceId == snapshot.requestedId)
        return false;

    int nextIndex = gRequestedDeviceIndex.load(std::memory_order_relaxed) ^ 1;
    gRequestedDeviceIds[nextIndex] = deviceId;
    gRequestedDeviceIndex.store(nextIndex, std::memory_order_release);
    deviceChangeRequested.store(true, std::memory_order_release);
    return true;
}
