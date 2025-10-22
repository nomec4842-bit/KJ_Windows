#include "core/audio_engine.h"
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <vector>

#include "core/sample_loader.h"
#include "core/sequencer.h"
#include "core/tracks.h"

std::atomic<bool> isPlaying = false;
static bool running = true;
static std::thread audioThread;
static std::shared_ptr<const SampleBuffer> gSampleBuffer;

namespace {

std::filesystem::path getExecutableDirectory() {
    std::array<wchar_t, MAX_PATH> buffer{};
    DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length == buffer.size())
        return {};
    return std::filesystem::path(buffer.data()).parent_path();
}

std::filesystem::path findDefaultSamplePath() {
    auto exeDir = getExecutableDirectory();
    if (exeDir.empty())
        return {};

    const std::array<std::filesystem::path, 2> candidates = {
        exeDir / "assets" / "sample.wav",
        exeDir / "sample.wav"
    };

    for (const auto& candidate : candidates) {
        if (std::filesystem::exists(candidate))
            return candidate;
    }

    return {};
}

} // namespace

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
    double samplePosition = 0.0;
    double sampleIncrement = 1.0;
    bool samplePlaying = false;

    std::shared_ptr<const SampleBuffer> sampleBuffer;
    size_t sampleFrameCount = 0;

    while (running) {
        auto loadedBuffer = std::atomic_load(&gSampleBuffer);
        if (loadedBuffer != sampleBuffer) {
            sampleBuffer = std::move(loadedBuffer);
            sampleFrameCount = sampleBuffer ? sampleBuffer->frameCount() : 0;
            if (sampleBuffer && sampleBuffer->sampleRate > 0) {
                sampleIncrement = static_cast<double>(sampleBuffer->sampleRate) / sampleRate;
            } else {
                sampleIncrement = 1.0;
            }
            samplePlaying = false;
            samplePosition = 0.0;
        }

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

            auto trackInfos = getTracks();
            std::vector<int> trackStepCounts;
            trackStepCounts.reserve(trackInfos.size());

            int activeTrackId = getActiveSequencerTrackId();
            int activeTrackStepCount = 0;

            for (const auto& trackInfo : trackInfos) {
                int count = getSequencerStepCount(trackInfo.id);
                trackStepCounts.push_back(count);
                if (trackInfo.id == activeTrackId) {
                    activeTrackStepCount = count;
                }
            }

            if (activeTrackStepCount <= 0) {
                if (!trackInfos.empty()) {
                    activeTrackStepCount = trackStepCounts.front();
                    activeTrackId = trackInfos.front().id;
                } else {
                    activeTrackStepCount = kSequencerStepsPerPage;
                }
            }

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
                    samplePlaying = false;
                    samplePosition = 0.0;
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
                        int stepCount = activeTrackStepCount > 0 ? activeTrackStepCount : kSequencerStepsPerPage;
                        int nextStep = sequencerCurrentStep.load(std::memory_order_relaxed) + 1;
                        if (nextStep >= stepCount) nextStep = 0;
                        sequencerCurrentStep.store(nextStep, std::memory_order_relaxed);
                        stepAdvanced = true;
                    }

                    int stepCount = activeTrackStepCount > 0 ? activeTrackStepCount : kSequencerStepsPerPage;
                    int currentStep = sequencerCurrentStep.load(std::memory_order_relaxed);
                    if (currentStep >= stepCount) {
                        currentStep = 0;
                        sequencerCurrentStep.store(currentStep, std::memory_order_relaxed);
                    }
                    bool gate = false;
                    bool triggered = false;

                    for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                        int trackId = trackInfos[trackIndex].id;
                        int trackStepCount = trackStepCounts[trackIndex];
                        if (trackStepCount <= 0)
                            continue;
                        if (currentStep >= trackStepCount)
                            continue;
                        if (getTrackStepState(trackId, currentStep)) {
                            gate = true;
                            if (stepAdvanced)
                                triggered = true;
                        }
                    }

                    double target = gate ? 0.8 : 0.0;

                    if (triggered) {
                        envelope = 0.9;
                        if (sampleBuffer && sampleFrameCount > 0) {
                            samplePlaying = true;
                            samplePosition = 0.0;
                        }
                    }

                    if (envelope < target) {
                        envelope += 0.005;
                        if (envelope > target) envelope = target;
                    } else {
                        envelope -= 0.003;
                        if (envelope < target) envelope = target;
                    }
                }

                double leftValue = 0.0;
                double rightValue = 0.0;
                bool producedSample = false;

                if (sampleBuffer && samplePlaying) {
                    size_t index = static_cast<size_t>(samplePosition);
                    if (index < sampleFrameCount) {
                        int channels = std::max(sampleBuffer->channels, 1);
                        const auto& rawSamples = sampleBuffer->samples;
                        int16_t leftSample = rawSamples[index * channels];
                        int16_t rightSample = channels > 1 ? rawSamples[index * channels + 1] : leftSample;
                        leftValue = static_cast<double>(leftSample) / 32768.0;
                        rightValue = static_cast<double>(rightSample) / 32768.0;
                        samplePosition += sampleIncrement;
                        producedSample = true;
                    } else {
                        samplePlaying = false;
                    }
                }

                if (!producedSample) {
                    if (!sampleBuffer) {
                        double sampleValue = sin(phase) * envelope;
                        phase += twoPi * freq / sampleRate;
                        if (phase >= twoPi) phase -= twoPi;
                        leftValue = sampleValue;
                        rightValue = sampleValue;
                    } else {
                        leftValue = 0.0;
                        rightValue = 0.0;
                    }
                }

                leftValue = std::clamp(leftValue, -1.0, 1.0);
                rightValue = std::clamp(rightValue, -1.0, 1.0);

                samples[i * 2] = static_cast<short>(leftValue * 32767.0);
                samples[i * 2 + 1] = static_cast<short>(rightValue * 32767.0);
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
    if (!std::atomic_load(&gSampleBuffer)) {
        auto defaultSample = findDefaultSamplePath();
        if (!defaultSample.empty()) {
            SampleBuffer buffer;
            if (loadSampleFromFile(defaultSample, buffer)) {
                auto sharedBuffer = std::make_shared<SampleBuffer>(std::move(buffer));
                std::shared_ptr<const SampleBuffer> immutableBuffer = std::move(sharedBuffer);
                std::atomic_store(&gSampleBuffer, std::move(immutableBuffer));
            }
        }
    }
    running = true;
    audioThread = std::thread(audioLoop);
}

void shutdownAudio() {
    running = false;
    isPlaying = false;
    if (audioThread.joinable()) audioThread.join();
}

bool loadSampleFile(const std::filesystem::path& path) {
    SampleBuffer buffer;
    if (!loadSampleFromFile(path, buffer))
        return false;

    auto sharedBuffer = std::make_shared<SampleBuffer>(std::move(buffer));
    std::shared_ptr<const SampleBuffer> immutableBuffer = std::move(sharedBuffer);
    std::atomic_store(&gSampleBuffer, std::move(immutableBuffer));
    return true;
}
