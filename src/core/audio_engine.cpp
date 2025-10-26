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
#include <string>
#include <vector>
#include <unordered_map>
#include <mutex>

#include "core/tracks.h"
#include "core/sample_loader.h"
#include "core/sequencer.h"
#include "core/audio_device_handler.h"

std::atomic<bool> isPlaying = false;
static bool running = true;
static std::thread audioThread;
static std::mutex deviceMutex;
static std::wstring requestedDeviceId;
static std::wstring activeDeviceId;
static std::wstring activeDeviceName;
static std::wstring activeRequestedDeviceId;
static std::atomic<bool> deviceChangeRequested{false};

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

struct BiquadFilter {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
    double z1L = 0.0;
    double z2L = 0.0;
    double z1R = 0.0;
    double z2R = 0.0;
};

constexpr double kPi = 3.14159265358979323846264338327950288;
constexpr double kLowShelfFrequency = 200.0;
constexpr double kMidPeakFrequency = 1000.0;
constexpr double kHighShelfFrequency = 5000.0;
constexpr double kMidPeakQ = 1.0;

void resetFilterState(BiquadFilter& filter)
{
    filter.z1L = filter.z2L = 0.0;
    filter.z1R = filter.z2R = 0.0;
}

void setBiquadCoefficients(BiquadFilter& filter, double b0, double b1, double b2, double a0, double a1, double a2)
{
    if (std::abs(a0) < 1e-12)
        a0 = 1.0;

    filter.b0 = b0 / a0;
    filter.b1 = b1 / a0;
    filter.b2 = b2 / a0;
    filter.a1 = a1 / a0;
    filter.a2 = a2 / a0;
}

double processBiquadSample(BiquadFilter& filter, double input, bool rightChannel)
{
    double& z1 = rightChannel ? filter.z1R : filter.z1L;
    double& z2 = rightChannel ? filter.z2R : filter.z2L;

    double y = filter.b0 * input + z1;
    double newZ1 = filter.b1 * input + z2 - filter.a1 * y;
    double newZ2 = filter.b2 * input - filter.a2 * y;
    z1 = newZ1;
    z2 = newZ2;
    return y;
}

double clampFrequency(double sampleRate, double frequency)
{
    double sr = std::max(sampleRate, 1.0);
    double nyquist = sr * 0.5;
    double minFreq = 10.0;
    double maxFreq = std::max(nyquist - 10.0, minFreq);
    return std::clamp(frequency, minFreq, maxFreq);
}

