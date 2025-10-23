#include "core/audio_device_handler.h"

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

bool AudioDeviceHandler::initialize() {
    if (initialized_) {
        return true;
    }

    shutdown();

    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator_);
    if (FAILED(hr)) {
        return false;
    }

    hr = enumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &device_);
    if (FAILED(hr)) {
        shutdown();
        return false;
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

UINT32 AudioDeviceHandler::currentPadding() const {
    if (!client_) {
        return 0;
    }
    UINT32 padding = 0;
    HRESULT hr = client_->GetCurrentPadding(&padding);
    if (FAILED(hr)) {
        return 0;
    }
    return padding;
}

bool AudioDeviceHandler::getBuffer(UINT32 frameCount, BYTE** data) {
    if (!renderClient_) {
        return false;
    }
    HRESULT hr = renderClient_->GetBuffer(frameCount, data);
    return SUCCEEDED(hr);
}

void AudioDeviceHandler::releaseBuffer(UINT32 frameCount) {
    if (renderClient_) {
        renderClient_->ReleaseBuffer(frameCount, 0);
    }
}

