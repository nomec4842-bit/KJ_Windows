#include "core/audio_device_handler.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "hosting/VST3Host.h"

extern uint32_t VST3Host_GetExpectedOutputChannels(kj::VST3Host* host);

std::atomic<bool> AudioDeviceHandler::streamStarted_{false};
std::atomic<bool> AudioDeviceHandler::callbackInvoked_{false};

#if defined(_WIN32) || defined(_MSC_VER) || defined(__MINGW32__)

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <mmreg.h>
#include <ksmedia.h>
#include <iomanip>
#include <sstream>

namespace {
constexpr DWORD kStreamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
constexpr REFERENCE_TIME kBufferDuration = 10000000; // 1 second

bool isFloatFormat(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT && format->wBitsPerSample == 32) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT) &&
               extensible->Format.wBitsPerSample == 32;
    }
    return false;
}

bool isPcm16Format(const WAVEFORMATEX* format) {
    if (!format) {
        return false;
    }
    if (format->wFormatTag == WAVE_FORMAT_PCM && format->wBitsPerSample == 16) {
        return true;
    }
    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= (sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))) {
        const auto* extensible = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(extensible->SubFormat, KSDATAFORMAT_SUBTYPE_PCM) &&
               extensible->Format.wBitsPerSample == 16;
    }
    return false;
}

void logMessage(const std::wstring& message) {
    std::wstring formatted = L"[AudioDeviceHandler] " + message + L"\n";
    OutputDebugStringW(formatted.c_str());
}

std::wstring describeHResult(HRESULT hr) {
    LPWSTR buffer = nullptr;
    DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, hr, MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring description;
    if (size != 0 && buffer) {
        description.assign(buffer, size);
        LocalFree(buffer);
        while (!description.empty() && (description.back() == L'\r' || description.back() == L'\n')) {
            description.pop_back();
        }
    }
    return description;
}

void logFailure(const wchar_t* action, HRESULT hr) {
    std::wstringstream stream;
    stream << action << L" failed with HRESULT 0x" << std::uppercase << std::hex << std::setw(8)
           << std::setfill(L'0') << static_cast<unsigned long>(hr);
    std::wstring description = describeHResult(hr);
    if (!description.empty()) {
        stream << L" (" << description << L")";
    }
    logMessage(stream.str());
}

void logInfo(const std::wstring& message) {
    logMessage(message);
}
}

namespace {
std::atomic<bool> running_{false};
std::thread renderThread_{};
HANDLE samplesReadyEvent_ = nullptr;
}

namespace {
constexpr size_t kRingBufferCapacityBlocks = 8;
static const UINT32 engineBlockSize = 256;
}

AudioDeviceHandler::AudioDeviceHandler() = default;

AudioDeviceHandler::~AudioDeviceHandler() {
    shutdown();
}

void AudioDeviceHandler::setVSTHost(kj::VST3Host* host) {
    vstHost_ = host;
}

void AudioDeviceHandler::registerStreamCallback(AudioStreamCallback callback, void* userData) {
    callback_ = callback;
    callbackContext_ = userData;
    callbackInvoked_.store(false, std::memory_order_relaxed);
    if (callback_) {
        logInfo(L"Registered audio stream callback");
    } else {
        logInfo(L"Cleared audio stream callback");
    }
}

AudioDeviceHandler::AudioStreamCallback AudioDeviceHandler::streamCallback() const {
    return callback_;
}

void* AudioDeviceHandler::streamCallbackContext() const {
    return callbackContext_;
}

void AudioDeviceHandler::notifyCallbackExecuted() {
    callbackInvoked_.store(true, std::memory_order_release);
}

void AudioDeviceHandler::resetCallbackMonitor() {
    streamStarted_.store(false, std::memory_order_release);
    callbackInvoked_.store(false, std::memory_order_release);
}

bool AudioDeviceHandler::streamStartedSuccessfully() {
    return streamStarted_.load(std::memory_order_acquire);
}

bool AudioDeviceHandler::callbackHasFired() {
    return callbackInvoked_.load(std::memory_order_acquire);
}

void AudioDeviceHandler::FormatDeleter::operator()(WAVEFORMATEX* format) const {
    if (format) {
        CoTaskMemFree(format);
    }
}

