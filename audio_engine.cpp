#include "core/audio_engine.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <thread>
#include <atomic>
#include <cmath>

std::atomic<bool> isPlaying = false;
static bool running = true;
static std::thread audioThread;

void audioLoop() {
    HRESULT hr;
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* renderClient = nullptr;

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                          __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) return;
    hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (FAILED(hr)) return;

    WAVEFORMATEX* pwfx = nullptr;
    hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
    if (FAILED(hr)) return;
    hr = client->GetMixFormat(&pwfx);
    if (FAILED(hr)) return;

    pwfx->wFormatTag = WAVE_FORMAT_PCM;
    pwfx->nChannels = 2;
    pwfx->nSamplesPerSec = 44100;
    pwfx->wBitsPerSample = 16;
    pwfx->nBlockAlign = (pwfx->wBitsPerSample / 8) * pwfx->nChannels;
    pwfx->nAvgBytesPerSec = pwfx->nSamplesPerSec * pwfx->nBlockAlign;

    REFERENCE_TIME bufferDuration = 10000000;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, 0, bufferDuration, 0, pwfx, NULL);
    if (FAILED(hr)) return;

    UINT32 bufferFrameCount;
    client->GetBufferSize(&bufferFrameCount);
    client->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
    hr = client->Start();
    if (FAILED(hr)) return;

    const double freq = 440.0;
    double phase = 0.0;
    const double twoPi = 6.283185307179586;
    const double sampleRate = 44100.0;

    while (running) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 available = bufferFrameCount - padding;
        if (available > 0) {
            BYTE* data;
            renderClient->GetBuffer(available, &data);
            short* samples = (short*)data;
            for (UINT32 i = 0; i < available; i++) {
                short value = isPlaying ? (short)(sin(phase) * 32767.0) : 0;
                samples[i * 2] = value;
                samples[i * 2 + 1] = value;
                phase += twoPi * freq / sampleRate;
                if (phase >= twoPi) phase -= twoPi;
            }
            renderClient->ReleaseBuffer(available, 0);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    client->Stop();
    CoTaskMemFree(pwfx);
    renderClient->Release();
    client->Release();
    device->Release();
    enumerator->Release();
    CoUninitialize();
}

void initAudio() {
    running = true;
    audioThread = std::thread(audioLoop);
}

void shutdownAudio() {
    running = false;
    isPlaying = false;
    if (audioThread.joinable()) audioThread.join();
}
