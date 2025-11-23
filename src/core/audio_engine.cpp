#include "core/audio_engine.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <thread>
#include <chrono>
#include <atomic>
#include <cmath>
#include <algorithm>
#include <iterator>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <system_error>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <deque>
#include <condition_variable>
#include <future>
#ifdef DEBUG_AUDIO
#include <iostream>
#endif

#include "core/tracks.h"
#include "core/track_type_sample.h"
#include "core/track_type_synth.h"
#include "core/track_type_vst.h"
#include "core/sample_loader.h"
#include "core/sequencer.h"
#include "core/audio_device_handler.h"
#include "core/effects/delay_effect.h"
#include "core/effects/sidechain_processor.h"
#include "core/midi_output.h"
#include "core/mod_matrix.h"
#include "core/mod_matrix_parameters.h"
#include "audio/thread_pool.h"
#include "hosting/VST3Host.h"

std::atomic<bool> isPlaying = false;
static std::atomic<bool> running{true};
static std::thread audioThread;
static std::thread sequencerThread;
static std::atomic<bool> audioSequencerReady{false};
struct AudioDeviceSnapshot
{
    std::wstring requestedId;
    std::wstring activeId;
    std::wstring activeName;
};

static std::array<AudioDeviceSnapshot, 2> gDeviceSnapshots{};
static std::atomic<int> gDeviceSnapshotIndex{0};
static std::array<std::wstring, 2> gRequestedDeviceIds{};
static std::atomic<int> gRequestedDeviceIndex{0};
static std::atomic<bool> deviceChangeRequested{false};
static ThreadPool& getTrackProcessingPool()
{
    auto concurrency = std::thread::hardware_concurrency();
    std::size_t threadCount = (concurrency > 1) ? concurrency - 1 : 2;
    std::size_t queueCapacity = std::max<std::size_t>(getTrackCount(), 64u);
    static ThreadPool pool(threadCount, queueCapacity);
    return pool;
}

static const AudioDeviceSnapshot& getDeviceSnapshot()
{
    return gDeviceSnapshots[gDeviceSnapshotIndex.load(std::memory_order_acquire)];
}

static void publishDeviceSnapshot(const AudioDeviceSnapshot& snapshot)
{
    int nextIndex = gDeviceSnapshotIndex.load(std::memory_order_relaxed) ^ 1;
    gDeviceSnapshots[nextIndex] = snapshot;
    gDeviceSnapshotIndex.store(nextIndex, std::memory_order_release);
}

constexpr std::size_t kMasterWaveformBufferSize = 44100;
struct WaveformBuffer
{
    std::array<float, kMasterWaveformBufferSize> data{};
    std::size_t count = 0;
};

static std::array<WaveformBuffer, 2> masterWaveformBuffers{};
static std::atomic<int> masterWaveformPublishIndex{0};
static int masterWaveformWriteIndex = 1;
constexpr std::size_t kAudioNotificationCapacity = 128;
static std::array<AudioThreadNotification, kAudioNotificationCapacity> gAudioNotificationQueue{};
static std::atomic<std::size_t> gAudioNotificationHead{0};
static std::atomic<std::size_t> gAudioNotificationTail{0};

enum class VstCommandType
{
    Load,
    Unload,
};

struct VstCommand
{
    VstCommandType type = VstCommandType::Load;
    int trackId = -1;
    std::filesystem::path path;
    std::shared_ptr<kj::VST3Host> host;
    std::shared_ptr<std::promise<bool>> completion;
};

static std::mutex vstCommandMutex;
static std::deque<VstCommand> vstCommandQueue;
static std::condition_variable vstCommandCv;
static std::atomic<int> vstOperationsPending{0};
static std::thread vstCommandThread;

static void enqueuePluginLoad(VstCommand&& command)
{
    // Plugin loading happens off the audio thread, so the request queue can
    // use conventional synchronization primitives without affecting the
    // real-time callback.
    std::lock_guard<std::mutex> lock(vstCommandMutex);
    vstCommandQueue.push_back(std::move(command));
    vstOperationsPending.fetch_add(1, std::memory_order_acq_rel);
    vstCommandCv.notify_one();
}

constexpr std::size_t kVstResetCapacity = 128;

struct VstResetBatch
{
    std::array<int, kVstResetCapacity> ids{};
    std::size_t count = 0;
};

static std::array<VstResetBatch, 2> gVstResetBatches{};
static std::atomic<int> gPublishedVstBatch{-1};
static int gVstWriteBatchIndex = 0;

static void writeWaveformSamples(const float* samples, std::size_t sampleCount)
{
    WaveformBuffer& buffer = masterWaveformBuffers[masterWaveformWriteIndex];
    const std::size_t capacity = buffer.data.size();
    if (!samples || sampleCount == 0 || capacity == 0)
        return;

    const std::size_t copyCount = std::min(sampleCount, capacity);
    std::memcpy(buffer.data.data(), samples, sizeof(float) * copyCount);
    buffer.count = copyCount;

    masterWaveformPublishIndex.store(masterWaveformWriteIndex, std::memory_order_release);
    masterWaveformWriteIndex ^= 1;
}

static void enqueueAudioThreadNotification(const std::wstring& title, const std::wstring& message)
{
    const std::size_t tail = gAudioNotificationTail.load(std::memory_order_relaxed);
    const std::size_t nextTail = (tail + 1) % kAudioNotificationCapacity;
    const std::size_t head = gAudioNotificationHead.load(std::memory_order_acquire);

    // Drop the notification if the buffer is full; the audio thread must never block.
    if (nextTail == head)
        return;

    gAudioNotificationQueue[tail] = AudioThreadNotification{title, message};
    gAudioNotificationTail.store(nextTail, std::memory_order_release);
}

static void enqueueVstReset(int trackId)
{
    if (trackId <= 0)
        return;

    auto& batch = gVstResetBatches[gVstWriteBatchIndex];
    if (batch.count < batch.ids.size())
    {
        batch.ids[batch.count++] = trackId;
    }
}

static void publishVstResetBatch()
{
    auto& batch = gVstResetBatches[gVstWriteBatchIndex];
    if (batch.count == 0)
        return;

    gPublishedVstBatch.store(gVstWriteBatchIndex, std::memory_order_release);
    gVstWriteBatchIndex ^= 1;
    gVstResetBatches[gVstWriteBatchIndex].count = 0;
}

bool consumeAudioThreadNotification(AudioThreadNotification& notification)
{
    const std::size_t head = gAudioNotificationHead.load(std::memory_order_acquire);
    const std::size_t tail = gAudioNotificationTail.load(std::memory_order_acquire);

    if (head == tail)
        return false;

    notification = gAudioNotificationQueue[head];
    const std::size_t nextHead = (head + 1) % kAudioNotificationCapacity;
    gAudioNotificationHead.store(nextHead, std::memory_order_release);
    return true;
}

bool requestTrackVstLoad(int trackId, const std::filesystem::path& path)
{
    if (trackId <= 0 || path.empty())
        return false;

    auto host = trackEnsureVstHost(trackId);
    if (!host)
        return false;

    auto completion = std::make_shared<std::promise<bool>>();

    VstCommand command{};
    command.type = VstCommandType::Load;
    command.trackId = trackId;
    command.path = path;
    command.host = std::move(host);
    command.completion = completion;

    enqueuePluginLoad(std::move(command));
    // Do not block the GUI thread; the caller can poll host->isPluginReady().
    return true;
}

bool requestTrackVstUnload(int trackId)
{
    if (trackId <= 0)
        return false;

    auto host = trackGetVstHost(trackId);
    if (!host)
        return false;

    auto completion = std::make_shared<std::promise<bool>>();
    auto result = completion->get_future();

    VstCommand command{};
    command.type = VstCommandType::Unload;
    command.trackId = trackId;
    command.host = std::move(host);
    command.completion = completion;

    std::lock_guard<std::mutex> lock(vstCommandMutex);
    vstCommandQueue.push_back(std::move(command));
    vstOperationsPending.fetch_add(1, std::memory_order_acq_rel);
    vstCommandCv.notify_one();

    // This synchronization is allowed because plugin unloading is non-real-time.
    constexpr auto kUnloadTimeout = std::chrono::seconds(15);
    if (result.wait_for(kUnloadTimeout) != std::future_status::ready)
        return false;

    return result.get();
}

static void vstCommandLoop()
{
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    while (running.load(std::memory_order_acquire))
    {
        std::unique_lock<std::mutex> lock(vstCommandMutex);
        vstCommandCv.wait(lock, [] {
            return !vstCommandQueue.empty() || !running.load(std::memory_order_acquire);
        });

        if (!running.load(std::memory_order_acquire) && vstCommandQueue.empty())
            break;

        if (vstCommandQueue.empty())
            continue;

        auto command = std::move(vstCommandQueue.front());
        vstCommandQueue.pop_front();
        lock.unlock();

        bool success = (command.host != nullptr);
        if (command.host)
        {
            if (command.type == VstCommandType::Load)
                success = command.host->load(command.path.string());
            else
                command.host->unload();
        }

        if (command.completion)
            command.completion->set_value(success);

        if (command.trackId > 0)
        {
            enqueueVstReset(command.trackId);
            publishVstResetBatch();
        }

        vstOperationsPending.fetch_sub(1, std::memory_order_acq_rel);
    }

    {
        std::lock_guard<std::mutex> lock(vstCommandMutex);
        for (auto& pending : vstCommandQueue)
        {
            if (pending.completion)
                pending.completion->set_value(false);
        }
        vstCommandQueue.clear();
        vstOperationsPending.store(0, std::memory_order_release);
    }

    CoUninitialize();
}

namespace {

bool isFloatWaveFormat(const WAVEFORMATEX* format)
{
    if (!format)
        return false;
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32)
        return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
    {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
               extensible->Format.wBitsPerSample == 32;
    }
    return false;
}

bool isPcm16WaveFormat(const WAVEFORMATEX* format)
{
    if (!format)
        return false;
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16)
        return true;
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)))
    {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
               extensible->Format.wBitsPerSample == 16;
    }
    return false;
}

UINT32 bytesPerFrame(const WAVEFORMATEX* format)
{
    if (!format)
        return 0;
    if (format->nBlockAlign != 0)
        return format->nBlockAlign;
    if (format->nChannels == 0 || format->wBitsPerSample == 0)
        return 0;
    return static_cast<UINT32>(format->nChannels) * (format->wBitsPerSample / 8);
}

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

void audioDriverProbeCallback(BYTE*, UINT32, const WAVEFORMATEX*, void* userData)
{
    auto* handler = static_cast<AudioDeviceHandler*>(userData);
    if (handler)
    {
        handler->notifyCallbackExecuted();
    }
}

#ifdef DEBUG_AUDIO
std::string narrowFromWide(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (wchar_t ch : value) {
        if (ch >= 0 && ch <= 0x7F) {
            result.push_back(static_cast<char>(ch));
        } else {
            result.push_back('?');
        }
    }
    return result;
}
#endif

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
constexpr double kSampleEnvelopeSmoothingSeconds = 0.003;
constexpr double kSynthEnvelopeSmoothingSeconds = 0.002;
constexpr double kSynthGainSmoothingSeconds = 0.002;
constexpr double kDelayTimeMinMs = DelayEffect::kMinDelayTimeMs;
constexpr double kDelayTimeMaxMs = DelayEffect::kMaxDelayTimeMs;
constexpr double kDelayFeedbackMin = DelayEffect::kMinFeedback;
constexpr double kDelayFeedbackMax = DelayEffect::kMaxFeedback;
constexpr double kDelayMixMin = DelayEffect::kMinMix;
constexpr double kDelayMixMax = DelayEffect::kMaxMix;
constexpr double kCompressorThresholdMinDb = -60.0;
constexpr double kCompressorThresholdMaxDb = 0.0;
constexpr double kCompressorRatioMin = 1.0;
constexpr double kCompressorRatioMax = 20.0;
constexpr double kCompressorAttackMin = 0.001;
constexpr double kCompressorAttackMax = 1.0;
constexpr double kCompressorReleaseMin = 0.01;
constexpr double kCompressorReleaseMax = 4.0;
constexpr size_t kModSourceCount = 6;
constexpr std::array<double, 3> kDefaultLfoFrequencies = {0.5, 1.0, 2.0};

int cachedModMatrixParameterCount()
{
    static int count = modMatrixGetParameterCount();
    return count;
}

struct ModMatrixParameterLookup
{
    int volume = -1;
    int pan = -1;
    int synthPitch = -1;
    int synthFormant = -1;
    int synthResonance = -1;
    int synthFeedback = -1;
    int synthPitchRange = -1;
    int synthAttack = -1;
    int synthDecay = -1;
    int synthSustain = -1;
    int synthRelease = -1;
    int sampleAttack = -1;
    int sampleRelease = -1;
    int delayMix = -1;
    int compressorThreshold = -1;
    int compressorRatio = -1;
};

const ModMatrixParameterLookup& getModMatrixParameterLookup()
{
    static const ModMatrixParameterLookup lookup = [] {
        ModMatrixParameterLookup result;
        result.volume = modMatrixGetParameterIndex(ModMatrixParameter::Volume);
        result.pan = modMatrixGetParameterIndex(ModMatrixParameter::Pan);
        result.synthPitch = modMatrixGetParameterIndex(ModMatrixParameter::SynthPitch);
        result.synthFormant = modMatrixGetParameterIndex(ModMatrixParameter::SynthFormant);
        result.synthResonance = modMatrixGetParameterIndex(ModMatrixParameter::SynthResonance);
        result.synthFeedback = modMatrixGetParameterIndex(ModMatrixParameter::SynthFeedback);
        result.synthPitchRange = modMatrixGetParameterIndex(ModMatrixParameter::SynthPitchRange);
        result.synthAttack = modMatrixGetParameterIndex(ModMatrixParameter::SynthAttack);
        result.synthDecay = modMatrixGetParameterIndex(ModMatrixParameter::SynthDecay);
        result.synthSustain = modMatrixGetParameterIndex(ModMatrixParameter::SynthSustain);
        result.synthRelease = modMatrixGetParameterIndex(ModMatrixParameter::SynthRelease);
        result.sampleAttack = modMatrixGetParameterIndex(ModMatrixParameter::SampleAttack);
        result.sampleRelease = modMatrixGetParameterIndex(ModMatrixParameter::SampleRelease);
        result.delayMix = modMatrixGetParameterIndex(ModMatrixParameter::DelayMix);
        result.compressorThreshold = modMatrixGetParameterIndex(ModMatrixParameter::CompressorThreshold);
        result.compressorRatio = modMatrixGetParameterIndex(ModMatrixParameter::CompressorRatio);
        return result;
    }();
    return lookup;
}

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

double midiNoteToFrequency(double midiNote)
{
    double clamped = std::clamp(midiNote, 0.0, 127.0);
    return 440.0 * std::pow(2.0, (clamped - 69.0) / 12.0);
}

double computeFormantFrequency(double sampleRate, double normalizedFormant)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double safeNorm = std::clamp(normalizedFormant, 0.0, 1.0);
    constexpr double kMinFormantFreq = 200.0;
    constexpr double kMaxFormantFreq = 8000.0;
    double maxAllowed = std::max(kMinFormantFreq, std::min(sr * 0.45, kMaxFormantFreq));
    return kMinFormantFreq * std::pow(maxAllowed / kMinFormantFreq, safeNorm);
}