void AudioDeviceHandler::resetComObjectsLocked() {
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

void AudioDeviceHandler::resetStateLocked() {
    resetComObjectsLocked();
    if (samplesReadyEvent_) {
        CloseHandle(samplesReadyEvent_);
        samplesReadyEvent_ = nullptr;
    }
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
    activeRenderBuffer_ = nullptr;
    activeRenderFrameCount_ = 0;
    activeRenderBufferSizeBytes_ = 0;
    bufferPendingRelease_ = false;
}

void AudioDeviceHandler::initializeRingBuffer(UINT32 framesPerBuffer, UINT32 channels) {
    framesPerBlock_ = framesPerBuffer;
    ringBufferChannels_ = channels;
    ringBufferCapacityBlocks_ = kRingBufferCapacityBlocks;
    const size_t totalSamples = static_cast<size_t>(framesPerBlock_) * ringBufferChannels_ * ringBufferCapacityBlocks_;
    ringBuffer_.assign(totalSamples, 0.0f);
    ringBufferReadIndex_.store(0, std::memory_order_release);
    ringBufferWriteIndex_.store(0, std::memory_order_release);
}

bool AudioDeviceHandler::pushAudioBlock(const float* interleavedBlock) {
    if (!interleavedBlock || framesPerBlock_ == 0 || ringBufferChannels_ == 0 || ringBufferCapacityBlocks_ == 0) {
        return false;
    }

    const size_t write = ringBufferWriteIndex_.load(std::memory_order_relaxed);
    const size_t read = ringBufferReadIndex_.load(std::memory_order_acquire);
    const size_t next = (write + 1) % ringBufferCapacityBlocks_;
    if (next == read) {
        return false; // Buffer full
    }

    const size_t offset = write * static_cast<size_t>(framesPerBlock_) * ringBufferChannels_;
    std::memcpy(&ringBuffer_[offset], interleavedBlock,
                static_cast<size_t>(framesPerBlock_) * ringBufferChannels_ * sizeof(float));
    ringBufferWriteIndex_.store(next, std::memory_order_release);
    return true;
}

bool AudioDeviceHandler::popAudioBlock(float* interleavedBlock) {
    if (!interleavedBlock || framesPerBlock_ == 0 || ringBufferChannels_ == 0 || ringBufferCapacityBlocks_ == 0) {
        return false;
    }

    const size_t read = ringBufferReadIndex_.load(std::memory_order_relaxed);
    const size_t write = ringBufferWriteIndex_.load(std::memory_order_acquire);
    if (read == write) {
        return false; // Buffer empty
    }

    const size_t offset = read * static_cast<size_t>(framesPerBlock_) * ringBufferChannels_;
    std::memcpy(interleavedBlock, &ringBuffer_[offset],
                static_cast<size_t>(framesPerBlock_) * ringBufferChannels_ * sizeof(float));
    ringBufferReadIndex_.store((read + 1) % ringBufferCapacityBlocks_, std::memory_order_release);
    return true;
}

bool AudioDeviceHandler::initialize(const std::wstring& deviceId) {
    std::unique_lock<std::mutex> lock(stateMutex_);

    if (initThreadActive_) {
        if (!initCompleted_) {
            return false;
        }

        bool success = initSuccess_;
        initThreadActive_ = false;
        initCompleted_ = false;
        lock.unlock();
        if (initThread_.joinable()) {
            initThread_.join();
        }
        lock.lock();
        if (!success) {
            logInfo(L"Audio device initialization thread reported failure");
            resetStateLocked();
            return false;
        }
    }

    if (initialized_) {
        if ((deviceId.empty() && deviceId_.empty()) || (!deviceId.empty() && deviceId == deviceId_)) {
            return true;
        }
    }

    cancelRequested_ = false;
    resetStateLocked();
    initThreadActive_ = true;
    initCompleted_ = false;
    initSuccess_ = false;

    std::thread worker([this, deviceId]() {
        bool success = runInitialization(deviceId);
        {
            std::lock_guard<std::mutex> guard(stateMutex_);
            initSuccess_ = success;
            initCompleted_ = true;
        }
    });

    if (initThread_.joinable()) {
        lock.unlock();
        initThread_.join();
        lock.lock();
    }
    initThread_ = std::move(worker);
    logInfo(L"Started audio device initialization thread");
    return initSuccess_;
}

bool AudioDeviceHandler::isInitializing() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return initThreadActive_ && !initCompleted_;
}

