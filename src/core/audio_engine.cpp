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
#include <unordered_map>

#include "core/sample_loader.h"
#include "core/sequencer.h"
#include "core/tracks.h"
#include "core/audio_device_handler.h"

std::atomic<bool> isPlaying = false;
static bool running = true;
static std::thread audioThread;

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

struct TrackPlaybackState {
    TrackType type = TrackType::Synth;
    double envelope = 0.0;
    double phase = 0.0;
    bool samplePlaying = false;
    double samplePosition = 0.0;
    double sampleIncrement = 1.0;
    std::shared_ptr<const SampleBuffer> sampleBuffer;
    size_t sampleFrameCount = 0;
};

} // namespace

void audioLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    AudioDeviceHandler deviceHandler;
    if (!deviceHandler.initialize()) {
        return;
    }
    if (!deviceHandler.start()) {
        return;
    }

    UINT32 bufferFrameCount = deviceHandler.bufferFrameCount();
    const WAVEFORMATEX* format = deviceHandler.format();

    const double baseFreq = 440.0;
    const double twoPi = 6.283185307179586;
    const double sampleRate = format ? static_cast<double>(format->nSamplesPerSec) : 44100.0;
    double stepSampleCounter = 0.0;
    bool previousPlaying = false;
    std::unordered_map<int, TrackPlaybackState> playbackStates;

    while (running) {
        UINT32 padding = deviceHandler.currentPadding();
        UINT32 available = bufferFrameCount - padding;
        if (available > 0) {
            BYTE* data;
            if (!deviceHandler.getBuffer(available, &data)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
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

            for (auto it = playbackStates.begin(); it != playbackStates.end(); ) {
                int trackId = it->first;
                bool exists = std::any_of(trackInfos.begin(), trackInfos.end(), [trackId](const Track& track) {
                    return track.id == trackId;
                });
                if (!exists) {
                    it = playbackStates.erase(it);
                } else {
                    ++it;
                }
            }

            for (const auto& trackInfo : trackInfos) {
                auto& state = playbackStates[trackInfo.id];
                state.type = trackInfo.type;
                if (trackInfo.type == TrackType::Sample) {
                    auto sampleBuffer = trackGetSampleBuffer(trackInfo.id);
                    if (sampleBuffer != state.sampleBuffer) {
                        state.sampleBuffer = std::move(sampleBuffer);
                        state.sampleFrameCount = state.sampleBuffer ? state.sampleBuffer->frameCount() : 0;
                        if (state.sampleBuffer && state.sampleBuffer->sampleRate > 0) {
                            state.sampleIncrement = static_cast<double>(state.sampleBuffer->sampleRate) / sampleRate;
                        } else {
                            state.sampleIncrement = 1.0;
                        }
                        state.samplePlaying = false;
                        state.samplePosition = 0.0;
                    }
                    state.envelope = 0.0;
                    state.phase = 0.0;
                } else {
                    state.sampleBuffer.reset();
                    state.sampleFrameCount = 0;
                    state.samplePlaying = false;
                    state.samplePosition = 0.0;
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
                    for (auto& [_, state] : playbackStates) {
                        state.envelope *= 0.92;
                        if (state.envelope < 0.0001)
                            state.envelope = 0.0;
                        state.samplePlaying = false;
                        state.samplePosition = 0.0;
                    }
                } else {
                    if (!previousPlaying) {
                        sequencerResetRequested.store(true, std::memory_order_relaxed);
                    }
                    previousPlaying = true;

                    if (sequencerResetRequested.exchange(false, std::memory_order_acq_rel)) {
                        sequencerCurrentStep.store(0, std::memory_order_relaxed);
                        stepSampleCounter = 0.0;
                        for (auto& [_, state] : playbackStates) {
                            state.envelope = 0.0;
                            state.samplePlaying = false;
                            state.samplePosition = 0.0;
                        }
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

                    double leftValue = 0.0;
                    double rightValue = 0.0;

                    for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                        const auto& trackInfo = trackInfos[trackIndex];
                        int trackStepCount = trackStepCounts[trackIndex];
                        auto stateIt = playbackStates.find(trackInfo.id);
                        if (stateIt == playbackStates.end())
                            continue;
                        auto& state = stateIt->second;

                        bool gate = false;
                        bool triggered = false;
                        if (trackStepCount > 0 && currentStep < trackStepCount) {
                            if (getTrackStepState(trackInfo.id, currentStep)) {
                                gate = true;
                                if (stepAdvanced)
                                    triggered = true;
                            }
                        }

                        if (trackInfo.type == TrackType::Sample) {
                            if (triggered) {
                                if (state.sampleBuffer && state.sampleFrameCount > 0) {
                                    state.samplePlaying = true;
                                    state.samplePosition = 0.0;
                                } else {
                                    state.samplePlaying = false;
                                }
                            }

                            if (state.samplePlaying && state.sampleBuffer) {
                                size_t index = static_cast<size_t>(state.samplePosition);
                                if (index < state.sampleFrameCount) {
                                    int channels = std::max(state.sampleBuffer->channels, 1);
                                    const auto& rawSamples = state.sampleBuffer->samples;
                                    float leftSample = rawSamples[index * channels];
                                    float rightSample = channels > 1 ? rawSamples[index * channels + 1] : leftSample;
                                    leftValue += static_cast<double>(leftSample);
                                    rightValue += static_cast<double>(rightSample);
                                    state.samplePosition += state.sampleIncrement;
                                } else {
                                    state.samplePlaying = false;
                                }
                            }
                        } else {
                            double target = gate ? 0.8 : 0.0;

                            if (triggered) {
                                state.envelope = 0.9;
                            }

                            if (state.envelope < target) {
                                state.envelope += 0.005;
                                if (state.envelope > target)
                                    state.envelope = target;
                            } else {
                                state.envelope -= 0.003;
                                if (state.envelope < target)
                                    state.envelope = target;
                            }

                            double sampleValue = std::sin(state.phase) * state.envelope;
                            state.phase += twoPi * baseFreq / sampleRate;
                            if (state.phase >= twoPi)
                                state.phase -= twoPi;
                            leftValue += sampleValue;
                            rightValue += sampleValue;
                        }
                    }

                    leftValue = std::clamp(leftValue, -1.0, 1.0);
                    rightValue = std::clamp(rightValue, -1.0, 1.0);

                    samples[i * 2] = static_cast<short>(leftValue * 32767.0);
                    samples[i * 2 + 1] = static_cast<short>(rightValue * 32767.0);
                    continue;
                }
                samples[i * 2] = 0;
                samples[i * 2 + 1] = 0;
            }
            deviceHandler.releaseBuffer(available);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    deviceHandler.stop();
    deviceHandler.shutdown();
    CoUninitialize();
}

void initAudio() {
    auto defaultSample = findDefaultSamplePath();
    if (!defaultSample.empty()) {
        SampleBuffer buffer;
        if (loadSampleFromFile(defaultSample, buffer)) {
            auto sharedBuffer = std::make_shared<SampleBuffer>(std::move(buffer));
            std::shared_ptr<const SampleBuffer> immutableBuffer = sharedBuffer;
            auto tracks = getTracks();
            if (!tracks.empty()) {
                trackSetSampleBuffer(tracks.front().id, std::move(immutableBuffer));
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

bool loadSampleFile(int trackId, const std::filesystem::path& path) {
    if (trackId <= 0)
        return false;

    SampleBuffer buffer;
    if (!loadSampleFromFile(path, buffer))
        return false;

    auto sharedBuffer = std::make_shared<SampleBuffer>(std::move(buffer));
    std::shared_ptr<const SampleBuffer> immutableBuffer = std::move(sharedBuffer);
    trackSetSampleBuffer(trackId, std::move(immutableBuffer));
    return true;
}