double computeFormantResonanceQ(double normalizedResonance)
{
    double safeNorm = std::clamp(normalizedResonance, 0.0, 1.0);
    constexpr double kMinQ = 0.5;
    constexpr double kMaxQ = 12.0;
    return kMinQ + safeNorm * (kMaxQ - kMinQ);
}

void configureFormantFilter(BiquadFilter& filter,
                            double sampleRate,
                            double normalizedFormant,
                            double normalizedResonance)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double frequency = computeFormantFrequency(sr, normalizedFormant);
    double w0 = 2.0 * kPi * clampFrequency(sr, frequency) / sr;
    double cosw0 = std::cos(w0);
    double sinw0 = std::sin(w0);
    double q = computeFormantResonanceQ(normalizedResonance);
    double alpha = sinw0 / (2.0 * std::max(q, 1e-3));

    double b0 = (1.0 - cosw0) * 0.5;
    double b1 = 1.0 - cosw0;
    double b2 = (1.0 - cosw0) * 0.5;
    double a0 = 1.0 + alpha;
    double a1 = -2.0 * cosw0;
    double a2 = 1.0 - alpha;
    setBiquadCoefficients(filter, b0, b1, b2, a0, a1, a2);
}

double computePitchEnvelopeStep(double sampleRate, double rangeSemitones)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double normalized = std::clamp(rangeSemitones / 23.0, 0.0, 1.0);
    double envelopeTime = 0.04 + normalized * 0.26; // seconds
    if (envelopeTime <= 0.0 || !std::isfinite(envelopeTime))
        return 1.0;
    return 1.0 / (envelopeTime * sr);
}

enum class EnvelopeStage
{
    Idle,
    Attack,
    Decay,
    Sustain,
    Release,
};

const char* envelopeStageToString(EnvelopeStage stage)
{
    switch (stage)
    {
    case EnvelopeStage::Idle:
        return "Idle";
    case EnvelopeStage::Attack:
        return "Attack";
    case EnvelopeStage::Decay:
        return "Decay";
    case EnvelopeStage::Sustain:
        return "Sustain";
    case EnvelopeStage::Release:
        return "Release";
    }
    return "Unknown";
}

double advanceEnvelope(EnvelopeStage& stage, double currentValue, double attack, double decay, double sustain,
                       double release, double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double value = currentValue;
    double safeSustain = std::clamp(sustain, 0.0, 1.0);
    auto advanceCurved = [&](double target, double timeSeconds) {
        if (timeSeconds <= 0.0)
        {
            value = target;
            return true;
        }

        double totalSamples = std::max(timeSeconds * sr, 1.0);
        constexpr double epsilon = 1e-5;
        double coefficient = std::exp(std::log(epsilon) / totalSamples);
        if (!std::isfinite(coefficient) || coefficient < 0.0 || coefficient >= 1.0)
        {
            coefficient = 0.0;
        }

        double delta = (target - value) * (1.0 - coefficient);
        if (!std::isfinite(delta))
        {
            value = target;
            return true;
        }

        double next = value + delta;
        if (target >= value)
            next = std::min(next, target);
        else
            next = std::max(next, target);

        value = next;

        double tolerance = std::max(1e-5, std::abs(target) * 1e-5);
        if (std::abs(value - target) <= tolerance)
        {
            value = target;
            return true;
        }
        return false;
    };

    switch (stage)
    {
    case EnvelopeStage::Idle:
        value = 0.0;
        break;
    case EnvelopeStage::Attack:
        if (advanceCurved(1.0, attack))
            stage = EnvelopeStage::Decay;
        break;
    case EnvelopeStage::Decay:
        if (advanceCurved(safeSustain, decay))
            stage = EnvelopeStage::Sustain;
        break;
    case EnvelopeStage::Sustain:
        value = safeSustain;
        break;
    case EnvelopeStage::Release:
        if (advanceCurved(0.0, release))
        {
            stage = EnvelopeStage::Idle;
            value = 0.0;
        }
        break;
    }

    if (!std::isfinite(value))
        value = 0.0;
    if (value < 0.0)
        value = 0.0;
    if (value > 1.0)
        value = 1.0;

    return value;
}

struct TrackModulationState
{
    std::array<double, 3> lfoPhase{0.0, 0.0, 0.0};
    std::array<double, 3> lfoValue{0.0, 0.0, 0.0};
    std::atomic<double> envelopeValue{0.0};
    std::array<double, 2> macroValue{0.0, 0.0};
    std::vector<double> parameterAmounts;
};

void prepareModulationParameters(TrackModulationState& modulation)
{
    int parameterCount = cachedModMatrixParameterCount();
    if (parameterCount < 0)
        parameterCount = 0;
    size_t desiredSize = static_cast<size_t>(parameterCount);
    if (modulation.parameterAmounts.size() != desiredSize)
        modulation.parameterAmounts.assign(desiredSize, 0.0);
    else
        std::fill(modulation.parameterAmounts.begin(), modulation.parameterAmounts.end(), 0.0);
}

struct TrackPlaybackState {
    TrackType type = TrackType::Synth;
    int currentMidiNote = 69;
    double currentFrequency = midiNoteToFrequency(69);
    int currentStep = 0;
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
    double feedbackAmount = 0.0;
    double formantNormalized = 0.5;
    double formantResonance = 0.2;
    double formantBlend = 1.0;
    BiquadFilter formantFilter;
    double pitchBaseOffset = 0.0;
    double pitchRangeSemitones = 0.0;
    double pitchEnvelope = 0.0;
    double pitchEnvelopeStep = 1.0;
    double stepVelocity = 1.0;
    double stepPan = 0.0;
    double stepPitchOffset = 0.0;
    double lastAppliedFormant = -1.0;
    double lastAppliedResonance = -1.0;
    int lastParameterStep = -1;
    BiquadFilter lowShelf;
    BiquadFilter midPeak;
    BiquadFilter highShelf;
    double synthAttack = 0.01;
    double synthDecay = 0.2;
    double synthSustain = 0.8;
    double synthRelease = 0.3;
    bool synthPhaseSync = false;
    double synthGainSmoothed = 1.0;
    double sampleEnvelope = 0.0;
    double sampleEnvelopeSmoothed = 0.0;
    EnvelopeStage sampleEnvelopeStage = EnvelopeStage::Idle;
    double sampleAttack = 0.005;
    double sampleRelease = 0.3;
    double sampleLastLeft = 0.0;
    double sampleLastRight = 0.0;
    bool sampleTailActive = false;
    bool eqEnabled = true;
    bool delayEnabled = false;
    double delayTimeMs = 350.0;
    double delayFeedback = 0.35;
    double delayMix = 0.4;
    std::unique_ptr<DelayEffect> delayEffect;
    double delaySampleRate = 0.0;
    bool delayParametersDirty = false;
    bool compressorEnabled = false;
    double compressorThresholdDb = -12.0;
    double compressorRatio = 4.0;
    double compressorAttack = 0.01;
    double compressorRelease = 0.2;
    double compressorGain = 1.0;
    double compressorAttackCoeff = 0.0;
    double compressorReleaseCoeff = 0.0;
    SidechainProcessor sidechain;
    double resetFadeGain = 1.0;
    double resetFadeStep = 0.0;
    int resetFadeSamples = 0;
    bool resetScheduled = false;
    SequencerResetReason resetReason = SequencerResetReason::Manual;
    bool vstPrepared = false;
    bool vstPrepareErrorNotified = false;
    double vstPreparedSampleRate = 0.0;
    int vstPreparedBlockSize = 0;
    struct SynthVoice {
        int midiNote = 69;
        double frequency = midiNoteToFrequency(69);
        double phase = 0.0;
        double lastOutput = 0.0;
        double velocity = 1.0;
        double velocitySmoothed = 1.0;
        double envelope = 0.0;
        EnvelopeStage envelopeStage = EnvelopeStage::Idle;
    };
    std::vector<SynthVoice> voices;
    int midiChannel = 0;
    int midiPort = -1;
    std::vector<int> activeMidiNotes;
    TrackModulationState modulation;
};

void releaseDelayEffect(TrackPlaybackState& state)
{
    state.delayEffect.reset();
    state.delaySampleRate = 0.0;
    state.delayParametersDirty = false;
}

void resetSamplePlaybackState(TrackPlaybackState& state)
{
    state.samplePlaying = false;
    state.samplePosition = 0.0;
    state.sampleIncrement = 1.0;
    state.sampleEnvelope = 0.0;
    state.sampleEnvelopeSmoothed = 0.0;
    state.sampleEnvelopeStage = EnvelopeStage::Idle;
    state.sampleTailActive = false;
    state.sampleLastLeft = 0.0;
    state.sampleLastRight = 0.0;
    state.modulation.envelopeValue.store(0.0, std::memory_order_relaxed);
    prepareModulationParameters(state.modulation);
    state.lastAppliedFormant = -1.0;
    state.lastAppliedResonance = -1.0;
}

void resetSynthPlaybackState(TrackPlaybackState& state)
{
    state.pitchEnvelope = 0.0;
    state.voices.clear();
    state.synthGainSmoothed = 1.0;
    resetFilterState(state.formantFilter);
    state.modulation.envelopeValue.store(0.0, std::memory_order_relaxed);
    prepareModulationParameters(state.modulation);
    state.lastAppliedFormant = -1.0;
    state.lastAppliedResonance = -1.0;
}

void ensureDelayEffect(TrackPlaybackState& state, double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;

    if (!state.delayEffect)
    {
        state.delayEffect = std::make_unique<DelayEffect>(sr);
        state.delaySampleRate = sr;
        state.delayParametersDirty = true;
        return;
    }

    if (std::abs(state.delaySampleRate - sr) > 1e-6)
    {
        state.delayEffect->setSampleRate(sr);
        state.delaySampleRate = sr;
        state.delayParametersDirty = true;
    }
}

// Added proper includes and scope for updateMixerState (fix undefined type errors)
void updateMixerState(TrackPlaybackState& state, const Track& track, double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double newVolume = std::clamp(static_cast<double>(track.volume), 0.0, 1.0);
    double newPan = std::clamp(static_cast<double>(track.pan), -1.0, 1.0);
    double newLow = static_cast<double>(track.lowGainDb);
    double newMid = static_cast<double>(track.midGainDb);
    double newHigh = static_cast<double>(track.highGainDb);
    double newFormant = std::clamp(static_cast<double>(track.formant), 0.0, 1.0);
    double newResonance = std::clamp(static_cast<double>(track.resonance), 0.0, 1.0);
    double newFeedback = std::clamp(static_cast<double>(track.feedback), 0.0, 1.0);
    double newPitch = static_cast<double>(track.pitch);
    double newPitchRange = std::max(0.0, static_cast<double>(track.pitchRange) - 1.0);
    double newSynthAttack = std::clamp(static_cast<double>(track.synthAttack), 0.0, 4.0);
    double newSynthDecay = std::clamp(static_cast<double>(track.synthDecay), 0.0, 4.0);
    double newSynthSustain = std::clamp(static_cast<double>(track.synthSustain), 0.0, 1.0);
    double newSynthRelease = std::clamp(static_cast<double>(track.synthRelease), 0.0, 4.0);
    bool newSynthPhaseSync = track.synthPhaseSync;
    double safeSynthAttack = std::max(newSynthAttack, kSynthEnvelopeSmoothingSeconds);
    double safeSynthDecay = std::max(newSynthDecay, kSynthEnvelopeSmoothingSeconds);
    double safeSynthRelease = std::max(newSynthRelease, kSynthEnvelopeSmoothingSeconds);
    double newSampleAttack = std::clamp(static_cast<double>(track.sampleAttack), 0.0, 4.0);
    double newSampleRelease = std::clamp(static_cast<double>(track.sampleRelease), 0.0, 4.0);
    double newDelayTime = std::clamp(static_cast<double>(track.delayTimeMs), kDelayTimeMinMs, kDelayTimeMaxMs);
    double newDelayFeedback = std::clamp(static_cast<double>(track.delayFeedback), kDelayFeedbackMin, kDelayFeedbackMax);
    double newDelayMix = std::clamp(static_cast<double>(track.delayMix), kDelayMixMin, kDelayMixMax);
    bool newCompressorEnabled = track.compressorEnabled;
    double newCompressorThreshold = std::clamp(static_cast<double>(track.compressorThresholdDb),
                                               kCompressorThresholdMinDb,
                                               kCompressorThresholdMaxDb);
    double newCompressorRatio = std::clamp(static_cast<double>(track.compressorRatio),
                                           kCompressorRatioMin,
                                           kCompressorRatioMax);
    double newCompressorAttack = std::clamp(static_cast<double>(track.compressorAttack),
                                            kCompressorAttackMin,
                                            kCompressorAttackMax);
    double newCompressorRelease = std::clamp(static_cast<double>(track.compressorRelease),
                                             kCompressorReleaseMin,
                                             kCompressorReleaseMax);
    bool newSidechainEnabled = track.sidechainEnabled;
    int newSidechainSourceTrackId = track.sidechainSourceTrackId;
    double newSidechainAmount = std::clamp(static_cast<double>(track.sidechainAmount), 0.0, 1.0);
    double newSidechainAttack = std::clamp(static_cast<double>(track.sidechainAttack), 0.0, 4.0);
    double newSidechainRelease = std::clamp(static_cast<double>(track.sidechainRelease), 0.0, 4.0);
    bool newEqEnabled = track.eqEnabled;
    bool requestedDelayEnabled = track.delayEnabled;

    bool sampleRateChanged = std::abs(state.lastSampleRate - sr) > 1e-6;
    bool lowChanged = sampleRateChanged || std::abs(state.lowGain - newLow) > 1e-6;
    bool midChanged = sampleRateChanged || std::abs(state.midGain - newMid) > 1e-6;
    bool highChanged = sampleRateChanged || std::abs(state.highGain - newHigh) > 1e-6;
    bool formantChanged = sampleRateChanged || std::abs(state.formantNormalized - newFormant) > 1e-6;
    bool resonanceChanged = sampleRateChanged || std::abs(state.formantResonance - newResonance) > 1e-6;
    bool pitchRangeChanged = sampleRateChanged || std::abs(state.pitchRangeSemitones - newPitchRange) > 1e-6;
    bool synthEnvelopeChanged = std::abs(state.synthAttack - safeSynthAttack) > 1e-6 ||
                                std::abs(state.synthDecay - safeSynthDecay) > 1e-6 ||
                                std::abs(state.synthSustain - newSynthSustain) > 1e-6 ||
                                std::abs(state.synthRelease - safeSynthRelease) > 1e-6;
    bool sampleEnvelopeChanged = std::abs(state.sampleAttack - newSampleAttack) > 1e-6 ||
                                 std::abs(state.sampleRelease - newSampleRelease) > 1e-6;
    bool delayTimeChanged = std::abs(state.delayTimeMs - newDelayTime) > 1e-6;
    bool delayFeedbackChanged = std::abs(state.delayFeedback - newDelayFeedback) > 1e-6;
    bool delayMixChanged = std::abs(state.delayMix - newDelayMix) > 1e-6;
    bool compressorEnabledChanged = state.compressorEnabled != newCompressorEnabled;
    bool compressorThresholdChanged = std::abs(state.compressorThresholdDb - newCompressorThreshold) > 1e-6;
    bool compressorRatioChanged = std::abs(state.compressorRatio - newCompressorRatio) > 1e-6;
    bool compressorAttackChanged = std::abs(state.compressorAttack - newCompressorAttack) > 1e-6;
    bool compressorReleaseChanged = std::abs(state.compressorRelease - newCompressorRelease) > 1e-6;

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
    if (formantChanged || resonanceChanged)
    {
        state.formantNormalized = newFormant;
        state.formantResonance = newResonance;
        state.formantBlend = newFormant;
        state.lastAppliedFormant = -1.0;
        state.lastAppliedResonance = -1.0;
    }
    if (sampleRateChanged || formantChanged || resonanceChanged)
    {
        configureFormantFilter(state.formantFilter, sr, state.formantNormalized, state.formantResonance);
        resetFilterState(state.formantFilter);
    }
    if (pitchRangeChanged)
    {
        state.pitchRangeSemitones = newPitchRange;
        state.pitchEnvelopeStep = computePitchEnvelopeStep(sr, newPitchRange);
    }

    bool eqEnabledChanged = state.eqEnabled != newEqEnabled;

    if (sampleRateChanged || lowChanged || midChanged || highChanged)
    {
        resetFilterState(state.lowShelf);
        resetFilterState(state.midPeak);
        resetFilterState(state.highShelf);
    }

    if (eqEnabledChanged)
    {
        resetFilterState(state.lowShelf);
        resetFilterState(state.midPeak);
        resetFilterState(state.highShelf);
    }

    state.eqEnabled = newEqEnabled;

    state.volume = newVolume;
    state.pan = newPan;
    state.feedbackAmount = newFeedback;
    state.pitchBaseOffset = newPitch;
    state.lastSampleRate = sr;
    if (synthEnvelopeChanged)
    {
        state.synthAttack = safeSynthAttack;
        state.synthDecay = safeSynthDecay;
        state.synthSustain = newSynthSustain;
        state.synthRelease = safeSynthRelease;
    }
    state.synthPhaseSync = newSynthPhaseSync;
    if (sampleEnvelopeChanged)
    {
        state.sampleAttack = std::max(newSampleAttack, kSampleEnvelopeSmoothingSeconds);
        state.sampleRelease = std::max(newSampleRelease, kSampleEnvelopeSmoothingSeconds);
    }

    if (delayTimeChanged)
        state.delayTimeMs = newDelayTime;
    if (delayFeedbackChanged)
        state.delayFeedback = newDelayFeedback;
    if (delayMixChanged)
        state.delayMix = newDelayMix;

    bool newDelayEnabled = requestedDelayEnabled;
    bool delayEnabledChanged = newDelayEnabled != state.delayEnabled;

    if (delayTimeChanged || delayFeedbackChanged || delayMixChanged)
        state.delayParametersDirty = true;
    if (delayEnabledChanged && newDelayEnabled)
        state.delayParametersDirty = true;

    if (!newDelayEnabled)
    {
        releaseDelayEffect(state);
        state.delayEnabled = false;
    }
    else
    {
        ensureDelayEffect(state, sr);
        state.delayEnabled = (state.delayEffect != nullptr);
        if (state.delayEffect)
        {
            if (state.delayParametersDirty)
            {
                state.delayEffect->setDelayTime(static_cast<float>(state.delayTimeMs));
                state.delayEffect->setFeedback(static_cast<float>(state.delayFeedback));
                state.delayEffect->setMix(static_cast<float>(state.delayMix));
                state.delayParametersDirty = false;
            }
            if (delayEnabledChanged)
            {
                state.delayEffect->reset();
            }
        }
    }

    if (compressorThresholdChanged)
        state.compressorThresholdDb = newCompressorThreshold;
    if (compressorRatioChanged)
        state.compressorRatio = newCompressorRatio;
    if (compressorAttackChanged)
        state.compressorAttack = newCompressorAttack;
    if (compressorReleaseChanged)
        state.compressorRelease = newCompressorRelease;

    auto computeSmoothingCoefficient = [&](double timeSeconds) {
        double srSafe = sr > 0.0 ? sr : 44100.0;
        double minTime = 1.0 / std::max(srSafe, 1.0);
        double clampedTime = std::max(timeSeconds, minTime);
        double coeff = std::exp(-1.0 / (clampedTime * srSafe));
        if (!std::isfinite(coeff))
            coeff = 0.0;
        return std::clamp(coeff, 0.0, 0.999999);
    };

    if (sampleRateChanged || compressorAttackChanged)
        state.compressorAttackCoeff = computeSmoothingCoefficient(state.compressorAttack);
    if (sampleRateChanged || compressorReleaseChanged)
        state.compressorReleaseCoeff = computeSmoothingCoefficient(state.compressorRelease);

    if (compressorEnabledChanged)
        state.compressorGain = 1.0;
    if (compressorThresholdChanged || compressorRatioChanged)
        state.compressorGain = 1.0;

    state.compressorEnabled = newCompressorEnabled;
    if (!state.compressorEnabled)
        state.compressorGain = 1.0;

    state.sidechain.setEnabled(newSidechainEnabled);
    state.sidechain.setSourceTrackId(newSidechainSourceTrackId);
    state.sidechain.setAmount(newSidechainAmount);
    state.sidechain.setAttack(newSidechainAttack);
    state.sidechain.setRelease(newSidechainRelease);
}