bool AudioDeviceHandler::runInitialization(const std::wstring& deviceId) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool shouldUninitialize = SUCCEEDED(hr);
    if (FAILED(hr)) {
        if (hr != RPC_E_CHANGED_MODE) {
            logFailure(L"CoInitializeEx", hr);
            return false;
        }
        shouldUninitialize = false;
    }

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* client = nullptr;
    IAudioRenderClient* renderClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;
    UINT32 bufferFrameCount = 0;
    std::wstring resolvedDeviceId;
    std::wstring resolvedDeviceName;
    bool success = false;
    bool fallbackUsed = false;
    std::wstring selectedDeviceName;

    do {
        hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                              __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
        if (FAILED(hr) || !enumerator) {
            logFailure(L"CoCreateInstance(IMMDeviceEnumerator)", hr);
            break;
        }

        const wchar_t* getDeviceAction = L"IMMDeviceEnumerator::GetDevice";
        if (deviceId.empty()) {
            hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
            getDeviceAction = L"IMMDeviceEnumerator::GetDefaultAudioEndpoint";
        } else {
            hr = enumerator->GetDevice(deviceId.c_str(), &device);
        }

        if (FAILED(hr) || !device) {
            logFailure(getDeviceAction, hr);

            if (deviceId.empty()) {
                IMMDeviceCollection* collection = nullptr;
                hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
                if (FAILED(hr) || !collection) {
                    logFailure(L"IMMDeviceEnumerator::EnumAudioEndpoints", hr);
                    break;
                }

                UINT count = 0;
                hr = collection->GetCount(&count);
                if (FAILED(hr)) {
                    logFailure(L"IMMDeviceCollection::GetCount", hr);
                    collection->Release();
                    break;
                }

                if (count == 0) {
                    logInfo(L"No active audio render devices were found");
                    collection->Release();
                    break;
                }

                logInfo(L"Falling back to the first available audio render device");

                for (UINT index = 0; index < count; ++index) {
                    IMMDevice* candidate = nullptr;
                    hr = collection->Item(index, &candidate);
                    if (FAILED(hr) || !candidate) {
                        if (candidate) {
                            candidate->Release();
                        }
                        continue;
                    }
                    device = candidate;
                    fallbackUsed = true;
                    break;
                }

                collection->Release();

                if (!device) {
                    logInfo(L"Unable to select a fallback audio render device");
                    break;
                }
            } else {
                break;
            }
        }

        LPWSTR resolvedId = nullptr;
        hr = device->GetId(&resolvedId);
        if (SUCCEEDED(hr) && resolvedId) {
            resolvedDeviceId = resolvedId;
            CoTaskMemFree(resolvedId);
        }

        IPropertyStore* propertyStore = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (SUCCEEDED(hr) && propertyStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    resolvedDeviceName = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            propertyStore->Release();
        }

        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, NULL, (void**)&client);
        if (FAILED(hr) || !client) {
            logFailure(L"IMMDevice::Activate(IAudioClient)", hr);
            break;
        }

        hr = client->GetMixFormat(&mixFormat);
        if (FAILED(hr) || !mixFormat) {
            logFailure(L"IAudioClient::GetMixFormat", hr);
            break;
        }

        if (mixFormat->nChannels == 0) {
            logInfo(L"Mix format reported zero channels, defaulting to stereo output");
            mixFormat->nChannels = 2;
            mixFormat->wBitsPerSample = 16;
        }
        if (mixFormat->nSamplesPerSec == 0) {
            logInfo(L"Mix format reported zero sample rate, defaulting to 44100 Hz");
            mixFormat->nSamplesPerSec = 44100;
        }
        if (mixFormat->nBlockAlign == 0 && mixFormat->nChannels > 0) {
            mixFormat->nBlockAlign = (mixFormat->wBitsPerSample / 8) * mixFormat->nChannels;
        }
        if (mixFormat->nAvgBytesPerSec == 0) {
            mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
        }

        WAVEFORMATEX* originalMixFormat = mixFormat;
        bool originalMixFormatUsed = true;
        WAVEFORMATEX* supportedClosest = nullptr;

        if (!isFloatFormat(mixFormat)) {
            UINT16 desiredChannels = mixFormat->nChannels ? mixFormat->nChannels : 2;
            DWORD desiredSampleRate = mixFormat->nSamplesPerSec ? mixFormat->nSamplesPerSec : 44100;

            WAVEFORMATEX desiredFloat{};
            desiredFloat.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
            desiredFloat.nChannels = desiredChannels;
            desiredFloat.nSamplesPerSec = desiredSampleRate;
            desiredFloat.wBitsPerSample = 32;
            desiredFloat.nBlockAlign = desiredFloat.nChannels * (desiredFloat.wBitsPerSample / 8);
            desiredFloat.nAvgBytesPerSec = desiredFloat.nSamplesPerSec * desiredFloat.nBlockAlign;
            desiredFloat.cbSize = 0;

            HRESULT supportResult = client->IsFormatSupported(AUDCLNT_SHAREMODE_SHARED, &desiredFloat, &supportedClosest);
            if (supportResult == S_OK) {
                WAVEFORMATEX* floatFormat = reinterpret_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
                if (floatFormat) {
                    *floatFormat = desiredFloat;
                    mixFormat = floatFormat;
                    originalMixFormatUsed = false;
                    std::wstringstream stream;
                    stream << L"Using float render format: " << desiredChannels << L" channels at "
                           << desiredSampleRate << L" Hz";
                    logInfo(stream.str());
                } else {
                    logInfo(L"Failed to allocate memory for float audio format; falling back to device mix format");
                }
            } else if (supportResult == S_FALSE && supportedClosest) {
                mixFormat = supportedClosest;
                supportedClosest = nullptr;
                originalMixFormatUsed = false;
                std::wstringstream stream;
                stream << L"Using closest supported audio format: " << mixFormat->nChannels
                       << L" channels, " << mixFormat->wBitsPerSample << L" bits";
                logInfo(stream.str());
            } else if (FAILED(supportResult)) {
                logFailure(L"IAudioClient::IsFormatSupported", supportResult);
            }
        } else {
            std::wstringstream stream;
            stream << L"Device mix format already uses 32-bit float with " << mixFormat->nChannels
                   << L" channels at " << mixFormat->nSamplesPerSec << L" Hz";
            logInfo(stream.str());
        }

        if (supportedClosest) {
            CoTaskMemFree(supportedClosest);
            supportedClosest = nullptr;
        }
        if (!originalMixFormatUsed && originalMixFormat) {
            CoTaskMemFree(originalMixFormat);
            originalMixFormat = nullptr;
        }
        if (mixFormat && mixFormat->nBlockAlign == 0 && mixFormat->nChannels > 0) {
            mixFormat->nBlockAlign = (mixFormat->wBitsPerSample / 8) * mixFormat->nChannels;
        }
        if (mixFormat && mixFormat->nAvgBytesPerSec == 0) {
            mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, kBufferDuration, 0,
                                 mixFormat, NULL);
        if (FAILED(hr)) {
            logFailure(L"IAudioClient::Initialize", hr);
            break;
        }

        if (!samplesReadyEvent_) {
            samplesReadyEvent_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
            if (!samplesReadyEvent_) {
                logInfo(L"Failed to create samples ready event");
                break;
            }
        }

        hr = client->SetEventHandle(samplesReadyEvent_);
        if (FAILED(hr)) {
            logFailure(L"IAudioClient::SetEventHandle", hr);
            break;
        }

        hr = client->GetBufferSize(&bufferFrameCount);
        if (FAILED(hr)) {
            logFailure(L"IAudioClient::GetBufferSize", hr);
            break;
        }

        hr = client->GetService(__uuidof(IAudioRenderClient), (void**)&renderClient);
        if (FAILED(hr) || !renderClient) {
            logFailure(L"IAudioClient::GetService(IAudioRenderClient)", hr);
            break;
        }

        {
            std::lock_guard<std::mutex> lock(stateMutex_);
            if (cancelRequested_) {
                logInfo(L"Audio device initialization canceled");
                break;
            }

            resetComObjectsLocked();
            mixFormat_.reset(mixFormat);
            mixFormat = nullptr;
            bufferFrameCount_ = bufferFrameCount;
            enumerator_ = enumerator;
            device_ = device;
            client_ = client;
            renderClient_ = renderClient;
            initialized_ = true;
            deviceId_ = std::move(resolvedDeviceId);
            if (deviceId_.empty()) {
                deviceId_ = deviceId;
            }
            deviceName_ = std::move(resolvedDeviceName);
            if (deviceName_.empty()) {
                deviceName_ = L"Audio Device";
            }
            selectedDeviceName = deviceName_;
        }

        logInfo(L"Audio device initialization succeeded");
        if (fallbackUsed) {
            std::wstring message = L"Using fallback audio device: " + selectedDeviceName;
            logInfo(message);
        }

        enumerator = nullptr;
        device = nullptr;
        client = nullptr;
        renderClient = nullptr;
        success = true;
    } while (false);

    if (renderClient) {
        renderClient->Release();
    }
    if (client) {
        client->Release();
    }
    if (device) {
        device->Release();
    }
    if (enumerator) {
        enumerator->Release();
    }
    if (mixFormat) {
        CoTaskMemFree(mixFormat);
    }
    if (shouldUninitialize) {
        CoUninitialize();
    }
    return success;
}

