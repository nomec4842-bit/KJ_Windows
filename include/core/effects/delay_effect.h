#pragma once

#include <cstddef>
#include <vector>

class DelayEffect
{
public:
    static constexpr float kMinDelayTimeMs = 10.0f;
    static constexpr float kMaxDelayTimeMs = 2000.0f;
    static constexpr float kMinFeedback = 0.0f;
    static constexpr float kMaxFeedback = 0.95f;
    static constexpr float kMinMix = 0.0f;
    static constexpr float kMaxMix = 1.0f;
    static constexpr float kDefaultDelayTimeMs = 350.0f;
    static constexpr float kDefaultDelayFeedback = 0.35f;
    static constexpr float kDefaultDelayMix = 0.4f;

    explicit DelayEffect(double sampleRate = 44100.0);

    void setSampleRate(double sampleRate);
    void setDelayTime(float milliseconds);
    void setFeedback(float value);
    void setMix(float value);

    void reset();
    void process(float* left, float* right, std::size_t frameCount);

    [[nodiscard]] double sampleRate() const noexcept { return m_sampleRate; }
    [[nodiscard]] float delayTimeMs() const noexcept { return m_delayTimeMs; }
    [[nodiscard]] float feedback() const noexcept { return m_feedback; }
    [[nodiscard]] float mix() const noexcept { return m_mix; }

private:
    void resizeBuffer(std::size_t requiredSamples);
    void updateDelaySamples();

    double m_sampleRate;
    float m_delayTimeMs;
    std::size_t m_delaySamples;
    float m_feedback;
    float m_mix;
    std::vector<float> m_bufferLeft;
    std::vector<float> m_bufferRight;
    std::size_t m_writeIndex;
};
