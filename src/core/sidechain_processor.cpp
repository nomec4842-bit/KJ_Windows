#include "core/effects/sidechain_processor.h"

#include <algorithm>
#include <cmath>

namespace
{
constexpr double kDefaultAmount = 1.0;
constexpr double kDefaultAttack = 0.01;
constexpr double kDefaultRelease = 0.3;
constexpr double kMinAmount = 0.0;
constexpr double kMaxAmount = 1.0;
constexpr double kMinTime = 0.0;
constexpr double kMaxTime = 4.0;
constexpr double kDefaultSampleRate = 44100.0;

double clamp01(double value)
{
    return std::clamp(value, 0.0, 1.0);
}

double clampTime(double value)
{
    if (!std::isfinite(value))
        return kMinTime;
    if (value < kMinTime)
        return kMinTime;
    if (value > kMaxTime)
        return kMaxTime;
    return value;
}
} // namespace

SidechainProcessor::SidechainProcessor()
    : m_enabled(false)
    , m_sourceTrackId(-1)
    , m_amount(kDefaultAmount)
    , m_attack(kDefaultAttack)
    , m_release(kDefaultRelease)
    , m_envelopeValue(0.0)
    , m_detectorLevel(0.0)
{
}

void SidechainProcessor::setEnabled(bool enabled)
{
    if (m_enabled == enabled)
        return;

    m_enabled = enabled;
    if (!m_enabled)
        resetEnvelope();
}

void SidechainProcessor::setSourceTrackId(int trackId)
{
    m_sourceTrackId = trackId;
}

void SidechainProcessor::setAmount(double amount)
{
    if (!std::isfinite(amount))
        amount = kDefaultAmount;
    m_amount = std::clamp(amount, kMinAmount, kMaxAmount);
}

void SidechainProcessor::setAttack(double seconds)
{
    m_attack = clampTime(seconds);
}

void SidechainProcessor::setRelease(double seconds)
{
    m_release = clampTime(seconds);
}

bool SidechainProcessor::enabled() const noexcept
{
    return m_enabled;
}

int SidechainProcessor::sourceTrackId() const noexcept
{
    return m_sourceTrackId;
}

double SidechainProcessor::amount() const noexcept
{
    return m_amount;
}

double SidechainProcessor::attack() const noexcept
{
    return m_attack;
}

double SidechainProcessor::release() const noexcept
{
    return m_release;
}

double SidechainProcessor::envelopeValue() const noexcept
{
    return m_envelopeValue;
}

void SidechainProcessor::resetEnvelope()
{
    m_envelopeValue = 0.0;
}

double SidechainProcessor::detectorLevel() const noexcept
{
    return m_detectorLevel;
}

void SidechainProcessor::setDetectorLevel(double level)
{
    if (!std::isfinite(level))
        level = 0.0;
    m_detectorLevel = clamp01(level);
}

void SidechainProcessor::resetDetector()
{
    m_detectorLevel = 0.0;
}

void SidechainProcessor::reset()
{
    resetEnvelope();
    resetDetector();
}

double SidechainProcessor::computeGain(double sourceLevel, double sampleRate)
{
    if (!m_enabled)
    {
        resetEnvelope();
        return 1.0;
    }

    double sr = sampleRate > 0.0 ? sampleRate : kDefaultSampleRate;
    double target = clamp01(std::isfinite(sourceLevel) ? sourceLevel : 0.0);
    m_envelopeValue = computeEnvelope(target, sr);

    double depth = clamp01(m_amount);
    double gain = 1.0 - depth * m_envelopeValue;
    return std::clamp(gain, 0.0, 1.0);
}

double SidechainProcessor::computeEnvelope(double targetValue, double sampleRate)
{
    double target = clamp01(targetValue);
    auto coefficientForTime = [&](double timeSeconds) {
        if (timeSeconds <= 0.0)
            return 0.0;
        double samples = std::max(timeSeconds * sampleRate, 1.0);
        return std::exp(-1.0 / samples);
    };

    double coeff = (target > m_envelopeValue) ? coefficientForTime(m_attack)
                                              : coefficientForTime(m_release);
    double next = target + (m_envelopeValue - target) * coeff;
    if (!std::isfinite(next))
        next = target;
    if (next < 0.0)
        next = 0.0;
    if (next > 1.0)
        next = 1.0;
    return next;
}