std::array<double, kModSourceCount> evaluateModulationSources(TrackPlaybackState& state,
                                                              const Track& track,
                                                              double sampleRate)
{
    auto& modulation = state.modulation;
    double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
    double twoPi = 2.0 * kPi;
    auto evaluateLfoValue = [twoPi](double phase, LfoShape shape, double deform) {
        double wrappedPhase = std::fmod(phase, twoPi);
        if (wrappedPhase < 0.0)
            wrappedPhase += twoPi;
        double t = wrappedPhase / twoPi; // 0..1
        double base = 0.0;
        switch (shape)
        {
        case LfoShape::Triangle:
            base = 2.0 * std::abs(2.0 * t - 1.0) - 1.0;
            break;
        case LfoShape::Saw:
            base = 2.0 * t - 1.0;
            break;
        case LfoShape::Square:
        {
            double duty = 0.5 + (std::clamp(deform, 0.0, 1.0) - 0.5) * 0.8;
            base = (t < duty) ? 1.0 : -1.0;
            break;
        }
        case LfoShape::Sine:
        default:
            base = std::sin(wrappedPhase);
            break;
        }

        double deformAmount = std::clamp(deform, 0.0, 1.0);
        double drive = 1.0 + deformAmount * 4.0;
        double shaped = std::tanh(base * drive);
        if (!std::isfinite(shaped))
            shaped = 0.0;
        return std::clamp(shaped, -1.0, 1.0);
    };

    for (size_t i = 0; i < modulation.lfoPhase.size(); ++i)
    {
        double rate = static_cast<double>(trackGetLfoRate(track.id, static_cast<int>(i)));
        if (!std::isfinite(rate) || rate <= 0.0)
            rate = kDefaultLfoFrequencies[i];

        double increment = twoPi * rate / sr;
        double phase = modulation.lfoPhase[i] + increment;
        if (!std::isfinite(phase))
            phase = 0.0;
        phase = std::fmod(phase, twoPi);
        if (phase < 0.0)
            phase += twoPi;
        modulation.lfoPhase[i] = phase;

        LfoShape shape = trackGetLfoShape(track.id, static_cast<int>(i));
        double deform = static_cast<double>(trackGetLfoDeform(track.id, static_cast<int>(i)));
        modulation.lfoValue[i] = evaluateLfoValue(phase, shape, deform);
    }

    double envelope = modulation.envelopeValue.load(std::memory_order_relaxed);
    if (!std::isfinite(envelope))
        envelope = 0.0;
    envelope = std::clamp(envelope, 0.0, 1.0);
    modulation.envelopeValue.store(envelope, std::memory_order_relaxed);

    std::array<double, kModSourceCount> sources{};
    for (size_t i = 0; i < kDefaultLfoFrequencies.size(); ++i)
        sources[i] = modulation.lfoValue[i];
    sources[3] = modulation.envelopeValue;
    sources[4] = modulation.macroValue[0];
    sources[5] = modulation.macroValue[1];
    return sources;
}

void updateTrackModulationState(TrackPlaybackState& state,
                                const Track& track,
                                double sampleRate,
                                const std::vector<ModMatrixAssignment>* assignments)
{
    auto& modulation = state.modulation;
    prepareModulationParameters(modulation);
    int parameterCount = static_cast<int>(modulation.parameterAmounts.size());

    auto sources = evaluateModulationSources(state, track, sampleRate);

    if (!assignments || assignments->empty())
        return;

    for (const auto& assignment : *assignments)
    {
        if (assignment.parameterIndex < 0 || assignment.parameterIndex >= parameterCount)
            continue;
        if (assignment.sourceIndex < 0 || assignment.sourceIndex >= static_cast<int>(kModSourceCount))
            continue;

        const ModParameterInfo* info = modMatrixGetParameterInfo(assignment.parameterIndex);
        if (!info)
            continue;
        if (!modMatrixParameterSupportsTrackType(*info, track.type))
            continue;

        double depth = static_cast<double>(modMatrixClampNormalized(assignment.normalizedAmount));
        double sourceValue = sources[static_cast<size_t>(assignment.sourceIndex)];
        if (!std::isfinite(sourceValue))
            sourceValue = 0.0;
        modulation.parameterAmounts[assignment.parameterIndex] += depth * sourceValue;
    }

    for (double& amount : modulation.parameterAmounts)
    {
        if (!std::isfinite(amount))
            amount = 0.0;
        amount = std::clamp(amount, -1.0, 1.0);
    }
}

double applyModulatedParameter(double base, const ModParameterInfo& info, double amount)
{
    double clampedAmount = std::clamp(amount, -1.0, 1.0);
    double range = static_cast<double>(info.maxValue - info.minValue);
    if (range <= 0.0)
        return base;

    double result = base + clampedAmount * range;
    double minValue = static_cast<double>(info.minValue);
    double maxValue = static_cast<double>(info.maxValue);
    if (!std::isfinite(result))
        result = base;
    return std::clamp(result, minValue, maxValue);
}

struct TrackModulatedParameters
{
    double volume = 0.0;
    double pan = 0.0;
    double synthPitch = 0.0;
    double synthFormant = 0.0;
    double synthResonance = 0.0;
    double synthFeedback = 0.0;
    double synthPitchRange = 0.0;
    double synthAttack = 0.0;
    double synthDecay = 0.0;
    double synthSustain = 0.0;
    double synthRelease = 0.0;
    double sampleAttack = 0.0;
    double sampleRelease = 0.0;
    double delayMix = 0.0;
    double compressorThreshold = 0.0;
    double compressorRatio = 0.0;
};

TrackModulatedParameters computeTrackModulatedParameters(const TrackPlaybackState& state,
                                                         const Track& track)
{
    const auto& lookup = getModMatrixParameterLookup();
    TrackModulatedParameters result{};

    auto getAmount = [&](int parameterIndex) {
        if (parameterIndex < 0)
            return 0.0;
        size_t idx = static_cast<size_t>(parameterIndex);
        if (idx >= state.modulation.parameterAmounts.size())
            return 0.0;
        return state.modulation.parameterAmounts[idx];
    };

    auto apply = [&](double base, int parameterIndex) {
        if (parameterIndex < 0)
            return base;
        const ModParameterInfo* info = modMatrixGetParameterInfo(parameterIndex);
        if (!info)
            return base;
        return applyModulatedParameter(base, *info, getAmount(parameterIndex));
    };

    result.volume = apply(state.volume, lookup.volume);
    result.pan = apply(state.pan, lookup.pan);
    result.synthPitch = apply(state.pitchBaseOffset, lookup.synthPitch);
    result.synthFormant = apply(state.formantNormalized, lookup.synthFormant);
    result.synthResonance = apply(state.formantResonance, lookup.synthResonance);
    result.synthFeedback = apply(state.feedbackAmount, lookup.synthFeedback);
    double basePitchRange = state.pitchRangeSemitones + 1.0;
    result.synthPitchRange = apply(basePitchRange, lookup.synthPitchRange);
    result.synthAttack = apply(state.synthAttack, lookup.synthAttack);
    result.synthDecay = apply(state.synthDecay, lookup.synthDecay);
    result.synthSustain = apply(state.synthSustain, lookup.synthSustain);
    result.synthRelease = apply(state.synthRelease, lookup.synthRelease);
    result.sampleAttack = apply(state.sampleAttack, lookup.sampleAttack);
    result.sampleRelease = apply(state.sampleRelease, lookup.sampleRelease);
    result.delayMix = apply(state.delayMix, lookup.delayMix);
    result.compressorThreshold = apply(state.compressorThresholdDb, lookup.compressorThreshold);
    result.compressorRatio = apply(state.compressorRatio, lookup.compressorRatio);

    if (track.type == TrackType::Sample && result.sampleAttack < kSampleEnvelopeSmoothingSeconds)
        result.sampleAttack = kSampleEnvelopeSmoothingSeconds;
    if (track.type == TrackType::Sample && result.sampleRelease < kSampleEnvelopeSmoothingSeconds)
        result.sampleRelease = kSampleEnvelopeSmoothingSeconds;

    return result;
}

void sendMidiNotesOffForState(TrackPlaybackState& state, int port, int channel)
{
    if (state.activeMidiNotes.empty())
        return;

    if (port < 0)
    {
        state.activeMidiNotes.clear();
        return;
    }

    channel = std::clamp(channel, 0, 15);
    for (int note : state.activeMidiNotes)
    {
        midiOutputSendNoteOff(port, channel, note, 0);
    }
    state.activeMidiNotes.clear();
}

} // namespace