void AudioDeviceHandler::shutdown() {
    std::thread pendingThread;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        cancelRequested_ = true;
        if (client_) {
            client_->Stop();
        }
        resetStateLocked();

        if (initThread_.joinable()) {
            pendingThread = std::move(initThread_);
        }
        initThreadActive_ = false;
        initCompleted_ = false;
        initSuccess_ = false;
    }

    if (pendingThread.joinable()) {
        pendingThread.join();
    }
    streamStarted_.store(false, std::memory_order_release);
}

bool AudioDeviceHandler::start() {
    std::unique_lock<std::mutex> lock(stateMutex_);
    if (initThreadActive_) {
        lock.unlock();
        if (initThread_.joinable()) {
            initThread_.join();
        }
        lock.lock();

        initThreadActive_ = false;
        initCompleted_ = false;
    }

    if (!initSuccess_) {
        return false;
    }
    if (!initialized_ || !client_) {
        logInfo(L"Audio client start requested before initialization completed successfully");
        return false;
    }
    if (!mixFormat_ || mixFormat_->nChannels == 0) {
        logInfo(L"Audio client start aborted because the output format has no channels");
        return false;
    }

    if (renderThread_.joinable()) {
        running_.store(false);
        lock.unlock();
        renderThread_.join();
        lock.lock();
    }
    if (dspThread_.joinable()) {
        dspRunning_.store(false, std::memory_order_release);
        lock.unlock();
        dspThread_.join();
        lock.lock();
    }

    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        logFailure(L"IAudioClient::Start", hr);
        return false;
    }
    logInfo(L"Audio client started successfully");
    streamStarted_.store(true, std::memory_order_release);

    const UINT32 bufferFrameCount = bufferFrameCount_;
    const WORD channelCount = mixFormat_ ? mixFormat_->nChannels : 0;

    initializeRingBuffer(engineBlockSize, channelCount);

    dspRunning_.store(true);
    dspThread_ = std::thread([this]() {
        const uint32_t frames = engineBlockSize;
        // Determine channel count based on VST3 bus arrangement
        uint32_t channels = ringBufferChannels_;
        if (vstHost_) {
            uint32_t expected = VST3Host_GetExpectedOutputChannels(vstHost_);
            if (expected > 0)
                channels = expected;
        }
        std::vector<float> interleavedBlock(static_cast<size_t>(frames) * channels, 0.0f);

        tempChannelBuffers_.resize(channels);
        tempChannelPointers_.resize(channels);

        for (uint32_t c = 0; c < channels; ++c) {
            tempChannelBuffers_[c].assign(frames, 0.0f);
            tempChannelPointers_[c] = tempChannelBuffers_[c].data();
        }

        while (dspRunning_.load(std::memory_order_relaxed)) {
            // Prepare input buffers (currently silence) and process via VST host
            if (vstHost_) {
                // Correct overload: (inputs, numInputs, outputs, numOutputs, samples)
                vstHost_->process(
                    nullptr,                 // no audio inputs
                    0,                       // num input channels
                    tempChannelPointers_.data(),
                    static_cast<int>(channels),
                    static_cast<int>(frames)
                );
            } else {
                for (uint32_t c = 0; c < channels; ++c) {
                    std::fill(tempChannelBuffers_[c].begin(), tempChannelBuffers_[c].end(), 0.0f);
                }
            }

            // Interleave processed buffers
            for (uint32_t f = 0; f < frames; ++f) {
                for (uint32_t c = 0; c < channels; ++c) {
                    interleavedBlock[f * channels + c] = tempChannelBuffers_[c][f];
                }
            }

            while (dspRunning_.load(std::memory_order_relaxed) && !pushAudioBlock(interleavedBlock.data())) {
                // Busy wait to maintain real-time constraints (no sleeping)
            }
        }
    });

    running_.store(true);
    renderThread_ = std::thread([this, bufferFrameCount, channelCount]() mutable {
        HANDLE hEvent = samplesReadyEvent_;
        const UINT32 bufferFrameCountLocal = bufferFrameCount;
        std::vector<float> tempBlock(static_cast<size_t>(engineBlockSize) * channelCount, 0.0f);

        while (running_.load()) {

            // Wait for WASAPI to request more data
            DWORD waitResult = WaitForSingleObject(hEvent, 200);
            if (waitResult != WAIT_OBJECT_0)
                continue;

            UINT32 padding = 0;
            if (FAILED(client_->GetCurrentPadding(&padding)))
                continue;

            UINT32 framesToWrite = bufferFrameCountLocal - padding;
            if (framesToWrite == 0)
                continue;

            BYTE* data = nullptr;
            if (FAILED(renderClient_->GetBuffer(framesToWrite, &data)))
                continue;

            float* interleavedOut = reinterpret_cast<float*>(data);
            const uint32_t channels = mixFormat_->nChannels;

            uint32_t framesRemaining = framesToWrite;
            size_t outputOffset = 0;
            bool blockAvailable = false;

            while (framesRemaining > 0) {
                if (!popAudioBlock(tempBlock.data())) {
                    std::fill(interleavedOut + outputOffset * channels,
                              interleavedOut + (outputOffset + framesRemaining) * channels, 0.0f);
                    break;
                }

                blockAvailable = true;
                const uint32_t framesFromBlock = std::min(engineBlockSize, framesRemaining);
                const size_t samplesToCopy = static_cast<size_t>(framesFromBlock) * channels;
                std::memcpy(interleavedOut + outputOffset * channels, tempBlock.data(),
                            samplesToCopy * sizeof(float));

                framesRemaining -= framesFromBlock;
                outputOffset += framesFromBlock;
            }

            if (!blockAvailable && framesRemaining == framesToWrite) {
                std::fill(interleavedOut, interleavedOut + framesToWrite * channels, 0.0f);
            }

            notifyCallbackExecuted();

            renderClient_->ReleaseBuffer(framesToWrite, blockAvailable ? 0 : AUDCLNT_BUFFERFLAGS_SILENT);
        }
    });
    return true;
}

