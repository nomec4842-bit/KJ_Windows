#pragma once

#if defined(_WIN32)
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

#include <memory>
#include <string>
#include <vector>

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

    HRESULT currentPadding(UINT32* padding) const;
    HRESULT getBuffer(UINT32 frameCount, BYTE** data);
    void releaseBuffer(UINT32 frameCount);

    static std::vector<DeviceInfo> enumerateRenderDevices();

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
    std::wstring deviceId_;
    std::wstring deviceName_;
};