void sequencerWarmupLoop()
{
    using Clock = std::chrono::steady_clock;

    Clock::time_point lastUpdate{};
    double stepSampleCounter = 0.0;
    int fallbackStep = 0;
    bool previousPlaying = false;

    while (running.load(std::memory_order_acquire))
    {
        if (audioSequencerReady.load(std::memory_order_acquire))
        {
            lastUpdate = Clock::time_point{};
            stepSampleCounter = 0.0;
            fallbackStep = sequencerCurrentStep.load(std::memory_order_relaxed);
            previousPlaying = isPlaying.load(std::memory_order_relaxed);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        auto now = Clock::now();
        bool playing = isPlaying.load(std::memory_order_relaxed);

        if (!playing)
        {
            if (previousPlaying)
            {
                requestSequencerReset();
            }
            previousPlaying = false;
            stepSampleCounter = 0.0;
            fallbackStep = 0;
            sequencerCurrentStep.store(0, std::memory_order_relaxed);
            lastUpdate = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
            continue;
        }

        if (!previousPlaying)
        {
            requestSequencerReset();
            previousPlaying = true;
            stepSampleCounter = 0.0;
            fallbackStep = sequencerCurrentStep.load(std::memory_order_relaxed);
            lastUpdate = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        if (lastUpdate.time_since_epoch().count() == 0)
        {
            lastUpdate = now;
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        double elapsedSeconds = std::chrono::duration<double>(now - lastUpdate).count();
        lastUpdate = now;

        double sampleRate = 44100.0;
        int bpm = std::clamp(sequencerBPM.load(std::memory_order_relaxed), 30, 240);
        double stepDurationSamples = sampleRate * 60.0 / (static_cast<double>(bpm) * 4.0);
        if (stepDurationSamples < 1.0)
            stepDurationSamples = 1.0;

        bool resetNow = sequencerResetRequested.exchange(false, std::memory_order_acq_rel);
        if (resetNow)
        {
            stepSampleCounter = 0.0;
            fallbackStep = 0;
            sequencerCurrentStep.store(0, std::memory_order_relaxed);
        }

        stepSampleCounter += elapsedSeconds * sampleRate;

        int activeTrackId = getActiveSequencerTrackId();
        int trackStepCount = getSequencerStepCount(activeTrackId);
        trackStepCount = std::max(trackStepCount, 1);

        if (fallbackStep >= trackStepCount)
        {
            fallbackStep %= trackStepCount;
            sequencerCurrentStep.store(fallbackStep, std::memory_order_relaxed);
        }

        bool stepAdvanced = false;
        while (stepSampleCounter >= stepDurationSamples)
        {
            stepSampleCounter -= stepDurationSamples;
            stepAdvanced = true;
            ++fallbackStep;
            if (fallbackStep >= trackStepCount)
                fallbackStep = 0;
        }

        if (stepAdvanced)
        {
            sequencerCurrentStep.store(fallbackStep, std::memory_order_relaxed);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

// REAL-TIME PATH ENTRY: audioLoop drives the WASAPI pull-model render thread. All
// work reachable from this function runs on the render thread and must avoid
// locks, waits, and allocations. Remaining legacy operations (map mutations,
// dynamic allocations for track/state changes, etc.) still need follow-up
// refactors; they are documented here to make the call graph boundary explicit
// for future passes.
void audioLoop() {
    CoInitializeEx(NULL, COINIT_MULTITHREADED);

    std::unique_ptr<AudioDeviceHandler> deviceHandler;
    UINT32 bufferFrameCount = 0;
    const WAVEFORMATEX* format = nullptr;
    double sampleRate = 44100.0;
    double transportSamplePosition = 0.0;
    const double twoPi = 6.283185307179586;
    double stepSampleCounter = 0.0;
    bool previousPlaying = false;
    std::unordered_map<int, TrackPlaybackState> playbackStates;
    bool deviceReady = false;
    bool samplerResetPending = true;
#ifdef DEBUG_AUDIO
    std::chrono::steady_clock::time_point lastCallbackTime{};
#endif

    struct ModulationRequest
    {
        const std::vector<Track>* tracks = nullptr;
        const std::vector<std::pair<int, std::vector<ModMatrixAssignment>>>* assignmentsByTrack = nullptr;
        std::unordered_map<int, TrackPlaybackState>* playbackStates = nullptr;
        double sampleRate = 44100.0;
        uint64_t id = 0;
    };

    auto applyVstResetRequests = [&]() {
        int published = gPublishedVstBatch.exchange(-1, std::memory_order_acq_rel);
        if (published < 0)
            return;

        auto& batch = gVstResetBatches[published];
        for (std::size_t i = 0; i < batch.count; ++i)
        {
            int trackId = batch.ids[i];
            auto stateIt = playbackStates.find(trackId);
            if (stateIt != playbackStates.end())
            {
                auto& state = stateIt->second;
                state.vstPrepared = false;
                state.vstPreparedSampleRate = 0.0;
                state.vstPreparedBlockSize = 0;
                state.vstPrepareErrorNotified = false;
            }
        }

        gVstResetBatches[published].count = 0;
    };

    struct ModulationResult
    {
        std::vector<TrackModulatedParameters> parameters;
        uint64_t id = 0;
    };

    class ModulationWorker
    {
    public:
        ModulationWorker()
        {
            m_worker = std::thread([this]() { workerLoop(); });
        }

        ~ModulationWorker()
        {
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_stop = true;
            }
            m_cv.notify_all();
            if (m_worker.joinable())
                m_worker.join();
        }

        void submit(const std::vector<Track>& tracks,
                    const std::vector<std::pair<int, std::vector<ModMatrixAssignment>>>& assignmentsByTrack,
                    std::unordered_map<int, TrackPlaybackState>& playbackStates,
                    double sampleRate)
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_request.tracks = &tracks;
            m_request.assignmentsByTrack = &assignmentsByTrack;
            m_request.playbackStates = &playbackStates;
            m_request.sampleRate = sampleRate;
            m_request.id = ++m_requestCounter;
            m_hasRequest = true;
            m_cv.notify_one();
        }

        const std::vector<TrackModulatedParameters>* consumeLatest(uint64_t& requestIdOut)
        {
            int index = m_latestIndex.load(std::memory_order_acquire);
            if (index < 0)
                return nullptr;
            requestIdOut = m_results[index].id;
            return &m_results[index].parameters;
        }

    private:
        void workerLoop()
        {
            while (true)
            {
                ModulationRequest localRequest{};
                {
                    std::unique_lock<std::mutex> lock(m_mutex);
                    m_cv.wait(lock, [this]() { return m_hasRequest || m_stop; });
                    if (m_stop)
                        return;
                    localRequest = m_request;
                    m_hasRequest = false;
                }

                processRequest(localRequest);
            }
        }

        void processRequest(const ModulationRequest& request)
        {
            if (!request.tracks || !request.playbackStates)
                return;

            int nextIndex = (m_latestIndex.load(std::memory_order_relaxed) + 1) % 2;
            auto& result = m_results[nextIndex];
            result.parameters.resize(request.tracks->size());

            JobGroup group;
            group.remaining.store(static_cast<int>(request.tracks->size()), std::memory_order_relaxed);
            auto& pool = getTrackProcessingPool();
            static const std::vector<ModMatrixAssignment> kEmptyAssignments;

            for (size_t trackIndex = 0; trackIndex < request.tracks->size(); ++trackIndex)
            {
                auto job = [&, trackIndex]() {
                    const auto& trackInfo = (*request.tracks)[trackIndex];
                    auto stateIt = request.playbackStates->find(trackInfo.id);
                    if (stateIt == request.playbackStates->end())
                    {
                        result.parameters[trackIndex] = TrackModulatedParameters{};
                        notifyFinished(group);
                        return;
                    }

                    auto& state = stateIt->second;
                    prepareModulationParameters(state.modulation);

                    const std::vector<ModMatrixAssignment>* assignmentList = &kEmptyAssignments;
                    if (trackIndex < request.assignmentsByTrack->size())
                    {
                        const auto& assignmentEntry = (*request.assignmentsByTrack)[trackIndex];
                        if (assignmentEntry.first == trackInfo.id)
                            assignmentList = &assignmentEntry.second;
                    }

                    if (assignmentList && !assignmentList->empty())
                        updateTrackModulationState(state, trackInfo, request.sampleRate, assignmentList);

                    result.parameters[trackIndex] = computeTrackModulatedParameters(state, trackInfo);
                    notifyFinished(group);
                };

                if (!pool.enqueue(job))
                    job();
            }

            waitUntilFinished(group);
            result.id = request.id;
            m_latestIndex.store(nextIndex, std::memory_order_release);
        }

        std::mutex m_mutex;
        std::condition_variable m_cv;
        std::thread m_worker;
        ModulationRequest m_request{};
        bool m_hasRequest = false;
        bool m_stop = false;
        std::atomic<int> m_latestIndex{-1};
        std::atomic<uint64_t> m_requestCounter{0};
        std::array<ModulationResult, 2> m_results{};
    };

    struct TrackFrameResult
    {
        double left = 0.0;
        double right = 0.0;
        double detection = 0.0;
        bool activeTrackHasSteps = false;
        int activeTrackStep = 0;
    };

    constexpr size_t kCachedTrackCapacity = 64;
    constexpr size_t kCachedAssignmentCapacity = 32;
    constexpr size_t kCachedStepCapacity = kMaxSequencerSteps;
    constexpr size_t kCachedNotesPerStep = 8;

    struct TrackDataSnapshot
    {
        std::vector<Track> tracks;
        std::vector<int> trackStepCounts;
        std::vector<std::pair<int, std::vector<ModMatrixAssignment>>> assignmentsByTrack;
        std::vector<std::vector<bool>> stepStatesByTrack;
        std::vector<std::vector<std::vector<StepNoteInfo>>> stepNotesByTrack;
        std::vector<std::vector<float>> stepVelocityByTrack;
        std::vector<std::vector<float>> stepPanByTrack;
        std::vector<std::vector<float>> stepPitchByTrack;

        void reserve()
        {
            tracks.reserve(kCachedTrackCapacity);
            trackStepCounts.reserve(kCachedTrackCapacity);
            assignmentsByTrack.reserve(kCachedTrackCapacity);
            stepStatesByTrack.reserve(kCachedTrackCapacity);
            stepNotesByTrack.reserve(kCachedTrackCapacity);
            stepVelocityByTrack.reserve(kCachedTrackCapacity);
            stepPanByTrack.reserve(kCachedTrackCapacity);
            stepPitchByTrack.reserve(kCachedTrackCapacity);
            for (auto& entry : assignmentsByTrack)
                entry.second.reserve(kCachedAssignmentCapacity);
        }

        void prepareForTracks(size_t trackCount)
        {
            if (tracks.capacity() < kCachedTrackCapacity)
                tracks.reserve(kCachedTrackCapacity);
            if (trackStepCounts.capacity() < kCachedTrackCapacity)
                trackStepCounts.reserve(kCachedTrackCapacity);
            if (assignmentsByTrack.capacity() < kCachedTrackCapacity)
                assignmentsByTrack.reserve(kCachedTrackCapacity);
            if (stepStatesByTrack.capacity() < kCachedTrackCapacity)
                stepStatesByTrack.reserve(kCachedTrackCapacity);
            if (stepNotesByTrack.capacity() < kCachedTrackCapacity)
                stepNotesByTrack.reserve(kCachedTrackCapacity);
            if (stepVelocityByTrack.capacity() < kCachedTrackCapacity)
                stepVelocityByTrack.reserve(kCachedTrackCapacity);
            if (stepPanByTrack.capacity() < kCachedTrackCapacity)
                stepPanByTrack.reserve(kCachedTrackCapacity);
            if (stepPitchByTrack.capacity() < kCachedTrackCapacity)
                stepPitchByTrack.reserve(kCachedTrackCapacity);

            trackStepCounts.assign(trackCount, 0);
            assignmentsByTrack.resize(trackCount);
            stepStatesByTrack.resize(trackCount);
            stepNotesByTrack.resize(trackCount);
            stepVelocityByTrack.resize(trackCount);
            stepPanByTrack.resize(trackCount);
            stepPitchByTrack.resize(trackCount);
            for (auto& entry : assignmentsByTrack)
            {
                entry.second.clear();
                if (entry.second.capacity() < kCachedAssignmentCapacity)
                    entry.second.reserve(kCachedAssignmentCapacity);
            }
            for (auto& stepStates : stepStatesByTrack)
            {
                stepStates.assign(kCachedStepCapacity, false);
            }
            for (auto& stepNotes : stepNotesByTrack)
            {
                stepNotes.clear();
                stepNotes.resize(kCachedStepCapacity);
                for (auto& notes : stepNotes)
                {
                    notes.clear();
                    if (notes.capacity() < kCachedNotesPerStep)
                        notes.reserve(kCachedNotesPerStep);
                }
            }
            for (auto& stepVelocities : stepVelocityByTrack)
            {
                stepVelocities.assign(kCachedStepCapacity, kTrackStepVelocityMax);
            }
            for (auto& stepPans : stepPanByTrack)
            {
                stepPans.assign(kCachedStepCapacity, 0.0f);
            }
            for (auto& stepPitches : stepPitchByTrack)
            {
                stepPitches.assign(kCachedStepCapacity, 0.0f);
            }
        }
    };

    TrackDataSnapshot trackSnapshotA;
    TrackDataSnapshot trackSnapshotB;
    trackSnapshotA.reserve();
    trackSnapshotB.reserve();
    std::atomic<TrackDataSnapshot*> activeTrackSnapshot{ &trackSnapshotA };
    std::atomic<bool> cacheThreadRunning{ true };
    ModulationWorker modulationWorker;
    std::vector<TrackModulatedParameters> fallbackModulationParameters;

    auto populateTrackSnapshot = [&](TrackDataSnapshot& snapshot)
    {
        auto tracks = getTracks();
        snapshot.prepareForTracks(tracks.size());
        snapshot.tracks = std::move(tracks);

        for (size_t i = 0; i < snapshot.tracks.size(); ++i)
        {
            snapshot.trackStepCounts[i] = getSequencerStepCount(snapshot.tracks[i].id);
            snapshot.assignmentsByTrack[i].first = snapshot.tracks[i].id;
            int stepCount = std::clamp(snapshot.trackStepCounts[i], 0, static_cast<int>(kCachedStepCapacity));
            snapshot.stepStatesByTrack[i].assign(stepCount, false);
            snapshot.stepNotesByTrack[i].resize(stepCount);
            snapshot.stepVelocityByTrack[i].assign(stepCount, kTrackStepVelocityMax);
            snapshot.stepPanByTrack[i].assign(stepCount, 0.0f);
            snapshot.stepPitchByTrack[i].assign(stepCount, 0.0f);
            for (auto& notes : snapshot.stepNotesByTrack[i])
            {
                notes.clear();
                if (notes.capacity() < kCachedNotesPerStep)
                    notes.reserve(kCachedNotesPerStep);
            }
        }

        auto assignments = modMatrixGetAssignments();
        for (const auto& assignment : assignments)
        {
            if (assignment.trackId <= 0)
                continue;
            auto it = std::find_if(snapshot.assignmentsByTrack.begin(), snapshot.assignmentsByTrack.end(),
                                   [&](const auto& entry) { return entry.first == assignment.trackId; });
            if (it != snapshot.assignmentsByTrack.end())
            {
                it->second.push_back(assignment);
            }
        }

        for (size_t i = 0; i < snapshot.tracks.size(); ++i)
        {
            int trackId = snapshot.tracks[i].id;
            int stepCount = snapshot.trackStepCounts[i];
            for (int step = 0; step < stepCount && step < static_cast<int>(kCachedStepCapacity); ++step)
            {
                snapshot.stepStatesByTrack[i][step] = trackGetStepState(trackId, step);
                snapshot.stepVelocityByTrack[i][step] = trackGetStepVelocity(trackId, step);
                snapshot.stepPanByTrack[i][step] = trackGetStepPan(trackId, step);
                snapshot.stepPitchByTrack[i][step] = trackGetStepPitchOffset(trackId, step);

                auto notes = trackGetStepNoteInfo(trackId, step);
                if (notes.empty())
                {
                    int fallback = trackGetStepNote(trackId, step);
                    if (fallback >= 0)
                    {
                        StepNoteInfo info{};
                        info.midiNote = fallback;
                        info.velocity = trackGetStepNoteVelocity(trackId, step, fallback);
                        info.sustain = trackGetStepNoteSustain(trackId, step, fallback);
                        notes.push_back(info);
                    }
                }
                snapshot.stepNotesByTrack[i][step] = std::move(notes);
            }
        }
    };

    populateTrackSnapshot(trackSnapshotA);
    std::thread cacheUpdater([&]()
    {
        while (cacheThreadRunning.load(std::memory_order_acquire) && running.load(std::memory_order_acquire))
        {
            TrackDataSnapshot* current = activeTrackSnapshot.load(std::memory_order_acquire);
            TrackDataSnapshot* staging = (current == &trackSnapshotA) ? &trackSnapshotB : &trackSnapshotA;
            populateTrackSnapshot(*staging);
            activeTrackSnapshot.store(staging, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    while (running.load(std::memory_order_acquire)) {
        bool changeRequested = deviceChangeRequested.exchange(false);
        std::wstring desiredDeviceId = gRequestedDeviceIds[gRequestedDeviceIndex.load(std::memory_order_acquire)];

        if (changeRequested) {
            audioSequencerReady.store(false, std::memory_order_release);
            if (deviceHandler) {
                deviceHandler->stop();
                deviceHandler->shutdown();
                deviceHandler.reset();
            }
            deviceReady = false;
        }

        if (!deviceHandler) {
            deviceHandler = std::make_unique<AudioDeviceHandler>();
            deviceHandler->registerStreamCallback(&audioDriverProbeCallback, deviceHandler.get());
        }

        if (!deviceReady) {
            if (deviceHandler->isInitializing()) {
                audioSequencerReady.store(false, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }

            bool usedFallback = false;
            bool initialized = deviceHandler->initialize(desiredDeviceId);
            if (!initialized) {
                if (deviceHandler->isInitializing()) {
                    audioSequencerReady.store(false, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (!desiredDeviceId.empty()) {
                    initialized = deviceHandler->initialize();
                    if (!initialized) {
                        if (deviceHandler->isInitializing()) {
                            audioSequencerReady.store(false, std::memory_order_release);
                            std::this_thread::sleep_for(std::chrono::milliseconds(10));
                            continue;
                        }
                    } else {
                        usedFallback = true;
                    }
                }
            }

            if (!initialized) {
                if (deviceHandler->isInitializing()) {
                    audioSequencerReady.store(false, std::memory_order_release);
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                    continue;
                }
                if (!usedFallback) {
                    auto snapshot = getDeviceSnapshot();
                    snapshot.activeId.clear();
                    snapshot.activeName.clear();
                    publishDeviceSnapshot(snapshot);
                }
                deviceHandler->shutdown();
                deviceHandler.reset();
                audioSequencerReady.store(false, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            AudioDeviceHandler::resetCallbackMonitor();

            if (!deviceHandler->start()) {
                deviceHandler->shutdown();
                deviceHandler.reset();
                audioSequencerReady.store(false, std::memory_order_release);
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }

            bufferFrameCount = deviceHandler->bufferFrameCount();
            format = deviceHandler->format();
            sampleRate = format ? static_cast<double>(format->nSamplesPerSec) : 44100.0;
            deviceReady = true;
            stepSampleCounter = 0.0;
            previousPlaying = false;
            for (auto& entry : playbackStates) {
                auto& state = entry.second;
                resetSynthPlaybackState(state);
                resetSamplePlaybackState(state);
                releaseDelayEffect(state);
            }
            playbackStates.clear();
            samplerResetPending = true;
#ifdef DEBUG_AUDIO
            lastCallbackTime = std::chrono::steady_clock::now();
#endif

#ifdef DEBUG_AUDIO
            std::wstring logDeviceName = deviceHandler->deviceName();
            std::cout << "[AudioEngine] Initialized device sampleRate="
                      << sampleRate
                      << " Hz blockSize=" << bufferFrameCount
                      << " name=" << narrowFromWide(logDeviceName) << std::endl;
#endif

            AudioDeviceSnapshot snapshot = getDeviceSnapshot();
            snapshot.activeId = deviceHandler->deviceId();
            snapshot.activeName = deviceHandler->deviceName();
            if (usedFallback) {
                snapshot.requestedId.clear();
                int nextRequested = gRequestedDeviceIndex.load(std::memory_order_relaxed) ^ 1;
                gRequestedDeviceIds[nextRequested].clear();
                gRequestedDeviceIndex.store(nextRequested, std::memory_order_release);
            } else {
                snapshot.requestedId = desiredDeviceId;
                int nextRequested = gRequestedDeviceIndex.load(std::memory_order_relaxed) ^ 1;
                gRequestedDeviceIds[nextRequested] = desiredDeviceId;
                gRequestedDeviceIndex.store(nextRequested, std::memory_order_release);
            }
            publishDeviceSnapshot(snapshot);
            audioSequencerReady.store(true, std::memory_order_release);
        }

        if (!deviceReady || bufferFrameCount == 0) {
            audioSequencerReady.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        if (!deviceHandler) {
            deviceReady = false;
            audioSequencerReady.store(false, std::memory_order_release);
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            continue;
        }

        UINT32 padding = 0;
        HRESULT paddingResult = deviceHandler->currentPadding(&padding);
        if (paddingResult == AUDCLNT_E_DEVICE_INVALIDATED) {
            deviceHandler->stop();
            deviceHandler->shutdown();
            auto snapshot = getDeviceSnapshot();
            snapshot.activeId.clear();
            snapshot.activeName.clear();
            publishDeviceSnapshot(snapshot);
            bufferFrameCount = 0;
            format = nullptr;
            deviceReady = false;
            audioSequencerReady.store(false, std::memory_order_release);
            continue;
        } else if (FAILED(paddingResult)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        UINT32 available = bufferFrameCount > padding ? bufferFrameCount - padding : 0;
        if (available > 0) {
            BYTE* data;
            HRESULT bufferResult = deviceHandler->getBuffer(available, &data);
            if (bufferResult == AUDCLNT_E_DEVICE_INVALIDATED) {
                deviceHandler->stop();
                deviceHandler->shutdown();
                auto snapshot = getDeviceSnapshot();
                snapshot.activeId.clear();
                snapshot.activeName.clear();
                publishDeviceSnapshot(snapshot);
                bufferFrameCount = 0;
                format = nullptr;
                deviceReady = false;
                audioSequencerReady.store(false, std::memory_order_release);
                continue;
            } else if (FAILED(bufferResult)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (auto callback = deviceHandler->streamCallback()) {
                void* context = deviceHandler->streamCallbackContext();
                callback(data, available, format, context);
            }
            BYTE* rawData = data;
            bool bufferIsFloat = isFloatWaveFormat(format);
            bool bufferIsPcm16 = isPcm16WaveFormat(format);
            UINT32 channelCount = format && format->nChannels > 0 ? format->nChannels : 2;
            if (channelCount == 0)
                channelCount = 2;
            UINT32 strideBytes = bytesPerFrame(format);
            if (strideBytes == 0) {
                UINT32 sampleSize = bufferIsFloat ? sizeof(float) : sizeof(std::int16_t);
                strideBytes = channelCount * sampleSize;
            }
            float* floatSamples = bufferIsFloat ? reinterpret_cast<float*>(rawData) : nullptr;
            std::int16_t* intSamples = bufferIsPcm16 ? reinterpret_cast<std::int16_t*>(rawData) : nullptr;
            auto toInt16 = [](double value) -> std::int16_t {
                double clamped = std::clamp(value, -1.0, 1.0);
                return static_cast<std::int16_t>(std::lround(clamped * 32767.0));
            };
            auto writeFrame = [&](UINT32 frameIndex, double left, double right) {
                if (bufferIsFloat && floatSamples) {
                    UINT32 base = frameIndex * channelCount;
                    float leftFloat = static_cast<float>(left);
                    float rightFloat = static_cast<float>(right);
                    if (channelCount == 1) {
                        floatSamples[base] = (leftFloat + rightFloat) * 0.5f;
                    } else {
                        floatSamples[base] = leftFloat;
                        floatSamples[base + 1] = rightFloat;
                        for (UINT32 ch = 2; ch < channelCount; ++ch) {
                            floatSamples[base + ch] = 0.0f;
                        }
                    }
                } else if (bufferIsPcm16 && intSamples) {
                    UINT32 base = frameIndex * channelCount;
                    if (channelCount == 1) {
                        intSamples[base] = toInt16((left + right) * 0.5);
                    } else {
                        intSamples[base] = toInt16(left);
                        intSamples[base + 1] = toInt16(right);
                        for (UINT32 ch = 2; ch < channelCount; ++ch) {
                            intSamples[base + ch] = 0;
                        }
                    }
                } else if (rawData && strideBytes > 0) {
                    std::memset(rawData + static_cast<std::size_t>(frameIndex) * strideBytes, 0, strideBytes);
                }
            };
#ifdef DEBUG_AUDIO
            double mixSumAbs = 0.0;
            double mixPeak = 0.0;
#endif
            int bpm = std::clamp(sequencerBPM.load(std::memory_order_relaxed), 30, 240);
            double stepDurationSamples = sampleRate * 60.0 / (static_cast<double>(bpm) * 4.0);
            if (stepDurationSamples < 1.0) stepDurationSamples = 1.0;

            TrackDataSnapshot* trackSnapshot = activeTrackSnapshot.load(std::memory_order_acquire);
            const auto& trackInfos = trackSnapshot ? trackSnapshot->tracks : trackSnapshotA.tracks;
            const auto& trackStepCounts = trackSnapshot ? trackSnapshot->trackStepCounts : trackSnapshotA.trackStepCounts;
            const auto& assignmentsByTrack = trackSnapshot ? trackSnapshot->assignmentsByTrack : trackSnapshotA.assignmentsByTrack;
            const auto& stepStatesByTrack = trackSnapshot ? trackSnapshot->stepStatesByTrack : trackSnapshotA.stepStatesByTrack;
            const auto& stepNotesByTrack = trackSnapshot ? trackSnapshot->stepNotesByTrack : trackSnapshotA.stepNotesByTrack;
            const auto& stepVelocityByTrack = trackSnapshot ? trackSnapshot->stepVelocityByTrack : trackSnapshotA.stepVelocityByTrack;
            const auto& stepPanByTrack = trackSnapshot ? trackSnapshot->stepPanByTrack : trackSnapshotA.stepPanByTrack;
            const auto& stepPitchByTrack = trackSnapshot ? trackSnapshot->stepPitchByTrack : trackSnapshotA.stepPitchByTrack;

            uint64_t modulationRequestId = 0;
            const auto* modulatedParameters = modulationWorker.consumeLatest(modulationRequestId);
            (void)modulationRequestId;
            modulationWorker.submit(trackInfos, assignmentsByTrack, playbackStates, sampleRate);
            if (!modulatedParameters || modulatedParameters->size() != trackInfos.size())
            {
                fallbackModulationParameters.resize(trackInfos.size());
                for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex)
                {
                    const auto& trackInfo = trackInfos[trackIndex];
                    auto stateIt = playbackStates.find(trackInfo.id);
                    if (stateIt != playbackStates.end())
                        fallbackModulationParameters[trackIndex] = computeTrackModulatedParameters(stateIt->second, trackInfo);
                    else
                        fallbackModulationParameters[trackIndex] = TrackModulatedParameters{};
                }
                modulatedParameters = &fallbackModulationParameters;
            }

            int activeTrackId = getActiveSequencerTrackId();

            for (auto it = playbackStates.begin(); it != playbackStates.end(); ) {
                int trackId = it->first;
                bool exists = std::any_of(trackInfos.begin(), trackInfos.end(), [trackId](const Track& track) {
                    return track.id == trackId;
                });
                if (!exists) {
                    if (it->second.type == TrackType::MidiOut)
                        sendMidiNotesOffForState(it->second, it->second.midiPort, it->second.midiChannel);
                    releaseDelayEffect(it->second);
                    it = playbackStates.erase(it);
                } else {
                    ++it;
                }
            }

            for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                const auto& trackInfo = trackInfos[trackIndex];
                int trackStepCount = trackIndex < trackStepCounts.size() ? trackStepCounts[trackIndex] : 0;
                auto insertResult = playbackStates.try_emplace(trackInfo.id);
                auto& state = insertResult.first->second;
                bool inserted = insertResult.second;
                TrackType previousType = state.type;
                bool typeChanged = inserted || previousType != trackInfo.type;
                state.type = trackInfo.type;

                int previousMidiChannel = state.midiChannel;
                int previousMidiPort = state.midiPort;
                int desiredMidiChannel = std::clamp(trackInfo.midiChannel, 1, 16) - 1;
                int desiredMidiPort = trackInfo.midiPort;
                bool midiSettingsChanged = (previousMidiChannel != desiredMidiChannel) ||
                                            (previousMidiPort != desiredMidiPort);

                if (previousType == TrackType::MidiOut &&
                    (trackInfo.type != TrackType::MidiOut || midiSettingsChanged))
                {
                    sendMidiNotesOffForState(state, previousMidiPort, previousMidiChannel);
                }

                state.midiChannel = desiredMidiChannel;
                state.midiPort = desiredMidiPort;
                if ((trackInfo.type != TrackType::MidiOut && trackInfo.type != TrackType::VST) || midiSettingsChanged)
                {
                    state.activeMidiNotes.clear();
                }

                if (inserted) {
                    int globalStep = sequencerCurrentStep.load(std::memory_order_relaxed);
                    if (globalStep < 0) {
                        globalStep = 0;
                    }
                    if (trackStepCount > 0) {
                        state.currentStep = globalStep % trackStepCount;
                    } else {
                        state.currentStep = 0;
                    }

                    resetSamplePlaybackState(state);
                    resetSynthPlaybackState(state);
                    state.pitchEnvelope = 0.0;
                    state.currentMidiNote = 69;
                    state.currentFrequency = midiNoteToFrequency(69);
                    state.lastParameterStep = -1;
                    state.stepVelocity = 1.0;
                    state.stepPan = 0.0;
                    state.stepPitchOffset = 0.0;
                    state.resetScheduled = false;
                    state.resetFadeGain = 1.0;
                    state.resetFadeStep = 0.0;
                    state.resetFadeSamples = 0;
                    state.resetReason = SequencerResetReason::Manual;
                    state.sidechain.reset();
                }

                if (trackInfo.type == TrackType::Sample) {
                    state.vstPrepared = false;
                    state.vstPreparedSampleRate = 0.0;
                    state.vstPreparedBlockSize = 0;
                    state.vstPrepareErrorNotified = false;
                    auto sampleBuffer = trackGetSampleBuffer(trackInfo.id);
                    bool sampleBufferChanged = sampleBuffer != state.sampleBuffer;
                    state.sampleBuffer = std::move(sampleBuffer);
                    state.sampleFrameCount = state.sampleBuffer ? state.sampleBuffer->frameCount() : 0;
                    if (state.sampleBuffer && state.sampleBuffer->sampleRate > 0) {
                        state.sampleIncrement = static_cast<double>(state.sampleBuffer->sampleRate) / sampleRate;
                    } else {
                        state.sampleIncrement = 1.0;
                    }

                    if (sampleBufferChanged || samplerResetPending || typeChanged) {
                        resetSamplePlaybackState(state);
                        resetSynthPlaybackState(state);
                        state.currentMidiNote = 69;
                        state.currentFrequency = midiNoteToFrequency(69);
                        state.lastParameterStep = -1;
                        state.stepVelocity = 1.0;
                        state.stepPan = 0.0;
                        state.stepPitchOffset = 0.0;
                    }
                } else if (trackInfo.type == TrackType::VST) {
                    if (state.sampleBuffer) {
                        state.sampleBuffer.reset();
                        state.sampleFrameCount = 0;
                    }
                    if (typeChanged || samplerResetPending) {
                        resetSamplePlaybackState(state);
                        resetSynthPlaybackState(state);
                        state.currentMidiNote = 69;
                        state.currentFrequency = midiNoteToFrequency(69);
                        state.lastParameterStep = -1;
                        state.stepVelocity = 1.0;
                        state.stepPan = 0.0;
                        state.stepPitchOffset = 0.0;
                    }

                    auto host = trackInfo.vstHost;
                    if (host) {
                        bool needsPrepare = typeChanged || samplerResetPending || !state.vstPrepared ||
                                            std::abs(state.vstPreparedSampleRate - sampleRate) > 1e-6 ||
                                            state.vstPreparedBlockSize != static_cast<int>(bufferFrameCount);

                        if (needsPrepare) {
                            bool prepared = host->prepare(sampleRate, static_cast<int>(bufferFrameCount));
                            state.vstPrepared = prepared;

                            if (prepared) {
                                state.vstPreparedSampleRate = sampleRate;
                                state.vstPreparedBlockSize = static_cast<int>(bufferFrameCount);
                                state.vstPrepareErrorNotified = false;
                            } else {
                                state.vstPreparedSampleRate = 0.0;
                                state.vstPreparedBlockSize = 0;

                                if (!state.vstPrepareErrorNotified) {
                                    std::wstring trackName(trackInfo.name.begin(), trackInfo.name.end());
                                    if (trackName.empty())
                                        trackName = L"Unnamed";

                                    std::wstring message = L"Failed to prepare VST plug-in for track '" + trackName +
                                                           L"'.\nTry reselecting the audio device or reloading the plug-in.";
                                    enqueueAudioThreadNotification(L"VST Prepare Failed", message);
                                    state.vstPrepareErrorNotified = true;
                                }
                            }
                        }
                    } else {
                        state.vstPrepared = false;
                        state.vstPreparedSampleRate = 0.0;
                        state.vstPreparedBlockSize = 0;
                        state.vstPrepareErrorNotified = false;
                    }
                } else if (trackInfo.type == TrackType::MidiOut) {
                    state.vstPrepared = false;
                    state.vstPreparedSampleRate = 0.0;
                    state.vstPreparedBlockSize = 0;
                    state.vstPrepareErrorNotified = false;
                    if (state.sampleBuffer) {
                        state.sampleBuffer.reset();
                        state.sampleFrameCount = 0;
                    }
                    if (typeChanged || samplerResetPending) {
                        resetSamplePlaybackState(state);
                        resetSynthPlaybackState(state);
                        state.currentMidiNote = 69;
                        state.currentFrequency = midiNoteToFrequency(69);
                        state.lastParameterStep = -1;
                        state.stepVelocity = 1.0;
                        state.stepPan = 0.0;
                        state.stepPitchOffset = 0.0;
                    }
                } else {
                    state.vstPrepared = false;
                    state.vstPreparedSampleRate = 0.0;
                    state.vstPreparedBlockSize = 0;
                    state.vstPrepareErrorNotified = false;
                    if (state.sampleBuffer) {
                        state.sampleBuffer.reset();
                        state.sampleFrameCount = 0;
                    }
                    if (typeChanged || samplerResetPending) {
                        resetSamplePlaybackState(state);
                    }
                    if (state.currentMidiNote < 0 || state.currentMidiNote > 127) {
                        state.currentMidiNote = 69;
                    }
                    if (typeChanged || samplerResetPending) {
                        resetSynthPlaybackState(state);
                        state.lastParameterStep = -1;
                        state.stepVelocity = 1.0;
                        state.stepPan = 0.0;
                        state.stepPitchOffset = 0.0;
                    }
                    state.currentFrequency = midiNoteToFrequency(state.currentMidiNote);
                }

                updateMixerState(state, trackInfo, sampleRate);
            }
            if (samplerResetPending) {
                samplerResetPending = false;
            }

            applyVstResetRequests();
            bool playingNow = isPlaying.load(std::memory_order_relaxed);
            if (vstOperationsPending.load(std::memory_order_acquire) > 0)
            {
                for (UINT32 i = 0; i < available; ++i)
                {
                    writeFrame(i, 0.0, 0.0);
                    if (playingNow)
                        transportSamplePosition += 1.0;
                }

                previousPlaying = playingNow;

                deviceHandler->releaseBuffer(available);
                continue;
            }

            static thread_local std::vector<float> capturedSamples;
            static thread_local std::size_t capturedCapacity = 0;
            std::size_t requiredCapacity = static_cast<std::size_t>(bufferFrameCount);
            if (requiredCapacity > capturedCapacity)
            {
                capturedCapacity = requiredCapacity;
                capturedSamples.assign(capturedCapacity, 0.0f);
            }

            std::size_t capturedCount = 0;

            for (UINT32 i = 0; i < available; i++) {
                bool playing = isPlaying.load(std::memory_order_relaxed);
                bool stepAdvanced = false;

                if (!playing) {
                    if (previousPlaying) {
                        requestSequencerReset();
                    }
                    previousPlaying = false;
                    stepSampleCounter = 0.0;
                    transportSamplePosition = 0.0;
                    for (auto& entry : playbackStates) {
                        auto& state = entry.second;
                        for (auto& voice : state.voices) {
                            voice.envelope = 0.0;
                            voice.envelopeStage = EnvelopeStage::Idle;
                        }
                        resetSamplePlaybackState(state);
                        state.voices.clear();
                        state.pitchEnvelope = 0.0;
                        state.lastParameterStep = -1;
                        state.stepVelocity = 1.0;
                        state.stepPan = 0.0;
                        state.stepPitchOffset = 0.0;
                        state.sidechain.reset();
                        if (state.type == TrackType::MidiOut)
                            sendMidiNotesOffForState(state, state.midiPort, state.midiChannel);
                    }
                } else {
                    if (!previousPlaying) {
                        requestSequencerReset();
                    }
                    previousPlaying = true;

                        if (sequencerResetRequested.exchange(false, std::memory_order_acq_rel)) {
                            SequencerResetReason reason = sequencerResetReason.load(std::memory_order_relaxed);
                            sequencerCurrentStep.store(0, std::memory_order_relaxed);
                            stepSampleCounter = 0.0;

                            if (reason == SequencerResetReason::TrackSelection) {
                                double fadeSeconds = 0.004; // ~4ms
                                double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
                                int fadeSamples = static_cast<int>(std::max(1.0, std::round(fadeSeconds * sr)));
                                double fadeStep = 1.0 / static_cast<double>(fadeSamples);

                                for (auto& entry : playbackStates) {
                                    auto& state = entry.second;
                                    state.resetScheduled = true;
                                    state.resetFadeGain = 1.0;
                                    state.resetFadeSamples = fadeSamples;
                                    state.resetFadeStep = fadeStep;
                                    state.resetReason = reason;
                                }
                            } else {
                                for (auto& entry : playbackStates) {
                                    auto& state = entry.second;
                                    state.resetScheduled = false;
                                    state.resetFadeGain = 1.0;
                                    state.resetFadeSamples = 0;
                                    state.resetFadeStep = 0.0;
                                    state.resetReason = reason;
                                    resetSamplePlaybackState(state);
                                    resetSynthPlaybackState(state);
                                    state.currentStep = 0;
                                    state.lastParameterStep = -1;
                                    state.stepVelocity = 1.0;
                                    state.stepPan = 0.0;
                                    state.stepPitchOffset = 0.0;
                                    state.sidechain.reset();
                                }
                            }
                        }

                    stepSampleCounter += 1.0;
                    if (stepSampleCounter >= stepDurationSamples) {
                        stepSampleCounter -= stepDurationSamples;
                        stepAdvanced = true;
                    }

                    double leftValue = 0.0;
                    double rightValue = 0.0;

                    if (stepAdvanced) {
                        for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                            const auto& trackInfo = trackInfos[trackIndex];
                            auto stateIt = playbackStates.find(trackInfo.id);
                            if (stateIt == playbackStates.end())
                                continue;
                            auto& state = stateIt->second;
                            int trackStepCount = trackStepCounts[trackIndex];
                            if (trackStepCount <= 0) {
                                if (trackInfo.type == TrackType::MidiOut)
                                    sendMidiNotesOffForState(state, state.midiPort, state.midiChannel);
                                state.currentStep = 0;
                                continue;
                            }
                            if (state.currentStep < 0 || state.currentStep >= trackStepCount) {
                                state.currentStep = 0;
                            }
                            int nextStep = state.currentStep + 1;
                            if (nextStep >= trackStepCount)
                                nextStep = 0;
                            state.currentStep = nextStep;
                        }
                    }

                    int activeTrackStep = 0;
                    bool activeTrackHasSteps = false;

                    for (size_t trackIndex = 0; trackIndex < trackInfos.size(); ++trackIndex) {
                        const auto& trackInfo = trackInfos[trackIndex];
                        int trackStepCount = trackStepCounts[trackIndex];
                        auto stateIt = playbackStates.find(trackInfo.id);
                        if (stateIt == playbackStates.end())
                            continue;
                        auto& state = stateIt->second;

                        if (trackStepCount <= 0) {
                            state.currentStep = 0;
                        } else if (state.currentStep < 0 || state.currentStep >= trackStepCount) {
                            state.currentStep = state.currentStep % trackStepCount;
                            if (state.currentStep < 0)
                                state.currentStep += trackStepCount;
                        }

                        int stepIndex = state.currentStep;

                        double previousStepVelocity = state.stepVelocity;
                        double previousStepPan = state.stepPan;
                        double previousStepPitchOffset = state.stepPitchOffset;
                        int previousParameterStep = state.lastParameterStep;
                        bool parameterStepUpdated = false;

                        int parameterStep = (trackStepCount > 0 && stepIndex < trackStepCount) ? stepIndex : -1;
                        if (parameterStep >= 0) {
                            if (state.lastParameterStep != parameterStep) {
                                float cachedVelocity = (trackIndex < stepVelocityByTrack.size() &&
                                                        parameterStep < static_cast<int>(stepVelocityByTrack[trackIndex].size()))
                                                           ? stepVelocityByTrack[trackIndex][parameterStep]
                                                           : kTrackStepVelocityMax;
                                float cachedPan = (trackIndex < stepPanByTrack.size() &&
                                                   parameterStep < static_cast<int>(stepPanByTrack[trackIndex].size()))
                                                      ? stepPanByTrack[trackIndex][parameterStep]
                                                      : 0.0f;
                                float cachedPitch = (trackIndex < stepPitchByTrack.size() &&
                                                     parameterStep < static_cast<int>(stepPitchByTrack[trackIndex].size()))
                                                        ? stepPitchByTrack[trackIndex][parameterStep]
                                                        : 0.0f;

                                state.stepVelocity = std::clamp(static_cast<double>(cachedVelocity),
                                                                static_cast<double>(kTrackStepVelocityMin),
                                                                static_cast<double>(kTrackStepVelocityMax));
                                state.stepPan = std::clamp(static_cast<double>(cachedPan),
                                                           static_cast<double>(kTrackStepPanMin),
                                                           static_cast<double>(kTrackStepPanMax));
                                state.stepPitchOffset = std::clamp(static_cast<double>(cachedPitch),
                                                                   static_cast<double>(kTrackStepPitchMin),
                                                                   static_cast<double>(kTrackStepPitchMax));
                                state.lastParameterStep = parameterStep;
                                parameterStepUpdated = true;
                            }
                        } else {
                            state.stepVelocity = 1.0;
                            state.stepPan = 0.0;
                            state.stepPitchOffset = 0.0;
                            state.lastParameterStep = -1;
                        }

                        bool gate = false;
                        bool triggered = false;
                        const std::vector<StepNoteInfo>* stepNotes = nullptr;
                        static thread_local std::vector<StepNoteInfo> cachedNoteOnNotes;
                        static thread_local std::vector<int> cachedNotesPresent;
                        cachedNoteOnNotes.clear();
                        cachedNotesPresent.clear();
                        auto& noteOnNotes = cachedNoteOnNotes;
                        auto& notesPresent = cachedNotesPresent;
                        if (trackStepCount > 0 && stepIndex < trackStepCount) {
                            bool stepEnabled = (trackIndex < stepStatesByTrack.size() &&
                                                stepIndex < static_cast<int>(stepStatesByTrack[trackIndex].size()))
                                                   ? stepStatesByTrack[trackIndex][stepIndex]
                                                   : false;
                            if (trackIndex < stepNotesByTrack.size() &&
                                stepIndex < static_cast<int>(stepNotesByTrack[trackIndex].size()) &&
                                (trackInfo.type == TrackType::Synth || trackInfo.type == TrackType::MidiOut || trackInfo.type == TrackType::VST))
                            {
                                stepNotes = &stepNotesByTrack[trackIndex][stepIndex];
                            }

                            if (stepEnabled) {
                                gate = true;
                                if (stepAdvanced) {
                                    if (stepNotes && (trackInfo.type == TrackType::Synth ||
                                                      trackInfo.type == TrackType::MidiOut ||
                                                      trackInfo.type == TrackType::VST)) {
                                        noteOnNotes.reserve(stepNotes->size());
                                        notesPresent.reserve(stepNotes->size());
                                        for (const auto& noteInfo : *stepNotes) {
                                            int clampedNote = std::clamp(noteInfo.midiNote, 0, 127);
                                            double velocity = std::clamp(static_cast<double>(noteInfo.velocity),
                                                                         static_cast<double>(kTrackStepVelocityMin),
                                                                         static_cast<double>(kTrackStepVelocityMax));
                                            bool includeInPresent = noteInfo.sustain || velocity > 0.0;
                                            if (includeInPresent)
                                                notesPresent.push_back(clampedNote);
                                            if (!noteInfo.sustain && velocity > 0.0)
                                                noteOnNotes.push_back(noteInfo);
                                        }
                                        std::sort(notesPresent.begin(), notesPresent.end());
                                        notesPresent.erase(std::unique(notesPresent.begin(), notesPresent.end()), notesPresent.end());
                                        triggered = !noteOnNotes.empty();
                                    } else {
                                        triggered = true;
                                    }
                                }
                            }
                        }

                        bool stepHasNoteOnEvents = false;
                        if (trackInfo.type == TrackType::Synth || trackInfo.type == TrackType::MidiOut ||
                            trackInfo.type == TrackType::VST) {
                            stepHasNoteOnEvents = !noteOnNotes.empty();
                        } else {
                            stepHasNoteOnEvents = triggered;
                        }

                        if (parameterStepUpdated && !stepHasNoteOnEvents) {
                            state.stepVelocity = previousStepVelocity;
                            state.stepPan = previousStepPan;
                            state.stepPitchOffset = previousStepPitchOffset;
                            state.lastParameterStep = previousParameterStep;
                        }

                        if (trackInfo.id == activeTrackId && trackStepCount > 0) {
                            activeTrackStep = stepIndex;
                            activeTrackHasSteps = true;
                        }

                        TrackModulatedParameters modulatedParams = (*modulatedParameters)[trackIndex];

                        double trackLeft = 0.0;
                        double trackRight = 0.0;

                        if (trackInfo.type == TrackType::Sample) {
                            if (triggered) {
                                bool allowRetrigger = !state.samplePlaying ||
                                                       state.sampleEnvelopeStage == EnvelopeStage::Idle ||
                                                       state.sampleEnvelopeStage == EnvelopeStage::Release;
                                if (allowRetrigger) {
                                    if (state.sampleBuffer && state.sampleFrameCount > 0) {
                                        state.samplePlaying = true;
                                        state.samplePosition = 0.0;
                                        state.sampleEnvelopeStage = EnvelopeStage::Attack;
                                        state.sampleEnvelope = 0.0;
                                        state.sampleEnvelopeSmoothed = 0.0;
                                        state.sampleTailActive = false;
                                        state.sampleLastLeft = 0.0;
                                        state.sampleLastRight = 0.0;
                                    } else {
                                        state.samplePlaying = false;
                                        state.sampleEnvelopeStage = EnvelopeStage::Idle;
                                        state.sampleEnvelope = 0.0;
                                        state.sampleEnvelopeSmoothed = 0.0;
                                        state.sampleTailActive = false;
                                        state.sampleLastLeft = 0.0;
                                        state.sampleLastRight = 0.0;
                                    }
                                }
                            }

                            if (!gate && state.sampleEnvelopeStage != EnvelopeStage::Idle &&
                                state.sampleEnvelopeStage != EnvelopeStage::Release) {
                                state.sampleEnvelopeStage = EnvelopeStage::Release;
                            }

                            size_t playbackFrame = static_cast<size_t>(state.samplePosition);

                            if (state.samplePlaying && state.sampleBuffer) {
                                size_t index = playbackFrame;
                                if (index < state.sampleFrameCount) {
                                    int channels = std::max(state.sampleBuffer->channels, 1);
                                    const auto& rawSamples = state.sampleBuffer->samples;
                                    float leftSample = rawSamples[index * channels];
                                    float rightSample = channels > 1 ? rawSamples[index * channels + 1] : leftSample;
                                    trackLeft = static_cast<double>(leftSample);
                                    trackRight = static_cast<double>(rightSample);
                                    state.sampleLastLeft = trackLeft;
                                    state.sampleLastRight = trackRight;
                                    state.sampleTailActive = true;
                                    state.samplePosition += state.sampleIncrement;
                                } else {
                                    state.samplePlaying = false;
                                    if (state.sampleEnvelopeStage != EnvelopeStage::Idle)
                                        state.sampleEnvelopeStage = EnvelopeStage::Release;
                                }
                            }

                            if (!state.samplePlaying && state.sampleTailActive &&
                                state.sampleEnvelopeStage != EnvelopeStage::Idle) {
                                trackLeft = state.sampleLastLeft;
                                trackRight = state.sampleLastRight;
                            }

                            state.sampleEnvelope = advanceEnvelope(state.sampleEnvelopeStage, state.sampleEnvelope,
                                                                    modulatedParams.sampleAttack, 0.0, 1.0,
                                                                    modulatedParams.sampleRelease,
                                                                    sampleRate);

                            double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
                            double maxDelta = (kSampleEnvelopeSmoothingSeconds > 0.0)
                                ? (1.0 / (kSampleEnvelopeSmoothingSeconds * sr))
                                : 1.0;
                            if (!std::isfinite(maxDelta) || maxDelta <= 0.0)
                                maxDelta = 1.0;
                            double delta = state.sampleEnvelope - state.sampleEnvelopeSmoothed;
                            if (delta > maxDelta)
                                delta = maxDelta;
                            else if (delta < -maxDelta)
                                delta = -maxDelta;
                            state.sampleEnvelopeSmoothed += delta;
                            state.modulation.envelopeValue.store(state.sampleEnvelopeSmoothed, std::memory_order_relaxed);

                            double sampleGain = state.sampleEnvelopeSmoothed;
                            trackLeft *= sampleGain;
                            trackRight *= sampleGain;

#ifdef DEBUG_AUDIO
                            if (i == 0) {
                                double outputAmplitude = 0.5 * (std::abs(trackLeft) + std::abs(trackRight));
                                std::cout << "[Sampler] track=" << trackInfo.id
                                          << " frame=" << playbackFrame
                                          << " cursor=" << state.samplePosition
                                          << " stage=" << envelopeStageToString(state.sampleEnvelopeStage)
                                          << " env=" << state.sampleEnvelope
                                          << " smooth=" << state.sampleEnvelopeSmoothed
                                          << " gain=" << sampleGain
                                          << " amp=" << outputAmplitude
                                          << " playing=" << (state.samplePlaying ? "true" : "false")
                                          << " tail=" << (state.sampleTailActive ? "true" : "false")
                                          << std::endl;
                            }
#endif

                            if (state.sampleEnvelopeStage == EnvelopeStage::Idle) {
                                state.sampleTailActive = false;
                                if (!state.samplePlaying) {
                                    state.sampleEnvelope = 0.0;
                                    state.sampleEnvelopeSmoothed = 0.0;
                                    state.sampleLastLeft = 0.0;
                                    state.sampleLastRight = 0.0;
                                }
                            }
                        } else if (trackInfo.type == TrackType::VST) {
                            state.modulation.envelopeValue.store(0.0, std::memory_order_relaxed);
                            auto host = trackInfo.vstHost;
                            if (host && state.vstPrepared) {
                                auto queueNoteOff = [&](int note) {
                                    Steinberg::Vst::Event ev {};
                                    ev.busIndex = 0;
                                    ev.sampleOffset = 0;
                                    ev.type = Steinberg::Vst::Event::kNoteOffEvent;
                                    ev.noteOff.pitch = static_cast<float>(note);
                                    ev.noteOff.velocity = 0.0f;
                                    ev.noteOff.channel = static_cast<Steinberg::int16>(state.midiChannel);
                                    ev.noteOff.noteId = -1;
                                    host->queueNoteEvent(ev);
                                };

                                auto queueNoteOn = [&](const StepNoteInfo& noteInfo) {
                                    double velocity = std::clamp(static_cast<double>(noteInfo.velocity),
                                                                  static_cast<double>(kTrackStepVelocityMin),
                                                                  static_cast<double>(kTrackStepVelocityMax));
                                    if (velocity <= 0.0)
                                        return;

                                    int note = std::clamp(noteInfo.midiNote, 0, 127);
                                    float normalizedVelocity = static_cast<float>(std::clamp(velocity, 0.0, 1.0));

                                    Steinberg::Vst::Event onEvent {};
                                    onEvent.busIndex = 0;
                                    onEvent.sampleOffset = 0;
                                    onEvent.type = Steinberg::Vst::Event::kNoteOnEvent;
                                    onEvent.noteOn.pitch = static_cast<float>(note);
                                    onEvent.noteOn.velocity = normalizedVelocity;
                                    onEvent.noteOn.channel = static_cast<Steinberg::int16>(state.midiChannel);
                                    onEvent.noteOn.noteId = -1;
                                    host->queueNoteEvent(onEvent);

                                    Steinberg::Vst::Event pressureEvent {};
                                    pressureEvent.busIndex = 0;
                                    pressureEvent.sampleOffset = 0;
                                    pressureEvent.type = Steinberg::Vst::Event::kPolyPressureEvent;
                                    pressureEvent.polyPressure.pitch = static_cast<float>(note);
                                    pressureEvent.polyPressure.pressure = normalizedVelocity;
                                    pressureEvent.polyPressure.channel = static_cast<Steinberg::int16>(state.midiChannel);
                                    host->queueNoteEvent(pressureEvent);
                                };

                                if (!gate) {
                                    if (!state.activeMidiNotes.empty()) {
                                        for (int note : state.activeMidiNotes)
                                            queueNoteOff(note);
                                        state.activeMidiNotes.clear();
                                    }
                                } else if (stepAdvanced) {
                                    std::vector<int> notesThisStep = notesPresent;
                                    std::sort(notesThisStep.begin(), notesThisStep.end());
                                    notesThisStep.erase(std::unique(notesThisStep.begin(), notesThisStep.end()),
                                                        notesThisStep.end());

                                    for (int activeNote : state.activeMidiNotes) {
                                        if (!std::binary_search(notesThisStep.begin(), notesThisStep.end(), activeNote))
                                            queueNoteOff(activeNote);
                                    }

                                    for (const auto& noteInfo : noteOnNotes) {
                                        int note = std::clamp(noteInfo.midiNote, 0, 127);
                                        auto wasActive = std::find(state.activeMidiNotes.begin(), state.activeMidiNotes.end(), note)
                                                         != state.activeMidiNotes.end();
                                        if (wasActive)
                                            queueNoteOff(note);
                                        queueNoteOn(noteInfo);
                                    }

                                    state.activeMidiNotes = std::move(notesThisStep);
                                }

                                kj::VST3Host::HostTransportState transport {};
                                transport.samplePosition = transportSamplePosition + static_cast<double>(i);
                                transport.tempo = static_cast<double>(sequencerBPM.load(std::memory_order_relaxed));
                                transport.timeSigNum = 4;
                                transport.timeSigDen = 4;
                                transport.playing = playing;
                                host->setTransportState(transport);

                                float left = 0.0f;
                                float right = 0.0f;
                                float* outputs[2] = { &left, &right };
                                host->process(outputs, 2, 1);
                                trackLeft = static_cast<double>(left);
                                trackRight = static_cast<double>(right);
                            } else {
                                state.activeMidiNotes.clear();
                            }
                        } else if (trackInfo.type == TrackType::MidiOut) {
                            state.modulation.envelopeValue.store(0.0, std::memory_order_relaxed);
                            state.samplePlaying = false;
                            state.sampleTailActive = false;
                            state.voices.clear();

                            if (!gate) {
                                state.sampleEnvelopeStage = EnvelopeStage::Idle;
                                sendMidiNotesOffForState(state, state.midiPort, state.midiChannel);
                            } else if (stepAdvanced) {
                                std::vector<int> notesThisStep = notesPresent;
                                std::sort(notesThisStep.begin(), notesThisStep.end());
                                notesThisStep.erase(std::unique(notesThisStep.begin(), notesThisStep.end()), notesThisStep.end());

                                for (int activeNote : state.activeMidiNotes) {
                                    if (!std::binary_search(notesThisStep.begin(), notesThisStep.end(), activeNote)) {
                                        midiOutputSendNoteOff(state.midiPort, state.midiChannel, activeNote, 0);
                                    }
                                }

                                for (const auto& noteInfo : noteOnNotes) {
                                    int note = std::clamp(noteInfo.midiNote, 0, 127);
                                    auto wasActive = std::find(state.activeMidiNotes.begin(), state.activeMidiNotes.end(), note)
                                                         != state.activeMidiNotes.end();
                                    if (wasActive) {
                                        midiOutputSendNoteOff(state.midiPort, state.midiChannel, note, 0);
                                    }
                                }

                                for (const auto& noteInfo : noteOnNotes) {
                                    double velocity = std::clamp(static_cast<double>(noteInfo.velocity),
                                                                  static_cast<double>(kTrackStepVelocityMin),
                                                                  static_cast<double>(kTrackStepVelocityMax));
                                    int eventVelocity = static_cast<int>(std::lround(velocity * 127.0));
                                    eventVelocity = std::clamp(eventVelocity, 0, 127);
                                    if (eventVelocity <= 0)
                                        continue;
                                    int note = std::clamp(noteInfo.midiNote, 0, 127);
                                    midiOutputSendNoteOn(state.midiPort, state.midiChannel, note, eventVelocity);
                                }

                                state.activeMidiNotes = std::move(notesThisStep);
                            }

                        } else {
                            if (!gate) {
                                for (auto& voice : state.voices) {
                                    if (voice.envelopeStage != EnvelopeStage::Idle &&
                                        voice.envelopeStage != EnvelopeStage::Release) {
                                        voice.envelopeStage = EnvelopeStage::Release;
                                    }
                                }
                            }

                            if (gate && stepAdvanced) {
                                std::vector<TrackPlaybackState::SynthVoice> updatedVoices;
                                size_t stepNoteCount = stepNotes ? stepNotes->size() : 0;
                                updatedVoices.reserve(stepNoteCount + state.voices.size());
                                bool createdNewVoice = false;

                                auto findExistingVoice = [&state](int note) {
                                    return std::find_if(state.voices.begin(), state.voices.end(),
                                        [note](const TrackPlaybackState::SynthVoice& voice) {
                                            return voice.midiNote == note;
                                        });
                                };

                                if (stepNotes) {
                                    for (const auto& noteInfo : *stepNotes) {
                                        int note = std::clamp(noteInfo.midiNote, 0, 127);
                                        double noteVelocity = std::clamp(static_cast<double>(noteInfo.velocity),
                                                                         static_cast<double>(kTrackStepVelocityMin),
                                                                         static_cast<double>(kTrackStepVelocityMax));

                                        auto existingIt = findExistingVoice(note);
                                        bool hasExistingVoice = existingIt != state.voices.end();
                                        bool restartVoice = !noteInfo.sustain || !hasExistingVoice;
                                        TrackPlaybackState::SynthVoice voice = hasExistingVoice
                                            ? *existingIt
                                            : TrackPlaybackState::SynthVoice{};
                                        voice.midiNote = note;
                                        voice.frequency = midiNoteToFrequency(static_cast<double>(note) + modulatedParams.synthPitch + state.stepPitchOffset);

                                        if (!hasExistingVoice) {
                                            voice.velocitySmoothed = noteVelocity;
                                            voice.envelope = 0.0;
                                            voice.lastOutput = 0.0;
                                        }

                                        voice.velocity = noteVelocity;

                                        if (restartVoice) {
                                            voice.envelopeStage = EnvelopeStage::Attack;
                                            if (state.synthPhaseSync) {
                                                voice.phase = 0.0;
                                                voice.lastOutput = 0.0;
                                            }
                                            createdNewVoice = true;
                                        }

                                        updatedVoices.push_back(voice);
                                    }
                                }

                                for (auto& voice : state.voices) {
                                    bool noteStillPresent = std::binary_search(notesPresent.begin(), notesPresent.end(),
                                                                               voice.midiNote);
                                    if (noteStillPresent) {
                                        if (voice.envelopeStage == EnvelopeStage::Release) {
                                            updatedVoices.push_back(voice);
                                        }
                                        continue;
                                    }
                                    if (voice.envelopeStage != EnvelopeStage::Idle &&
                                        voice.envelopeStage != EnvelopeStage::Release) {
                                        voice.envelopeStage = EnvelopeStage::Release;
                                    }
                                    updatedVoices.push_back(voice);
                                }

                                state.voices = std::move(updatedVoices);

                                if (!state.voices.empty()) {
                                    state.currentMidiNote = state.voices.front().midiNote;
                                    state.currentFrequency = state.voices.front().frequency;
                                } else {
                                    state.currentMidiNote = 69;
                                    state.currentFrequency = midiNoteToFrequency(69);
                                }

                                if (createdNewVoice)
                                    state.pitchEnvelope = 1.0;
                            }

                            double sampleValue = 0.0;
                            if (!state.voices.empty()) {
                                double pitchRangeSemitones = std::max(0.0, modulatedParams.synthPitchRange - 1.0);
                                double pitchOffset = modulatedParams.synthPitch + state.stepPitchOffset +
                                                     state.pitchEnvelope * pitchRangeSemitones;
                                double feedbackMix = std::clamp(modulatedParams.synthFeedback, 0.0, 0.99);
                                SynthWaveType waveType = trackInfo.synthWaveType;
                                double totalVelocity = 0.0;
                                double modulationEnvelope = 0.0;
                                double sr = sampleRate > 0.0 ? sampleRate : 44100.0;
                                double velocityMaxDelta = (kSynthEnvelopeSmoothingSeconds > 0.0)
                                    ? (1.0 / (kSynthEnvelopeSmoothingSeconds * sr))
                                    : 1.0;
                                if (!std::isfinite(velocityMaxDelta) || velocityMaxDelta <= 0.0)
                                    velocityMaxDelta = 1.0;
                                for (auto& voice : state.voices) {
                                    double noteWithPitch = static_cast<double>(voice.midiNote) + pitchOffset;
                                    double frequency = midiNoteToFrequency(noteWithPitch);
                                    voice.frequency = frequency;
                                    double waveform = 0.0;
                                    switch (waveType)
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
                                    if (feedbackMix > 0.0)
                                    {
                                        waveform = waveform * (1.0 - feedbackMix) + voice.lastOutput * feedbackMix;
                                    }
                                    waveform = std::clamp(waveform, -1.0, 1.0);
                                    voice.lastOutput = waveform;
                                    double velocityTarget = std::clamp(voice.velocity,
                                                                      static_cast<double>(kTrackStepVelocityMin),
                                                                      static_cast<double>(kTrackStepVelocityMax));
                                    double velocityDelta = velocityTarget - voice.velocitySmoothed;
                                    if (velocityDelta > velocityMaxDelta)
                                        velocityDelta = velocityMaxDelta;
                                    else if (velocityDelta < -velocityMaxDelta)
                                        velocityDelta = -velocityMaxDelta;
                                    voice.velocitySmoothed += velocityDelta;
                                    if (!std::isfinite(voice.velocitySmoothed))
                                        voice.velocitySmoothed = velocityTarget;
                                    voice.velocitySmoothed = std::clamp(voice.velocitySmoothed,
                                                                       static_cast<double>(kTrackStepVelocityMin),
                                                                       static_cast<double>(kTrackStepVelocityMax));
                                    double velocityGain = voice.velocitySmoothed;
                                    double envelopeGain = advanceEnvelope(voice.envelopeStage, voice.envelope,
                                                                          modulatedParams.synthAttack,
                                                                          modulatedParams.synthDecay,
                                                                          modulatedParams.synthSustain,
                                                                          modulatedParams.synthRelease,
                                                                          sampleRate);
                                    voice.envelope = envelopeGain;
                                    modulationEnvelope += envelopeGain;
                                    sampleValue += waveform * velocityGain * envelopeGain;
                                    totalVelocity += velocityGain;
                                    double increment = twoPi * frequency / sampleRate;
                                    voice.phase += increment;
                                    if (voice.phase >= twoPi)
                                    {
                                        voice.phase = std::fmod(voice.phase, twoPi);
                                    }
                                }
                                double gainTarget = (totalVelocity > 0.0) ? (1.0 / totalVelocity) : 1.0;
                                double gainMaxDelta = (kSynthGainSmoothingSeconds > 0.0)
                                    ? (1.0 / (kSynthGainSmoothingSeconds * sr))
                                    : 1.0;
                                if (!std::isfinite(gainMaxDelta) || gainMaxDelta <= 0.0)
                                    gainMaxDelta = 1.0;
                                double gainDelta = gainTarget - state.synthGainSmoothed;
                                if (gainDelta > gainMaxDelta)
                                    gainDelta = gainMaxDelta;
                                else if (gainDelta < -gainMaxDelta)
                                    gainDelta = -gainMaxDelta;
                                state.synthGainSmoothed += gainDelta;
                                if (!std::isfinite(state.synthGainSmoothed))
                                    state.synthGainSmoothed = gainTarget;
                                if (state.synthGainSmoothed < 0.0)
                                    state.synthGainSmoothed = 0.0;
                                sampleValue *= state.synthGainSmoothed;
                                double envelopeAverage = modulationEnvelope / static_cast<double>(state.voices.size());
                                if (!std::isfinite(envelopeAverage))
                                    envelopeAverage = 0.0;
                                state.modulation.envelopeValue.store(envelopeAverage, std::memory_order_relaxed);
                                if (state.pitchEnvelope > 0.0)
                                {
                                    state.pitchEnvelope = std::max(0.0, state.pitchEnvelope - state.pitchEnvelopeStep);
                                    if (state.pitchEnvelope < 1e-6)
                                        state.pitchEnvelope = 0.0;
                                }
                            } else {
                                state.synthGainSmoothed = 1.0;
                                state.modulation.envelopeValue.store(0.0, std::memory_order_relaxed);
                            }
                            state.voices.erase(std::remove_if(state.voices.begin(), state.voices.end(),
                                [](const TrackPlaybackState::SynthVoice& voice) {
                                    return voice.envelopeStage == EnvelopeStage::Idle && voice.envelope <= 0.0;
                                }), state.voices.end());
                            if (state.voices.empty()) {
                                state.currentMidiNote = 69;
                                state.currentFrequency = midiNoteToFrequency(69);
                            } else {
                                state.currentMidiNote = state.voices.front().midiNote;
                                state.currentFrequency = state.voices.front().frequency;
                            }
                            trackLeft = sampleValue;
                            trackRight = sampleValue;

                            double modFormant = std::clamp(modulatedParams.synthFormant, 0.0, 1.0);
                            double modResonance = std::clamp(modulatedParams.synthResonance, 0.0, 1.0);
                            bool formantNeedsUpdate = std::abs(modFormant - state.lastAppliedFormant) > 1e-4 ||
                                                      std::abs(modResonance - state.lastAppliedResonance) > 1e-4;
                            if (formantNeedsUpdate)
                            {
                                configureFormantFilter(state.formantFilter, sampleRate, modFormant, modResonance);
                                state.lastAppliedFormant = modFormant;
                                state.lastAppliedResonance = modResonance;
                            }

                            double blend = std::clamp(modFormant, 0.0, 1.0);
                            if (blend < 1.0 || modResonance > 0.0) {
                                double filteredLeft = processBiquadSample(state.formantFilter, trackLeft, false);
                                double filteredRight = processBiquadSample(state.formantFilter, trackRight, true);
                                trackLeft = filteredLeft * (1.0 - blend) + trackLeft * blend;
                                trackRight = filteredRight * (1.0 - blend) + trackRight * blend;
                            }
                        }

                        if (state.resetScheduled) {
                            trackLeft *= state.resetFadeGain;
                            trackRight *= state.resetFadeGain;

                            if (state.resetFadeSamples > 0) {
                                state.resetFadeGain = std::max(0.0, state.resetFadeGain - state.resetFadeStep);
                                --state.resetFadeSamples;
                            }

                            bool fadeComplete = (state.resetFadeSamples <= 0) || (state.resetFadeGain <= 1e-6);
                            if (fadeComplete) {
                                trackLeft = 0.0;
                                trackRight = 0.0;
                                resetSamplePlaybackState(state);
                                resetSynthPlaybackState(state);
                                state.currentStep = 0;
                                state.lastParameterStep = -1;
                                state.stepVelocity = 1.0;
                                state.stepPan = 0.0;
                                state.stepPitchOffset = 0.0;
                                state.sidechain.reset();
                                state.resetScheduled = false;
                                state.resetFadeGain = 1.0;
                                state.resetFadeStep = 0.0;
                                state.resetFadeSamples = 0;
                                state.resetReason = SequencerResetReason::Manual;
                            }
                        }

                        double processedLeft = trackLeft;
                        double processedRight = trackRight;
                        if (state.eqEnabled)
                        {
                            processedLeft = processBiquadSample(state.lowShelf, processedLeft, false);
                            processedLeft = processBiquadSample(state.midPeak, processedLeft, false);
                            processedLeft = processBiquadSample(state.highShelf, processedLeft, false);

                            processedRight = processBiquadSample(state.lowShelf, processedRight, true);
                            processedRight = processBiquadSample(state.midPeak, processedRight, true);
                            processedRight = processBiquadSample(state.highShelf, processedRight, true);
                        }

                        if (state.compressorEnabled)
                        {
                            double inputLevel = std::max(std::abs(processedLeft), std::abs(processedRight));
                            double inputDb = 20.0 * std::log10(inputLevel + 1e-12);
                            double gainDb = 0.0;
                            double compressorThreshold = modulatedParams.compressorThreshold;
                            double compressorRatio = std::max(modulatedParams.compressorRatio, kCompressorRatioMin);
                            if (inputDb > compressorThreshold)
                            {
                                double overDb = inputDb - compressorThreshold;
                                double compressedDb = compressorThreshold + overDb / compressorRatio;
                                gainDb = compressedDb - inputDb;
                            }
                            double targetGain = std::pow(10.0, gainDb / 20.0);
                            if (!std::isfinite(targetGain))
                                targetGain = 1.0;
                            double coeff = (targetGain < state.compressorGain) ? state.compressorAttackCoeff
                                                                               : state.compressorReleaseCoeff;
                            state.compressorGain = targetGain + coeff * (state.compressorGain - targetGain);
                            if (!std::isfinite(state.compressorGain))
                                state.compressorGain = 1.0;
                            processedLeft *= state.compressorGain;
                            processedRight *= state.compressorGain;
                        }
                        else
                        {
                            state.compressorGain = 1.0;
                        }

                        if (state.delayEnabled && state.delayEffect)
                        {
                            state.delayEffect->setMix(static_cast<float>(modulatedParams.delayMix));
                            float delayLeft = static_cast<float>(processedLeft);
                            float delayRight = static_cast<float>(processedRight);
                            state.delayEffect->process(&delayLeft, &delayRight, 1);
                            processedLeft = delayLeft;
                            processedRight = delayRight;
                        }

                        double sidechainGain = 1.0;
                        if (state.sidechain.enabled())
                        {
                            double sourceLevel = 0.0;
                            int sourceTrackId = state.sidechain.sourceTrackId();
                            if (sourceTrackId == trackInfo.id)
                            {
                                sourceLevel = state.sidechain.detectorLevel();
                            }
                            else
                            {
                                auto sourceIt = playbackStates.find(sourceTrackId);
                                if (sourceIt != playbackStates.end())
                                {
                                    sourceLevel = sourceIt->second.sidechain.detectorLevel();
                                }
                            }
                            sidechainGain = state.sidechain.computeGain(sourceLevel, sampleRate);
                        }
                        else
                        {
                            state.sidechain.resetEnvelope();
                        }

                        processedLeft *= sidechainGain;
                        processedRight *= sidechainGain;

                        double combinedPan = std::clamp(modulatedParams.pan + state.stepPan, -1.0, 1.0);
                        double panAmount = std::clamp((combinedPan + 1.0) * 0.5, 0.0, 1.0);
                        double leftPanGain = std::cos(panAmount * (kPi * 0.5));
                        double rightPanGain = std::sin(panAmount * (kPi * 0.5));
                        double volumeGain = std::clamp(modulatedParams.volume, 0.0, 1.0) * state.stepVelocity;

                        double finalLeft = processedLeft * volumeGain * leftPanGain;
                        double finalRight = processedRight * volumeGain * rightPanGain;

                        leftValue += finalLeft;
                        rightValue += finalRight;

                        double detectionLevel = std::max(std::abs(finalLeft), std::abs(finalRight));
                        state.sidechain.setDetectorLevel(detectionLevel);
                    }

                    if (activeTrackHasSteps) {
                        sequencerCurrentStep.store(activeTrackStep, std::memory_order_relaxed);
                    } else {
                        sequencerCurrentStep.store(0, std::memory_order_relaxed);
                    }

                    leftValue = std::clamp(leftValue, -1.0, 1.0);
                    rightValue = std::clamp(rightValue, -1.0, 1.0);

#ifdef DEBUG_AUDIO
                    mixSumAbs += std::abs(leftValue) + std::abs(rightValue);
                    double currentPeak = std::max(std::abs(leftValue), std::abs(rightValue));
                    if (currentPeak > mixPeak)
                        mixPeak = currentPeak;
#endif

                    if (playing)
                        transportSamplePosition += 1.0;

                    float monoValue = static_cast<float>((leftValue + rightValue) * 0.5);
                    if (capturedCount < capturedSamples.size())
                        capturedSamples[capturedCount++] = monoValue;
                    writeFrame(i, leftValue, rightValue);
                    continue;
                }
                if (playing)
                    transportSamplePosition += 1.0;
                if (capturedCount < capturedSamples.size())
                    capturedSamples[capturedCount++] = 0.0f;
                writeFrame(i, 0.0, 0.0);
            }

            if (capturedCount > 0)
                writeWaveformSamples(capturedSamples.data(), capturedCount);
#ifdef DEBUG_AUDIO
            double averageAmplitude = (available > 0)
                ? (mixSumAbs / (static_cast<double>(available) * 2.0))
                : 0.0;
            auto now = std::chrono::steady_clock::now();
            double intervalMs = 0.0;
            if (lastCallbackTime.time_since_epoch().count() != 0) {
                intervalMs = std::chrono::duration<double, std::milli>(now - lastCallbackTime).count();
            }
            lastCallbackTime = now;
            std::wstring driverNameCopy = getDeviceSnapshot().activeName;
            bool playingState = isPlaying.load(std::memory_order_relaxed);
            std::cout << "[AudioEngine] driver=" << narrowFromWide(driverNameCopy)
                      << " bufferFrames=" << available
                      << " deviceFrames=" << bufferFrameCount
                      << " sampleRate=" << sampleRate
                      << " intervalMs=" << intervalMs
                      << " tracks=" << trackInfos.size()
                      << " avg=" << averageAmplitude
                      << " peak=" << mixPeak
                      << " playing=" << (playingState ? "true" : "false")
                      << std::endl;
#endif
            deviceHandler->releaseBuffer(available);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    cacheThreadRunning.store(false, std::memory_order_release);
    if (cacheUpdater.joinable())
        cacheUpdater.join();

    if (deviceHandler) {
        deviceHandler->stop();
        deviceHandler->shutdown();
    }
    for (auto& entry : playbackStates) {
        releaseDelayEffect(entry.second);
    }
    audioSequencerReady.store(false, std::memory_order_release);
    CoUninitialize();
}

// Real-time safety summary:
// - audioLoop is the render-thread entry; it must remain allocation- and lock-free.
// - Replaced mutex-protected audioThreadNotifications with an SPSC ring buffer to
//   avoid locks and waits in the render path.
// - Additional legacy allocations and container mutations still exist in the
//   render path and require follow-up passes to conform fully to the real-time
//   design.

#include "audio_engine_devices.inl"

#include "audio_engine_waveform.inl"

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
    running.store(true, std::memory_order_release);
    audioSequencerReady.store(false, std::memory_order_release);
    if (sequencerThread.joinable())
        sequencerThread.join();
    if (audioThread.joinable())
        audioThread.join();
    if (vstCommandThread.joinable())
        vstCommandThread.join();
    sequencerThread = std::thread(sequencerWarmupLoop);
    vstCommandThread = std::thread(vstCommandLoop);
    audioThread = std::thread(audioLoop);
}

void shutdownAudio() {
    running.store(false, std::memory_order_release);
    audioSequencerReady.store(false, std::memory_order_release);
    isPlaying.store(false, std::memory_order_relaxed);
    vstCommandCv.notify_all();
    if (audioThread.joinable()) audioThread.join();
    if (vstCommandThread.joinable()) vstCommandThread.join();
    if (sequencerThread.joinable()) sequencerThread.join();
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
