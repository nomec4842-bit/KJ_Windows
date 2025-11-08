#include "core/audio_device_handler.h"

std::atomic<bool> AudioDeviceHandler::streamStarted_{false};
std::atomic<bool> AudioDeviceHandler::callbackInvoked_{false};

#if defined(_WIN32) || defined(_MSC_VER) || defined(__MINGW32__)

#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <iomanip>
#include <sstream>

namespace {
constexpr DWORD kStreamFlags = 0;
constexpr REFERENCE_TIME kBufferDuration = 10000000; // 1 second

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

AudioDeviceHandler::AudioDeviceHandler() = default;

AudioDeviceHandler::~AudioDeviceHandler() {
    shutdown();
}

void AudioDeviceHandler::registerStreamCallback(AudioStreamCallback callback, void* userData) {
    std::lock_guard<std::mutex> lock(stateMutex_);
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
    std::lock_guard<std::mutex> lock(stateMutex_);
    return callback_;
}

void* AudioDeviceHandler::streamCallbackContext() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
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
    mixFormat_.reset();
    bufferFrameCount_ = 0;
    initialized_ = false;
    deviceId_.clear();
    deviceName_.clear();
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
    return false;
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
            mixFormat->nSamplesPerSec = 44100;
            mixFormat->nBlockAlign = (mixFormat->wBitsPerSample / 8) * mixFormat->nChannels;
            mixFormat->nAvgBytesPerSec = mixFormat->nSamplesPerSec * mixFormat->nBlockAlign;
        }

        hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, kStreamFlags, kBufferDuration, 0,
                                 mixFormat, NULL);
        if (FAILED(hr)) {
            logFailure(L"IAudioClient::Initialize", hr);
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
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (initThreadActive_ && !initCompleted_) {
        logInfo(L"Audio client start requested while initialization is still running");
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
    HRESULT hr = client_->Start();
    if (FAILED(hr)) {
        logFailure(L"IAudioClient::Start", hr);
        return false;
    }
    logInfo(L"Audio client started successfully");
    streamStarted_.store(true, std::memory_order_release);
    return true;
}

void AudioDeviceHandler::stop() {
    std::lock_guard<std::mutex> lock(stateMutex_);
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
    std::lock_guard<std::mutex> lock(stateMutex_);
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
    std::lock_guard<std::mutex> lock(stateMutex_);
    *data = nullptr;
    if (!renderClient_) {
        return AUDCLNT_E_NOT_INITIALIZED;
    }
    return renderClient_->GetBuffer(frameCount, data);
}

void AudioDeviceHandler::releaseBuffer(UINT32 frameCount) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (renderClient_) {
        renderClient_->ReleaseBuffer(frameCount, 0);
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
    std::lock_guard<std::mutex> lock(stateMutex_);
    callback_ = callback;
    callbackContext_ = userData;
    callbackInvoked_.store(false, std::memory_order_relaxed);
}

AudioDeviceHandler::AudioStreamCallback AudioDeviceHandler::streamCallback() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return callback_;
}

void* AudioDeviceHandler::streamCallbackContext() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
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
    return static_cast<HRESULT>(0);
}

void AudioDeviceHandler::releaseBuffer(UINT32 /*frameCount*/) {}

std::vector<AudioDeviceHandler::DeviceInfo> AudioDeviceHandler::enumerateRenderDevices() {
    return {};
}

#endif  // defined(_WIN32) || defined(_MSC_VER) || defined(__MINGW32__)

