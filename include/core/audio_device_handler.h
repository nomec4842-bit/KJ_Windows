#pragma once

#if defined(_WIN32) || defined(_MSC_VER) || defined(__MINGW32__)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#else
#include <cstdint>

using BYTE = unsigned char;
using DWORD = unsigned long;
using HRESULT = long;
using UINT32 = unsigned int;

struct IMMDeviceEnumerator;
struct IMMDevice;
struct IAudioClient;
struct IAudioRenderClient;

struct WAVEFORMATEX {
    std::uint16_t wFormatTag = 0;
    std::uint16_t nChannels = 0;
    std::uint32_t nSamplesPerSec = 0;
    std::uint32_t nAvgBytesPerSec = 0;
    std::uint16_t nBlockAlign = 0;
    std::uint16_t wBitsPerSample = 0;
    std::uint16_t cbSize = 0;
};
#endif

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "hosting/VST3Host.h"

class AudioDeviceHandler {
public:
    AudioDeviceHandler();
    ~AudioDeviceHandler();

    AudioDeviceHandler(const AudioDeviceHandler&) = delete;
    AudioDeviceHandler& operator=(const AudioDeviceHandler&) = delete;

    struct DeviceInfo {
        std::wstring id;
        std::wstring name;
    };

    bool initialize(const std::wstring& deviceId = L"");
    bool isInitializing() const;
    void shutdown();

    bool start();
    void stop();

    bool isInitialized() const { return initialized_; }
    const std::wstring& deviceId() const { return deviceId_; }
    const std::wstring& deviceName() const { return deviceName_; }
    IAudioClient* client() const { return client_; }
    IAudioRenderClient* renderClient() const { return renderClient_; }
    const WAVEFORMATEX* format() const { return mixFormat_.get(); }
    UINT32 bufferFrameCount() const { return bufferFrameCount_; }

    using AudioStreamCallback = void (*)(BYTE* buffer, UINT32 frameCount, const WAVEFORMATEX* format, void* userData);

    void registerStreamCallback(AudioStreamCallback callback, void* userData);
    AudioStreamCallback streamCallback() const;
    void* streamCallbackContext() const;
    void notifyCallbackExecuted();
    void setVSTHost(kj::VST3Host* host);
    static void resetCallbackMonitor();
    static bool streamStartedSuccessfully();
    static bool callbackHasFired();

    HRESULT currentPadding(UINT32* padding) const;
    HRESULT getBuffer(UINT32 frameCount, BYTE** data);
    void releaseBuffer(UINT32 frameCount);

    static std::vector<DeviceInfo> enumerateRenderDevices();

private:
    struct FormatDeleter {
        void operator()(WAVEFORMATEX* format) const;
    };

    void resetComObjectsLocked();
    void resetStateLocked();
    bool runInitialization(const std::wstring& deviceId);

    bool pushAudioBlock(const float* interleavedBlock);
    bool popAudioBlock(float* interleavedBlock);
    void initializeRingBuffer(UINT32 framesPerBuffer, UINT32 channels);

    IMMDeviceEnumerator* enumerator_ = nullptr;
    IMMDevice* device_ = nullptr;
    IAudioClient* client_ = nullptr;
    IAudioRenderClient* renderClient_ = nullptr;
    std::unique_ptr<WAVEFORMATEX, FormatDeleter> mixFormat_;
    UINT32 bufferFrameCount_ = 0;
    bool initialized_ = false;
    std::wstring deviceId_;
    std::wstring deviceName_;
    BYTE* activeRenderBuffer_ = nullptr;
    UINT32 activeRenderFrameCount_ = 0;
    UINT32 activeRenderBufferSizeBytes_ = 0;
    bool bufferPendingRelease_ = false;
    mutable std::mutex stateMutex_;
    std::thread initThread_;
    bool initThreadActive_ = false;
    bool initCompleted_ = false;
    bool initSuccess_ = false;
    bool cancelRequested_ = false;

    AudioStreamCallback callback_ = nullptr;
    void* callbackContext_ = nullptr;

    kj::VST3Host* vstHost_ = nullptr;
    std::vector<std::vector<float>> tempChannelBuffers_;
    std::vector<float*> tempChannelPointers_;

    // Lock-free single-producer single-consumer ring buffer for audio blocks
    std::vector<float> ringBuffer_;
    uint32_t framesPerBlock_ = 0;
    uint32_t ringBufferChannels_ = 0;
    size_t ringBufferCapacityBlocks_ = 0;
    std::atomic<size_t> ringBufferReadIndex_{0};
    std::atomic<size_t> ringBufferWriteIndex_{0};

    std::thread dspThread_{};
    std::atomic<bool> dspRunning_{false};

    static std::atomic<bool> streamStarted_;
    static std::atomic<bool> callbackInvoked_;
};

