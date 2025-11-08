#include "core/audio_device_handler.h"

#if defined(_WIN32)

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>

namespace {
constexpr DWORD kStreamFlags = 0;
constexpr REFERENCE_TIME kBufferDuration = 10000000; // 1 second
}

AudioDeviceHandler::AudioDeviceHandler() = default;

AudioDeviceHandler::~AudioDeviceHandler() {
    shutdown();
}

void AudioDeviceHandler::FormatDeleter::operator()(WAVEFORMATEX* format) const {
    if (format) {
        CoTaskMemFree(format);
    }
}

void AudioDeviceHandler::resetComObjectsLocked() {
    if (renderClient_) {
        renderClient_->Release();
        renderClient_ = nullptr;
    }
    if (client_) {
        client_->Release();
        client_ = nullptr;
    }
    if (device_) {
        device_->Release();
        device_ = nullptr;
    }
    if (enumerator_) {
        enumerator_->Release();
        enumerator_ = nullptr;
    }
}

void AudioDeviceHandler::resetStateLocked() {
    resetComObjectsLocked();
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
}

bool AudioDeviceHandler::initialize(const std::wstring& deviceId) {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (initialized_) {
        if ((deviceId.empty() && deviceId_.empty()) || (!deviceId.empty() && deviceId == deviceId_)) {
            return true;
        }
    }

    if (initThreadActive_) {
        if (!initCompleted_) {
            return false;
        }

        bool success = initSuccess_;
        initThreadActive_ = false;
        initCompleted_ = false;
        lock.unlock();
        if (initThread_.joinable()) {
            initThread_.join();
        }
        lock.lock();
        if (!success) {
            resetStateLocked();
            return false;
        }
        return success;
    }

    cancelRequested_ = false;
    resetStateLocked();
    initThreadActive_ = true;
    initCompleted_ = false;
    initSuccess_ = false;

    std::thread worker([this, deviceId]() {
        bool success = runInitialization(deviceId);
        {
            std::lock_guard<std::mutex> guard(stateMutex_);
            initSuccess_ = success;
            initCompleted_ = true;
        }
    });

    if (initThread_.joinable()) {
        lock.unlock();
        initThread_.join();
        lock.lock();
    }
    initThread_ = std::move(worker);
    return false;
}

bool AudioDeviceHandler::isInitializing() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return initThreadActive_ && !initCompleted_;
}

bool AudioDeviceHandler::runInitialization(const std::wstring& deviceId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninitialize = SUCCEEDED(hr);
    if (FAILED(hr)) {
        if (hr != RPC_E_CHANGED_MODE) {
            return false;
        }
        shouldUninitialize = false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    UINT32 bufferFrameCount = 0;
    std::wstring resolvedDeviceId;
    std::wstring resolvedDeviceName;
    bool success = false;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr) || !enumerator) {
            break;
        }

        if (deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        } else {
            hr = enumerator->GetDevice(deviceId.c_str(), &device);
        }
        if (FAILED(hr) || !device) {
            break;
        }

        LPWSTR resolvedId = nullptr;
        hr = device->GetId(&resolvedId);
        if (SUCCEEDED(hr) && resolvedId) {
            resolvedDeviceId = resolvedId;
            CoTaskMemFree(resolvedId);
        }

        IPropertyStore* propertyStore = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (SUCCEEDED(hr) && propertyStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    resolvedDeviceName = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            propertyStore->Release();
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
        if (FAILED(hr) || !client) {
            break;
        }

        hr = client->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            break;
        }

        mixFormat->wFormatTag = WAVE_FORMAT_PCM;
        mixFormat->nChannels = 2;
        mixFormat->nSamplesPerSec = 44100;
        mixFormat->wBitsPerSample = 16;
        mixFormat->nBlockAlign = (mixFormat->wBitsPerSample / 8) * mixFormat->nChannels;
        mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, kBufferDuration, 0,
                                 mixFormat, NULL);
        if (FAILED(hr)) {
            break;
        }

        hr = client->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            break;
        }

        hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
        if (FAILED(hr) || !renderClient) {
            break;
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (cancelRequested_) {
                break;
            }

            resetComObjectsLocked();
            mixFormat_.reset(mixFormat);
            mixFormat = nullptr;
            bufferFrameCount_ = bufferFrameCount;
            enumerator_ = enumerator;
            device_ = device;
            client_ = client;
            renderClient_ = renderClient;
            initialized_ = true;
            deviceId_ = std::move(resolvedDeviceId);
            if (deviceId_.empty()) {
                deviceId_ = deviceId;
            }
            deviceName_ = std::move(resolvedDeviceName);
            if (deviceName_.empty()) {
                deviceName_ = L"Audio Device";
            }
        }

        enumerator = nullptr;
        device = nullptr;
        client = nullptr;
        renderClient = nullptr;
        success = true;
    } while (false);

    if (renderClient) {
        renderClient->Release();
    }
    if (client) {
        client->Release();
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (mixFormat) {
        CoTaskMemFree(mixFormat);
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return success;
}

void AudioDeviceHandler::shutdown() {
    std::thread pendingThread;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cancelRequested_ = true;
        if (client_) {
            client_->Stop();
        }
        resetStateLocked();

        if (initThread_.joinable()) {
            pendingThread = std::move(initThread_);
        }
        initThreadActive_ = false;
        initCompleted_ = false;
        initSuccess_ = false;
    }

    if (pendingThread.joinable()) {
        pendingThread.join();
    }
}

