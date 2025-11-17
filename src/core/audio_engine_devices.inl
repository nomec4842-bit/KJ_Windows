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
    std::lock_guard<std::mutex> lock(deviceMutex);
    AudioOutputDevice info;
    info.id = activeDeviceId;
    info.name = activeDeviceName;
    return info;
}

std::wstring getRequestedAudioOutputDeviceId() {
    std::lock_guard<std::mutex> lock(deviceMutex);
    return activeRequestedDeviceId;
}

bool setActiveAudioOutputDevice(const std::wstring& deviceId) {
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        requestedDeviceId = deviceId;
        activeRequestedDeviceId = deviceId;
    }
    deviceChangeRequested.store(true, std::memory_order_release);
    return true;
}
