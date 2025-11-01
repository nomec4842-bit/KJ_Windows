#include "core/effects/effect_plugin.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <iterator>
#include <vector>

namespace
{
constexpr float kDefaultDelayTimeMs = 350.0f;
constexpr float kDefaultDelayFeedback = 0.35f;
constexpr float kDefaultDelayMix = 0.4f;
constexpr float kMinDelayTimeMs = 10.0f;
constexpr float kMaxDelayTimeMs = 2000.0f;
constexpr float kMinDelayFeedback = 0.0f;
constexpr float kMaxDelayFeedback = 0.95f;
constexpr float kMinDelayMix = 0.0f;
constexpr float kMaxDelayMix = 1.0f;

struct DelayEffect
{
    explicit DelayEffect(double sr)
        : sampleRate(sr > 0.0 ? sr : 44100.0)
    {
        resizeBuffer();
        setDelayTime(kDefaultDelayTimeMs);
        setFeedback(kDefaultDelayFeedback);
        setMix(kDefaultDelayMix);
    }

    void resizeBuffer()
    {
        std::size_t requiredSamples = static_cast<std::size_t>(std::ceil(kMaxDelayTimeMs * 0.001 * sampleRate)) + 1;
        if (requiredSamples < 1)
            requiredSamples = 1;
        bufferLeft.assign(requiredSamples, 0.0f);
        bufferRight.assign(requiredSamples, 0.0f);
        writeIndex = 0;
    }

    void reset()
    {
        std::fill(bufferLeft.begin(), bufferLeft.end(), 0.0f);
        std::fill(bufferRight.begin(), bufferRight.end(), 0.0f);
        writeIndex = 0;
    }

    void setDelayTime(float ms)
    {
        delayTimeMs = std::clamp(ms, kMinDelayTimeMs, kMaxDelayTimeMs);
        delaySamples = static_cast<std::size_t>(std::round(delayTimeMs * 0.001 * sampleRate));
        if (delaySamples >= bufferLeft.size())
        {
            resizeBuffer();
            delaySamples = std::min(delaySamples, bufferLeft.empty() ? std::size_t{0} : bufferLeft.size() - 1);
        }
    }

    void setFeedback(float value)
    {
        feedback = std::clamp(value, kMinDelayFeedback, kMaxDelayFeedback);
    }

    void setMix(float value)
    {
        mix = std::clamp(value, kMinDelayMix, kMaxDelayMix);
    }

    void process(float* left, float* right, std::size_t frameCount)
    {
        if (!left || !right || frameCount == 0 || bufferLeft.empty() || bufferRight.empty())
            return;

        std::size_t bufferSize = bufferLeft.size();
        if (bufferSize == 0)
            return;

        std::size_t currentDelay = std::min(delaySamples, bufferSize - 1);

        for (std::size_t i = 0; i < frameCount; ++i)
        {
            std::size_t readIndex = (writeIndex + bufferSize - currentDelay) % bufferSize;
            float delayedLeft = bufferLeft[readIndex];
            float delayedRight = bufferRight[readIndex];

            float inputLeft = left[i];
            float inputRight = right[i];

            float wetLeft = delayedLeft;
            float wetRight = delayedRight;

            bufferLeft[writeIndex] = inputLeft + delayedLeft * feedback;
            bufferRight[writeIndex] = inputRight + delayedRight * feedback;

            float dryAmount = 1.0f - mix;
            float wetAmount = mix;
            left[i] = inputLeft * dryAmount + wetLeft * wetAmount;
            right[i] = inputRight * dryAmount + wetRight * wetAmount;

            writeIndex = (writeIndex + 1) % bufferSize;
        }
    }

    double sampleRate = 44100.0;
    float delayTimeMs = kDefaultDelayTimeMs;
    std::size_t delaySamples = 0;
    float feedback = kDefaultDelayFeedback;
    float mix = kDefaultDelayMix;
    std::vector<float> bufferLeft;
    std::vector<float> bufferRight;
    std::size_t writeIndex = 0;
};

const char* const kDelayTimeParamId = "time_ms";
const char* const kDelayFeedbackParamId = "feedback";
const char* const kDelayMixParamId = "mix";

EffectParameterInfo gDelayParameters[] = {
    {kDelayTimeParamId, "Time", kMinDelayTimeMs, kMaxDelayTimeMs, kDefaultDelayTimeMs},
    {kDelayFeedbackParamId, "Feedback", kMinDelayFeedback, kMaxDelayFeedback, kDefaultDelayFeedback},
    {kDelayMixParamId, "Mix", kMinDelayMix, kMaxDelayMix, kDefaultDelayMix},
};

void* createDelayInstance(double sampleRate)
{
    return new DelayEffect(sampleRate);
}

void destroyDelayInstance(void* instance)
{
    delete static_cast<DelayEffect*>(instance);
}

void setDelayParameter(void* instance, const char* parameterId, float value)
{
    if (!instance || !parameterId)
        return;

    auto* delay = static_cast<DelayEffect*>(instance);
    if (std::strcmp(parameterId, kDelayTimeParamId) == 0)
    {
        delay->setDelayTime(value);
    }
    else if (std::strcmp(parameterId, kDelayFeedbackParamId) == 0)
    {
        delay->setFeedback(value);
    }
    else if (std::strcmp(parameterId, kDelayMixParamId) == 0)
    {
        delay->setMix(value);
    }
}

void processDelay(void* instance, float* left, float* right, std::size_t frameCount)
{
    if (!instance)
        return;
    auto* delay = static_cast<DelayEffect*>(instance);
    delay->process(left, right, frameCount);
}

void resetDelay(void* instance)
{
    if (!instance)
        return;
    auto* delay = static_cast<DelayEffect*>(instance);
    delay->reset();
}

EffectDescriptor gDelayDescriptor {
    "kj.delay",
    "Stereo Delay",
    std::size_t(std::size(gDelayParameters)),
    gDelayParameters,
    &createDelayInstance,
    &destroyDelayInstance,
    &setDelayParameter,
    &processDelay,
    &resetDelay,
};

} // namespace

extern "C" __declspec(dllexport) const EffectDescriptor* getEffectDescriptor()
{
    return &gDelayDescriptor;
}