bool AudioDeviceHandler::start() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (initThreadActive_ && !initCompleted_) {
        return false;
    }
    if (!initialized_ || !client_) {
        return false;
    }
    HRESULT hr = client_->Start();
    return SUCCEEDED(hr);
}

void AudioDeviceHandler::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (client_) {
        client_->Stop();
    }
}

HRESULT AudioDeviceHandler::currentPadding(UINT32* padding) const {
    if (!padding) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    *padding = 0;
    if (!client_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    return client_->GetCurrentPadding(padding);
}

HRESULT AudioDeviceHandler::getBuffer(UINT32 frameCount, BYTE** data) {
    if (!data) {
        return E_POINTER;
    }
    std::lock_guard<std::mutex> lock(stateMutex_);
    *data = nullptr;
    if (!renderClient_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    return renderClient_->GetBuffer(frameCount, data);
}

void AudioDeviceHandler::releaseBuffer(UINT32 frameCount) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (renderClient_) {
        renderClient_->ReleaseBuffer(frameCount, 0);
    }
}

std::vector<AudioDeviceHandler::DeviceInfo> AudioDeviceHandler::enumerateRenderDevices() {
    std::vector<DeviceInfo> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        return devices;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        enumerator->Release();
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }

        IPropertyStore* propertyStore = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (SUCCEEDED(hr) && propertyStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    info.name = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            propertyStore->Release();
        }

        if (info.name.empty()) {
            info.name = L"Audio Device";
        }

        devices.push_back(std::move(info));
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return devices;
}

#else  // !_WIN32

AudioDeviceHandler::AudioDeviceHandler() = default;

AudioDeviceHandler::~AudioDeviceHandler() {
    shutdown();
}

void AudioDeviceHandler::FormatDeleter::operator()(WAVEFORMATEX* format) const {
    (void)format;
}

void AudioDeviceHandler::resetComObjectsLocked() {
    enumerator_ = nullptr;
    device_ = nullptr;
    client_ = nullptr;
    renderClient_ = nullptr;
}

void AudioDeviceHandler::resetStateLocked() {
    resetComObjectsLocked();
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
}

bool AudioDeviceHandler::initialize(const std::wstring& deviceId) {
    deviceId_ = deviceId;
    deviceName_.clear();
    initialized_ = false;
    bufferFrameCount_ = 0;
    mixFormat_.reset();
    resetComObjectsLocked();
    return false;
}

bool AudioDeviceHandler::isInitializing() const {
    return false;
}

bool AudioDeviceHandler::runInitialization(const std::wstring& /*deviceId*/) {
    return false;
}

void AudioDeviceHandler::shutdown() {
    resetStateLocked();
}

bool AudioDeviceHandler::start() {
    return false;
}

void AudioDeviceHandler::stop() {}

HRESULT AudioDeviceHandler::currentPadding(UINT32* padding) const {
    if (padding) {
        *padding = 0;
    }
    return static_cast<HRESULT>(0);
}

HRESULT AudioDeviceHandler::getBuffer(UINT32 /*frameCount*/, BYTE** data) {
    if (data) {
        *data = nullptr;
    }
    return static_cast<HRESULT>(0);
}

void AudioDeviceHandler::releaseBuffer(UINT32 /*frameCount*/) {}

std::vector<AudioDeviceHandler::DeviceInfo> AudioDeviceHandler::enumerateRenderDevices() {
    return {};
}

#endif  // _WIN32

