#include "core/audio_engine.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <thread>
#include <atomic>
#include <cmath>
#include <algorithm>

#include "core/sequencer.h"

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
    double stepSampleCounter = 0.0;
    double envelope = 0.0;
    bool previousPlaying = false;

    while (running) {
        UINT32 padding = 0;
        client->GetCurrentPadding(&padding);
        UINT32 available = bufferFrameCount - padding;
        if (available > 0) {
            BYTE* data;
            renderClient->GetBuffer(available, &data);
            short* samples = (short*)data;
            int bpm = std::clamp(sequencerBPM.load(std::memory_order_relaxed), 30, 240);
            double stepDurationSamples = sampleRate * 60.0 / (static_cast<double>(bpm) * 4.0);
            if (stepDurationSamples < 1.0) stepDurationSamples = 1.0;

            for (UINT32 i = 0; i < available; i++) {
                bool playing = isPlaying.load(std::memory_order_relaxed);
                bool stepAdvanced = false;

                if (!playing) {
                    if (previousPlaying) {
                        sequencerResetRequested.store(true, std::memory_order_relaxed);
                    }
                    previousPlaying = false;
                    stepSampleCounter = 0.0;
                    envelope *= 0.92;
                    if (envelope < 0.0001) envelope = 0.0;
                } else {
                    if (!previousPlaying) {
                        sequencerResetRequested.store(true, std::memory_order_relaxed);
                    }
                    previousPlaying = true;

                    if (sequencerResetRequested.exchange(false, std::memory_order_acq_rel)) {
                        sequencerCurrentStep.store(0, std::memory_order_relaxed);
                        stepSampleCounter = 0.0;
                        envelope = 0.0;
                    }

                    stepSampleCounter += 1.0;
                    if (stepSampleCounter >= stepDurationSamples) {
                        stepSampleCounter -= stepDurationSamples;
                        int nextStep = sequencerCurrentStep.load(std::memory_order_relaxed) + 1;
                        if (nextStep >= kNumSequencerSteps) nextStep = 0;
                        sequencerCurrentStep.store(nextStep, std::memory_order_relaxed);
                        stepAdvanced = true;
                    }

                    int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
                    bool gate = sequencerSteps[currentStep].load(std::memory_order_relaxed);
                    double target = gate ? 0.8 : 0.0;

                    if (stepAdvanced && gate) {
                        envelope = 0.9;
                    }

                    if (envelope < target) {
                        envelope += 0.005;
                        if (envelope > target) envelope = target;
                    } else {
                        envelope -= 0.003;
                        if (envelope < target) envelope = target;
                    }
                }

                double sampleValue = sin(phase) * envelope;
                phase += twoPi * freq / sampleRate;
                if (phase >= twoPi) phase -= twoPi;

                short value = static_cast<short>(sampleValue * 32767.0);
                samples[i * 2] = value;
                samples[i * 2 + 1] = value;
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