void AudioDeviceHandler::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    running_.store(false);
    dspRunning_.store(false, std::memory_order_release);
    if (renderThread_.joinable())
        renderThread_.join();
    if (dspThread_.joinable())
        dspThread_.join();
    if (client_) {
        HRESULT hr = client_->Stop();
        if (FAILED(hr)) {
            logFailure(L"IAudioClient::Stop", hr);
        } else {
            logInfo(L"Audio client stopped");
        }
    }
    streamStarted_.store(false, std::memory_order_release);
}

HRESULT AudioDeviceHandler::currentPadding(UINT32* padding) const {
    if (!padding) {
        return E_POINTER;
    }
    *padding = 0;
    if (!client_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    return client_->GetCurrentPadding(padding);
}

HRESULT AudioDeviceHandler::getBuffer(UINT32 frameCount, BYTE** data) {
    if (!data) {
        return E_POINTER;
    }
    *data = nullptr;
    if (!renderClient_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    if (bufferPendingRelease_) {
        std::wstringstream warning;
        warning << L"Render buffer requested while a previous buffer is still pending release ("
                << activeRenderFrameCount_ << L" frames). Releasing outstanding buffer.";
        logInfo(warning.str());
        HRESULT flushResult = renderClient_->ReleaseBuffer(activeRenderFrameCount_, AUDCLNT_BUFFERFLAGS_SILENT);
        if (FAILED(flushResult)) {
            logFailure(L"IAudioRenderClient::ReleaseBuffer (pending)", flushResult);
        }
        bufferPendingRelease_ = false;
        activeRenderBuffer_ = nullptr;
        activeRenderFrameCount_ = 0;
        activeRenderBufferSizeBytes_ = 0;
    }
    if (!streamStarted_.load(std::memory_order_acquire)) {
        logInfo(L"Render buffer requested but the audio stream has not been started."
                L" Ensure AudioDeviceHandler::start() has been called.");
    }
    HRESULT hr = renderClient_->GetBuffer(frameCount, data);
    if (SUCCEEDED(hr)) {
        activeRenderBuffer_ = *data;
        activeRenderFrameCount_ = frameCount;
        UINT32 frameBytes = 0;
        if (mixFormat_) {
            frameBytes = mixFormat_->nBlockAlign;
            if (frameBytes == 0 && mixFormat_->nChannels > 0) {
                frameBytes = (mixFormat_->wBitsPerSample / 8) * mixFormat_->nChannels;
            }
        }
        activeRenderBufferSizeBytes_ = frameBytes * frameCount;
        bufferPendingRelease_ = true;
    }
    return hr;
}

void AudioDeviceHandler::releaseBuffer(UINT32 frameCount) {
    if (renderClient_) {
        if (!bufferPendingRelease_) {
            logInfo(L"ReleaseBuffer called without an active render buffer; ignoring request.");
            return;
        }
        UINT32 framesToRelease = frameCount;
        if (frameCount > activeRenderFrameCount_) {
            std::wstringstream warning;
            warning << L"Requested to release " << frameCount
                    << L" frames, but only " << activeRenderFrameCount_
                    << L" frames were acquired. Clamping to acquired size.";
            logInfo(warning.str());
            framesToRelease = activeRenderFrameCount_;
        }
        UINT32 bytesPerFrame = 0;
        if (mixFormat_) {
            bytesPerFrame = mixFormat_->nBlockAlign;
            if (bytesPerFrame == 0 && mixFormat_->nChannels > 0) {
                bytesPerFrame = (mixFormat_->wBitsPerSample / 8) * mixFormat_->nChannels;
            }
        }
        UINT32 calculatedBytes = bytesPerFrame * framesToRelease;
        if (activeRenderBufferSizeBytes_ != 0 && calculatedBytes != activeRenderBufferSizeBytes_) {
            std::wstringstream warning;
            warning << L"ReleaseBuffer byte count mismatch: expected " << activeRenderBufferSizeBytes_
                    << L" bytes but calculated " << calculatedBytes << L" bytes based on format.";
            logInfo(warning.str());
        }
        HRESULT hr = renderClient_->ReleaseBuffer(framesToRelease, 0);
        if (FAILED(hr)) {
            logFailure(L"IAudioRenderClient::ReleaseBuffer", hr);
        }
        bufferPendingRelease_ = false;
        activeRenderBuffer_ = nullptr;
        activeRenderFrameCount_ = 0;
        activeRenderBufferSizeBytes_ = 0;
    }
}

std::vector<AudioDeviceHandler::DeviceInfo> AudioDeviceHandler::enumerateRenderDevices() {
    std::vector<DeviceInfo> devices;

    IMMDeviceEnumerator* enumerator = nullptr;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), NULL, CLSCTX_ALL,
                                  __uuidof(IMMDeviceEnumerator), (void**)&enumerator);
    if (FAILED(hr)) {
        logFailure(L"CoCreateInstance(IMMDeviceEnumerator)", hr);
        return devices;
    }

    IMMDeviceCollection* collection = nullptr;
    hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr)) {
        logFailure(L"IMMDeviceEnumerator::EnumAudioEndpoints", hr);
        enumerator->Release();
        return devices;
    }

    UINT count = 0;
    collection->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        IMMDevice* device = nullptr;
        hr = collection->Item(i, &device);
        if (FAILED(hr) || !device) {
            continue;
        }

        DeviceInfo info;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id)) && id) {
            info.id = id;
            CoTaskMemFree(id);
        }

        IPropertyStore* propertyStore = nullptr;
        hr = device->OpenPropertyStore(STGM_READ, &propertyStore);
        if (SUCCEEDED(hr) && propertyStore) {
            PROPVARIANT varName;
            PropVariantInit(&varName);
            if (SUCCEEDED(propertyStore->GetValue(PKEY_Device_FriendlyName, &varName))) {
                if (varName.vt == VT_LPWSTR && varName.pwszVal) {
                    info.name = varName.pwszVal;
                }
            }
            PropVariantClear(&varName);
            propertyStore->Release();
        }

        if (info.name.empty()) {
            info.name = L"Audio Device";
        }

        devices.push_back(std::move(info));
        device->Release();
    }

    collection->Release();
    enumerator->Release();
    return devices;
}