void configureLowShelf(BiquadFilter& filter, double sampleRate, double frequency, double gainDb)
{
    double sr = std::max(sampleRate, 1.0);
    double w0 = 2.0 * kPi * clampFrequency(sr, frequency) / sr;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double A = std::pow(10.0, gainDb / 40.0);
    double alpha = sinw0 / 2.0 * std::sqrt(2.0);
    double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    double b0 = A * ((A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha);
    double b1 = 2.0 * A * ((A - 1.0) - (A + 1.0) * cosw0);
    double b2 = A * ((A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha);
    double a0 = (A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha;
    double a1 = -2.0 * ((A - 1.0) + (A + 1.0) * cosw0);
    double a2 = (A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha;
    setBiquadCoefficients(filter, b0, b1, b2, a0, a1, a2);
}

void configureHighShelf(BiquadFilter& filter, double sampleRate, double frequency, double gainDb)
{
    double sr = std::max(sampleRate, 1.0);
    double w0 = 2.0 * kPi * clampFrequency(sr, frequency) / sr;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double A = std::pow(10.0, gainDb / 40.0);
    double alpha = sinw0 / 2.0 * std::sqrt(2.0);
    double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    double b0 = A * ((A + 1.0) + (A - 1.0) * cosw0 + twoSqrtAAlpha);
    double b1 = -2.0 * A * ((A - 1.0) + (A + 1.0) * cosw0);
    double b2 = A * ((A + 1.0) + (A - 1.0) * cosw0 - twoSqrtAAlpha);
    double a0 = (A + 1.0) - (A - 1.0) * cosw0 + twoSqrtAAlpha;
    double a1 = 2.0 * ((A - 1.0) - (A + 1.0) * cosw0);
    double a2 = (A + 1.0) - (A - 1.0) * cosw0 - twoSqrtAAlpha;
    setBiquadCoefficients(filter, b0, b1, b2, a0, a1, a2);
}

void configurePeaking(BiquadFilter& filter, double sampleRate, double frequency, double gainDb, double Q)
{
    double sr = std::max(sampleRate, 1.0);
    double w0 = 2.0 * kPi * clampFrequency(sr, frequency) / sr;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double A = std::pow(10.0, gainDb / 40.0);
    double safeQ = std::max(Q, 0.1);
    double alpha = sinw0 / (2.0 * safeQ);

    double b0 = 1.0 + alpha * A;
    double b1 = -2.0 * cosw0;
    double b2 = 1.0 - alpha * A;
    double a0 = 1.0 + alpha / A;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha / A;
    setBiquadCoefficients(filter, b0, b1, b2, a0, a1, a2);
}

double midiNoteToFrequency(int midiNote)
{
    int clamped = std::clamp(midiNote, 0, 127);
    return 440.0 * std::pow(2.0, (static_cast<double>(clamped) - 69.0) / 12.0);
}

struct TrackPlaybackState {
    TrackType type = TrackType::Synth;
    double envelope = 0.0;
    int currentMidiNote = 69;
    double currentFrequency = midiNoteToFrequency(69);
    bool samplePlaying = false;
    double samplePosition = 0.0;
    double sampleIncrement = 1.0;
    std::shared_ptr<const SampleBuffer> sampleBuffer;
    size_t sampleFrameCount = 0;
    double volume = 1.0;
    double pan = 0.0;
    double lowGain = 0.0;
    double midGain = 0.0;
    double highGain = 0.0;
    double lastSampleRate = 0.0;
    BiquadFilter lowShelf;
    BiquadFilter midPeak;
    BiquadFilter highShelf;
    struct SynthVoice {
        int midiNote = 69;
        double frequency = midiNoteToFrequency(69);
        double phase = 0.0;
    };
    std::vector<SynthVoice> voices;
};

// Added proper includes and scope for updateMixerState (fix undefined type errors)
void updateMixerState(TrackPlaybackState& state, const Track& track, double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double newVolume = std::clamp(static_cast<double>(track.volume), 0.0, 1.0);
    double newPan = std::clamp(static_cast<double>(track.pan), -1.0, 1.0);
    double newLow = static_cast<double>(track.lowGainDb);
    double newMid = static_cast<double>(track.midGainDb);
    double newHigh = static_cast<double>(track.highGainDb);

    bool sampleRateChanged = std::abs(state.lastSampleRate - sr) > 1e-6;
    bool lowChanged = sampleRateChanged || std::abs(state.lowGain - newLow) > 1e-6;
    bool midChanged = sampleRateChanged || std::abs(state.midGain - newMid) > 1e-6;
    bool highChanged = sampleRateChanged || std::abs(state.highGain - newHigh) > 1e-6;

    if (lowChanged)
    {
        configureLowShelf(state.lowShelf, sr, kLowShelfFrequency, newLow);
        state.lowGain = newLow;
    }
    if (midChanged)
    {
        configurePeaking(state.midPeak, sr, kMidPeakFrequency, newMid, kMidPeakQ);
        state.midGain = newMid;
    }
    if (highChanged)
    {
        configureHighShelf(state.highShelf, sr, kHighShelfFrequency, newHigh);
        state.highGain = newHigh;
    }

    if (sampleRateChanged || lowChanged || midChanged || highChanged)
    {
        resetFilterState(state.lowShelf);
        resetFilterState(state.midPeak);
        resetFilterState(state.highShelf);
    }

    state.volume = newVolume;
    state.pan = newPan;
    state.lastSampleRate = sr;
}

} // namespace

void audioLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    AudioDeviceHandler deviceHandler;
    UINT32 bufferFrameCount = 0;
    const WAVEFORMATEX* format = nullptr;
    double sampleRate = 44100.0;
    const double twoPi = 6.283185307179586;
    double stepSampleCounter = 0.0;
    bool previousPlaying = false;
    std::unordered_map<int, TrackPlaybackState> playbackStates;
    bool deviceReady = false;

    while (running) {
        bool changeRequested = deviceChangeRequested.exchange(false);
        std::wstring desiredDeviceId;
        {
            std::lock_guard<std::mutex> lock(deviceMutex);
            desiredDeviceId = requestedDeviceId;
        }

        if (changeRequested || !deviceReady) {
            deviceHandler.stop();
            deviceHandler.shutdown();
            deviceReady = false;

            bool usedFallback = false;
            bool initialized = deviceHandler.initialize(desiredDeviceId);
            if (!initialized && !desiredDeviceId.empty()) {
                initialized = deviceHandler.initialize();
                usedFallback = initialized;
            }

            if (!initialized) {
                {
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    if (!usedFallback) {
                        activeDeviceId.clear();
                        activeDeviceName.clear();
                    }
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            if (!deviceHandler.start()) {
                deviceHandler.shutdown();
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            bufferFrameCount = deviceHandler.bufferFrameCount();
            format = deviceHandler.format();
            sampleRate = format ? static_cast<double>(format->nSamplesPerSec) : 44100.0;
            deviceReady = true;
            stepSampleCounter = 0.0;
            previousPlaying = false;
            playbackStates.clear();

            {
                std::lock_guard<std::mutex> lock(deviceMutex);
                activeDeviceId = deviceHandler.deviceId();
                activeDeviceName = deviceHandler.deviceName();
                if (usedFallback) {
                    requestedDeviceId.clear();
                    activeRequestedDeviceId.clear();
                } else {
                    requestedDeviceId = desiredDeviceId;
                    activeRequestedDeviceId = desiredDeviceId;
                }
            }
        }

        if (!deviceReady || bufferFrameCount == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        UINT32 padding = 0;
        HRESULT paddingResult = deviceHandler.currentPadding(&padding);
        if (paddingResult == AUDCLNT_E_DEVICE_INVALIDATED) {
            deviceHandler.stop();
            deviceHandler.shutdown();
            {
                std::lock_guard<std::mutex> lock(deviceMutex);
                activeDeviceId.clear();
                activeDeviceName.clear();
            }
            bufferFrameCount = 0;
            format = nullptr;
            deviceReady = false;
            continue;
        } else if (FAILED(paddingResult)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        UINT32 available = bufferFrameCount > padding ? bufferFrameCount - padding : 0;
        if (available > 0) {
            BYTE* data;
            HRESULT bufferResult = deviceHandler.getBuffer(available, &data);
            if (bufferResult == AUDCLNT_E_DEVICE_INVALIDATED) {
                deviceHandler.stop();
                deviceHandler.shutdown();
                {
                    std::lock_guard<std::mutex> lock(deviceMutex);
                    activeDeviceId.clear();
                    activeDeviceName.clear();
                }
                bufferFrameCount = 0;
                format = nullptr;
                deviceReady = false;
                continue;
            } else if (FAILED(bufferResult)) {
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
                    state.currentMidiNote = 69;
                    state.currentFrequency = midiNoteToFrequency(69);
                    state.voices.clear();
                } else {
                    state.sampleBuffer.reset();
                    state.sampleFrameCount = 0;
                    state.samplePlaying = false;
                    state.samplePosition = 0.0;
                    if (state.currentMidiNote < 0 || state.currentMidiNote > 127) {
                        state.currentMidiNote = 69;
                    }
                    state.currentFrequency = midiNoteToFrequency(state.currentMidiNote);
                }

                updateMixerState(state, trackInfo, sampleRate);
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

                    auto getNotesForStep = [](int trackId, int stepIndex) {
                        std::vector<int> notes = trackGetStepNotes(trackId, stepIndex);
                        if (notes.empty()) {
                            int fallback = trackGetStepNote(trackId, stepIndex);
                            if (fallback >= 0)
                                notes.push_back(fallback);
                        }
                        return notes;
                    };

                    for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                        const auto& trackInfo = trackInfos[trackIndex];
                        int trackStepCount = trackStepCounts[trackIndex];
                        auto stateIt = playbackStates.find(trackInfo.id);
                        if (stateIt == playbackStates.end())
                            continue;
                        auto& state = stateIt->second;

                        bool gate = false;
                        bool triggered = false;
                        std::vector<int> triggeredNotes;
                        if (trackStepCount > 0 && currentStep < trackStepCount) {
                            if (getTrackStepState(trackInfo.id, currentStep)) {
                                gate = true;
                                if (stepAdvanced) {
                                    if (trackInfo.type == TrackType::Synth) {
                                        triggeredNotes = getNotesForStep(trackInfo.id, currentStep);
                                        triggered = !triggeredNotes.empty();
                                    } else {
                                        triggered = true;
                                    }
                                }
                            }
                        }

                        double trackLeft = 0.0;
                        double trackRight = 0.0;

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
                                    trackLeft = static_cast<double>(leftSample);
                                    trackRight = static_cast<double>(rightSample);
                                    state.samplePosition += state.sampleIncrement;
                                } else {
                                    state.samplePlaying = false;
                                }
                            }
                        } else {
                            double target = gate ? 0.8 : 0.0;

                            if (triggered) {
                                std::vector<int> sustainedNotes;
                                if (trackStepCount > 0) {
                                    int previousStep = currentStep - 1;
                                    if (previousStep < 0)
                                        previousStep = trackStepCount - 1;
                                    if (previousStep >= 0 && previousStep < trackStepCount && previousStep != currentStep &&
                                        getTrackStepState(trackInfo.id, previousStep)) {
                                        auto previousNotes = getNotesForStep(trackInfo.id, previousStep);
                                        for (int note : triggeredNotes) {
                                            if (std::find(previousNotes.begin(), previousNotes.end(), note) != previousNotes.end()) {
                                                sustainedNotes.push_back(note);
                                            }
                                        }
                                    }
                                }

                                std::vector<TrackPlaybackState::SynthVoice> updatedVoices;
                                updatedVoices.reserve(triggeredNotes.size());

                                bool createdNewVoice = false;
                                for (int note : triggeredNotes) {
                                    auto existingIt = std::find_if(state.voices.begin(), state.voices.end(),
                                        [note](const TrackPlaybackState::SynthVoice& voice) {
                                            return voice.midiNote == note;
                                        });

                                    bool isSustained = std::find(sustainedNotes.begin(), sustainedNotes.end(), note) != sustainedNotes.end();
                                    if (existingIt != state.voices.end() && isSustained) {
                                        updatedVoices.push_back(*existingIt);
                                    } else {
                                        TrackPlaybackState::SynthVoice voice{};
                                        voice.midiNote = note;
                                        voice.frequency = midiNoteToFrequency(note);
                                        voice.phase = 0.0;
                                        updatedVoices.push_back(voice);
                                        createdNewVoice = true;
                                    }
                                }

                                state.voices = std::move(updatedVoices);

                                if (!state.voices.empty()) {
                                    state.currentMidiNote = state.voices.front().midiNote;
                                    state.currentFrequency = state.voices.front().frequency;
                                } else {
                                    state.currentMidiNote = 69;
                                    state.currentFrequency = midiNoteToFrequency(69);
                                }

                                if (createdNewVoice) {
                                    state.envelope = 0.9;
                                }
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

                            double sampleValue = 0.0;
                            if (!state.voices.empty()) {
                                for (auto& voice : state.voices) {
                                    double waveform = 0.0;
                                    switch (trackInfo.synthWaveType)
                                    {
                                    case SynthWaveType::Sine:
                                        waveform = std::sin(voice.phase);
                                        break;
                                    case SynthWaveType::Square:
                                        waveform = (voice.phase < twoPi * 0.5) ? 1.0 : -1.0;
                                        break;
                                    case SynthWaveType::Saw:
                                    {
                                        double normalized = voice.phase / twoPi;
                                        waveform = 2.0 * normalized - 1.0;
                                        break;
                                    }
                                    case SynthWaveType::Triangle:
                                    {
                                        double normalized = voice.phase / twoPi;
                                        double centered = 2.0 * normalized - 1.0;
                                        waveform = 2.0 * (1.0 - std::abs(centered)) - 1.0;
                                        break;
                                    }
                                    }
                                    sampleValue += waveform;
                                    double increment = twoPi * voice.frequency / sampleRate;
                                    voice.phase += increment;
                                    if (voice.phase >= twoPi)
                                    {
                                        voice.phase = std::fmod(voice.phase, twoPi);
                                    }
                                }
                                sampleValue /= static_cast<double>(state.voices.size());
                            }
                            sampleValue *= state.envelope;
                            if (!gate && state.envelope <= 0.0001) {
                                state.voices.clear();
                            }
                            trackLeft = sampleValue;
                            trackRight = sampleValue;
                        }

                        double processedLeft = processBiquadSample(state.lowShelf, trackLeft, false);
                        processedLeft = processBiquadSample(state.midPeak, processedLeft, false);
                        processedLeft = processBiquadSample(state.highShelf, processedLeft, false);

                        double processedRight = processBiquadSample(state.lowShelf, trackRight, true);
                        processedRight = processBiquadSample(state.midPeak, processedRight, true);
                        processedRight = processBiquadSample(state.highShelf, processedRight, true);

                        double panAmount = std::clamp((state.pan + 1.0) * 0.5, 0.0, 1.0);
                        double leftPanGain = std::cos(panAmount * (kPi * 0.5));
                        double rightPanGain = std::sin(panAmount * (kPi * 0.5));
                        double volumeGain = state.volume;

                        leftValue += processedLeft * volumeGain * leftPanGain;
                        rightValue += processedRight * volumeGain * rightPanGain;
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

std::vector<AudioOutputDevice> getAvailableAudioOutputDevices() {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninitialize = SUCCEEDED(hr);
    if (hr == RPC_E_CHANGED_MODE) {
        shouldUninitialize = false;
    } else if (FAILED(hr)) {
        return {};
    }

    std::vector<AudioOutputDevice> result;
    auto devices = AudioDeviceHandler::enumerateRenderDevices();
    result.reserve(devices.size());
    for (auto& device : devices) {
        AudioOutputDevice info;
        info.id = std::move(device.id);
        info.name = std::move(device.name);
        result.push_back(std::move(info));
    }

    if (shouldUninitialize) {
        CoUninitialize();
    }
    return result;
}

AudioOutputDevice getActiveAudioOutputDevice() {
    std::lock_guard<std::mutex> lock(deviceMutex);
    AudioOutputDevice info;
    info.id = activeDeviceId;
    info.name = activeDeviceName;
    return info;
}

std::wstring getRequestedAudioOutputDeviceId() {
    std::lock_guard<std::mutex> lock(deviceMutex);
    return activeRequestedDeviceId;
}

bool setActiveAudioOutputDevice(const std::wstring& deviceId) {
    {
        std::lock_guard<std::mutex> lock(deviceMutex);
        requestedDeviceId = deviceId;
        activeRequestedDeviceId = deviceId;
    }
    deviceChangeRequested.store(true, std::memory_order_release);
    return true;
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
