#pragma once

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>

#include <memory>

class AudioDeviceHandler {
public:
    AudioDeviceHandler();
    ~AudioDeviceHandler();

    AudioDeviceHandler(const AudioDeviceHandler&) = delete;
    AudioDeviceHandler& operator=(const AudioDeviceHandler&) = delete;

    bool initialize();
    void shutdown();

    bool start();
    void stop();

    bool isInitialized() const { return initialized_; }
    IAudioClient* client() const { return client_; }
    IAudioRenderClient* renderClient() const { return renderClient_; }
    const WAVEFORMATEX* format() const { return mixFormat_.get(); }
    UINT32 bufferFrameCount() const { return bufferFrameCount_; }

    UINT32 currentPadding() const;
    bool getBuffer(UINT32 frameCount, BYTE** data);
    void releaseBuffer(UINT32 frameCount);

private:
    struct FormatDeleter {
        void operator()(WAVEFORMATEX* format) const;
    };

    void resetComObjects();

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    std::unique_ptr<WAVEFORMATEX, FormatDeleter> mixFormat_;
    UINT32 bufferFrameCount_ = 0;
    bool initialized_ = false;
};

