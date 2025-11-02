#pragma once

class SidechainProcessor
{
public:
    SidechainProcessor();

    void setEnabled(bool enabled);
    void setSourceTrackId(int trackId);
    void setAmount(double amount);
    void setAttack(double seconds);
    void setRelease(double seconds);

    [[nodiscard]] bool enabled() const noexcept;
    [[nodiscard]] int sourceTrackId() const noexcept;
    [[nodiscard]] double amount() const noexcept;
    [[nodiscard]] double attack() const noexcept;
    [[nodiscard]] double release() const noexcept;

    [[nodiscard]] double envelopeValue() const noexcept;
    void resetEnvelope();

    [[nodiscard]] double detectorLevel() const noexcept;
    void setDetectorLevel(double level);
    void resetDetector();

    void reset();

    double computeGain(double sourceLevel, double sampleRate);

private:
    double computeEnvelope(double targetValue, double sampleRate);

    bool m_enabled;
    int m_sourceTrackId;
    double m_amount;
    double m_attack;
    double m_release;
    double m_envelopeValue;
    double m_detectorLevel;
};