#else  // !(_WIN32 || _MSC_VER || __MINGW32__)

AudioDeviceHandler::AudioDeviceHandler() = default;

AudioDeviceHandler::~AudioDeviceHandler() {
    shutdown();
}

void AudioDeviceHandler::registerStreamCallback(AudioStreamCallback callback, void* userData) {
    callback_ = callback;
    callbackContext_ = userData;
    callbackInvoked_.store(false, std::memory_order_relaxed);
}

AudioDeviceHandler::AudioStreamCallback AudioDeviceHandler::streamCallback() const {
    return callback_;
}

void* AudioDeviceHandler::streamCallbackContext() const {
    return callbackContext_;
}

void AudioDeviceHandler::notifyCallbackExecuted() {
    callbackInvoked_.store(true, std::memory_order_release);
}

void AudioDeviceHandler::resetCallbackMonitor() {
    streamStarted_.store(false, std::memory_order_release);
    callbackInvoked_.store(false, std::memory_order_release);
}

bool AudioDeviceHandler::streamStartedSuccessfully() {
    return streamStarted_.load(std::memory_order_acquire);
}

bool AudioDeviceHandler::callbackHasFired() {
    return callbackInvoked_.load(std::memory_order_acquire);
}

void AudioDeviceHandler::FormatDeleter::operator()(WAVEFORMATEX* format) const {
    (void)format;
}

