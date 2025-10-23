#include "core/audio_device_handler.h"

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

void AudioDeviceHandler::resetComObjects() {
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

bool AudioDeviceHandler::initialize(const std::wstring& deviceId) {
    if (initialized_) {
        if ((deviceId.empty() && deviceId_.empty()) || (!deviceId.empty() && deviceId == deviceId_)) {
            return true;
        }
    }

    shutdown();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) {
        return false;
    }

    if (deviceId.empty()) {
        hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    } else {
        hr = enumerator_->GetDevice(deviceId.c_str(), &device_);
    }
    if (FAILED(hr)) {
        shutdown();
        return false;
    }

    LPWSTR resolvedId = nullptr;
    hr = device_->GetId(&resolvedId);
    if (SUCCEEDED(hr) && resolvedId) {
        deviceId_ = resolvedId;
        CoTaskMemFree(resolvedId);
    } else {
        deviceId_.clear();
    }

    deviceName_.clear();
    IPropertyStore* propertyStore = nullptr;
    hr = device_->OpenPropertyStore(STGM_READ, &propertyStore);
    if (SUCCEEDED(hr) && propertyStore) {
        PROPVARIANT varName;
        PropVariantInit(&varName);
        if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
            if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                deviceName_ = varName.pwszVal;
            }
        }
        PropVariantClear(&varName);
        propertyStore->Release();
    }

    hr = device_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client_);
    if (FAILED(hr)) {
        shutdown();
        return false;
    }

    WAVEFORMATEX* mixFormat = nullptr;
    hr = client_->GetMixFormat(&mixFormat);
    if (FAILED(hr)) {
        shutdown();
        return false;
    }
    mixFormat_.reset(mixFormat);

    mixFormat_->wFormatTag = WAVE_FORMAT_PCM;
    mixFormat_->nChannels = 2;
    mixFormat_->nSamplesPerSec = 44100;
    mixFormat_->wBitsPerSample = 16;
    mixFormat_->nBlockAlign = (mixFormat_->wBitsPerSample / 8) * mixFormat_->nChannels;
    mixFormat_->nAvgBytesPerSec = mixFormat_->nSamplesPerSec * mixFormat_->nBlockAlign;

    hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, kBufferDuration, 0,
                             mixFormat_.get(), NULL);
    if (FAILED(hr)) {
        shutdown();
        return false;
    }

    hr = client_->GetBufferSize(&bufferFrameCount_);
    if (FAILED(hr)) {
        shutdown();
        return false;
    }

    hr = client_->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient_);
    if (FAILED(hr)) {
        shutdown();
        return false;
    }

    initialized_ = true;
    if (deviceName_.empty()) {
        deviceName_ = L"Audio Device";
    }
    return true;
}

void AudioDeviceHandler::shutdown() {
    if (client_) {
        client_->Stop();
    }
    resetComObjects();
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
}

bool AudioDeviceHandler::start() {
    if (!initialized_ || !client_) {
        return false;
    }
    HRESULT hr = client_->Start();
    return SUCCEEDED(hr);
}

void AudioDeviceHandler::stop() {
    if (client_) {
        client_->Stop();
    }
}

HRESULT AudioDeviceHandler::currentPadding(UINT32* padding) const {
    if (!padding) {
        return E_POINTER;
    }
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
    *data = nullptr;
    if (!renderClient_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    return renderClient_->GetBuffer(frameCount, data);
}

void AudioDeviceHandler::releaseBuffer(UINT32 frameCount) {
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

