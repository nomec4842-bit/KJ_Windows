#include "core/effects/delay_effect.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kDefaultSampleRate = 44100.0;

std::size_t computeRequiredSamples(double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
    double maxDelaySeconds = DelayEffect::kMaxDelayTimeMs * 0.001;
    auto required = static_cast<std::size_t>(std::ceil(sr * maxDelaySeconds)) + 1;
    return std::max<std::size_t>(required, 1);
}
}

DelayEffect::DelayEffect(double sampleRate)
    : m_sampleRate(sampleRate > 0.0 ? sampleRate : kDefaultSampleRate)
    , m_delayTimeMs(kDefaultDelayTimeMs)
    , m_delaySamples(0)
    , m_feedback(kDefaultDelayFeedback)
    , m_mix(kDefaultDelayMix)
    , m_bufferLeft(computeRequiredSamples(m_sampleRate), 0.0f)
    , m_bufferRight(computeRequiredSamples(m_sampleRate), 0.0f)
    , m_writeIndex(0)
{
    updateDelaySamples();
}

void DelayEffect::setSampleRate(double sampleRate)
{
    double sr = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
    if (std::abs(m_sampleRate - sr) < 1e-6)
        return;

    m_sampleRate = sr;
    resizeBuffer(computeRequiredSamples(m_sampleRate));
    updateDelaySamples();
}

void DelayEffect::setDelayTime(float milliseconds)
{
    m_delayTimeMs = std::clamp(milliseconds, kMinDelayTimeMs, kMaxDelayTimeMs);
    updateDelaySamples();
}

void DelayEffect::setFeedback(float value)
{
    m_feedback = std::clamp(value, kMinFeedback, kMaxFeedback);
}

void DelayEffect::setMix(float value)
{
    m_mix = std::clamp(value, kMinMix, kMaxMix);
}

void DelayEffect::reset()
{
    std::fill(m_bufferLeft.begin(), m_bufferLeft.end(), 0.0f);
    std::fill(m_bufferRight.begin(), m_bufferRight.end(), 0.0f);
    m_writeIndex = 0;
}

void DelayEffect::process(float* left, float* right, std::size_t frameCount)
{
    if (!left || !right || frameCount == 0)
        return;
    if (m_bufferLeft.empty() || m_bufferRight.empty())
        return;

    std::size_t bufferSize = m_bufferLeft.size();
    std::size_t currentDelay = std::min(m_delaySamples, bufferSize > 0 ? bufferSize - 1 : std::size_t{0});

    const float dryAmount = 1.0f - m_mix;
    const float wetAmount = m_mix;

    for (std::size_t i = 0; i < frameCount; ++i)
    {
        std::size_t readIndex = (m_writeIndex + bufferSize - currentDelay) % bufferSize;
        float delayedLeft = m_bufferLeft[readIndex];
        float delayedRight = m_bufferRight[readIndex];

        float inputLeft = left[i];
        float inputRight = right[i];

        m_bufferLeft[m_writeIndex] = inputLeft + delayedLeft * m_feedback;
        m_bufferRight[m_writeIndex] = inputRight + delayedRight * m_feedback;

        float wetLeft = delayedLeft;
        float wetRight = delayedRight;

        left[i] = std::clamp(inputLeft * dryAmount + wetLeft * wetAmount, -1.0f, 1.0f);
        right[i] = std::clamp(inputRight * dryAmount + wetRight * wetAmount, -1.0f, 1.0f);

        m_writeIndex = (m_writeIndex + 1) % bufferSize;
    }
}

void DelayEffect::resizeBuffer(std::size_t requiredSamples)
{
    if (requiredSamples < 1)
        requiredSamples = 1;
    m_bufferLeft.assign(requiredSamples, 0.0f);
    m_bufferRight.assign(requiredSamples, 0.0f);
    m_writeIndex = 0;
}

void DelayEffect::updateDelaySamples()
{
    double sr = m_sampleRate > 0.0 ? m_sampleRate : kDefaultSampleRate;
    double delaySeconds = std::clamp(m_delayTimeMs, kMinDelayTimeMs, kMaxDelayTimeMs) * 0.001;
    auto samples = static_cast<std::size_t>(std::round(delaySeconds * sr));
    if (samples >= m_bufferLeft.size())
    {
        resizeBuffer(computeRequiredSamples(sr));
        samples = std::min(samples, m_bufferLeft.empty() ? std::size_t{0} : m_bufferLeft.size() - 1);
    }
    m_delaySamples = samples;
}