void AudioDeviceHandler::resetComObjectsLocked() {
    enumerator_ = nullptr;
    device_ = nullptr;
    client_ = nullptr;
    renderClient_ = nullptr;
}

void AudioDeviceHandler::resetStateLocked() {
    resetComObjectsLocked();
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
    activeRenderBuffer_ = nullptr;
    activeRenderFrameCount_ = 0;
    activeRenderBufferSizeBytes_ = 0;
    bufferPendingRelease_ = false;
}

bool AudioDeviceHandler::initialize(const std::wstring& deviceId) {
    deviceId_ = deviceId;
    deviceName_.clear();
    initialized_ = false;
    bufferFrameCount_ = 0;
    mixFormat_.reset();
    resetComObjectsLocked();
    return false;
}

bool AudioDeviceHandler::isInitializing() const {
    return false;
}

bool AudioDeviceHandler::runInitialization(const std::wstring& /*deviceId*/) {
    return false;
}

void AudioDeviceHandler::shutdown() {
    resetStateLocked();
}

bool AudioDeviceHandler::start() {
    return false;
}

void AudioDeviceHandler::stop() {}

HRESULT AudioDeviceHandler::currentPadding(UINT32* padding) const {
    if (padding) {
        *padding = 0;
    }
    return static_cast<HRESULT>(0);
}

HRESULT AudioDeviceHandler::getBuffer(UINT32 /*frameCount*/, BYTE** data) {
    if (data) {
        *data = nullptr;
    }
    bufferPendingRelease_ = false;
    activeRenderBuffer_ = nullptr;
    activeRenderFrameCount_ = 0;
    activeRenderBufferSizeBytes_ = 0;
    return static_cast<HRESULT>(0);
}

void AudioDeviceHandler::releaseBuffer(UINT32 /*frameCount*/) {
    bufferPendingRelease_ = false;
    activeRenderBuffer_ = nullptr;
    activeRenderFrameCount_ = 0;
    activeRenderBufferSizeBytes_ = 0;
}

std::vector<AudioDeviceHandler::DeviceInfo> AudioDeviceHandler::enumerateRenderDevices() {
    return {};
}

#endif  // defined(_WIN32) || defined(_MSC_VER) || defined(__MINGW32__)

