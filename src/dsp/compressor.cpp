#include "dsp/compressor.h"

#include <algorithm>
#include <cmath>

namespace audiolens::dsp {
namespace {

constexpr double kMinDb = -120.0;
constexpr double kMinMeanSquare = 1e-12;  // -120 dB, the floor the curve treats as silence.

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

/// One-pole coefficient for an envelope that reaches ~63 % of its target in
/// `timeMs`. A zero or negative time yields 0, i.e. an instantaneous jump.
double onePoleCoeff(double timeMs, double sampleRate) {
    if (timeMs <= 0.0) {
        return 0.0;
    }
    return std::exp(-1.0 / (timeMs * 0.001 * sampleRate));
}

}  // namespace

void Compressor::prepare(double sampleRate) noexcept {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    detectorCoeff_ = 1.0 - onePoleCoeff(kDetectorMs, sampleRate_);
    updateCoefficients();
    reset();
}

void Compressor::reset() noexcept {
    smoothedGainDb_ = 0.0;
    meanSquare_ = 0.0;
}

void Compressor::setSettings(const CompressorSettings& settings) noexcept {
    settings_ = settings;
    settings_.ratio = std::max(1.0, settings_.ratio);
    settings_.kneeDb = std::max(0.0, settings_.kneeDb);
    updateCoefficients();
}

void Compressor::updateCoefficients() noexcept {
    attackCoeff_ = onePoleCoeff(settings_.attackMs, sampleRate_);
    releaseCoeff_ = onePoleCoeff(settings_.releaseMs, sampleRate_);

    // Undo, at the reference level, exactly what the curve takes away there.
    makeupDb_ = -staticGainDb(settings_.makeupReferenceDb);
}

double Compressor::staticGainDb(double inputDb) const noexcept {
    const double overshoot = inputDb - settings_.thresholdDb;
    const double knee = settings_.kneeDb;

    double outputDb;
    if (knee > 0.0 && 2.0 * std::abs(overshoot) <= knee) {
        // Quadratic interpolation across the knee, tangent to both straight
        // segments at its edges.
        const double t = overshoot + knee / 2.0;
        outputDb = inputDb + (1.0 / settings_.ratio - 1.0) * t * t / (2.0 * knee);
    } else if (overshoot <= 0.0) {
        outputDb = inputDb;
    } else {
        outputDb = settings_.thresholdDb + overshoot / settings_.ratio;
    }

    return outputDb - inputDb;
}

void Compressor::processFrame(float* frame, std::uint32_t channels) noexcept {
    // Channel-linked RMS detection: one level for all channels, so the gain
    // cannot pull the stereo image toward whichever side is louder.
    double sumOfSquares = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c) {
        const double sample = frame[c];
        sumOfSquares += sample * sample;
    }
    const double instantaneous = channels > 0 ? sumOfSquares / channels : 0.0;
    meanSquare_ += detectorCoeff_ * (instantaneous - meanSquare_);

    const double levelDb =
        meanSquare_ > kMinMeanSquare ? 10.0 * std::log10(meanSquare_) : kMinDb;
    const double targetGainDb = staticGainDb(levelDb);

    // More reduction follows the attack time, less follows the release time.
    const double coeff = targetGainDb < smoothedGainDb_ ? attackCoeff_ : releaseCoeff_;
    smoothedGainDb_ = targetGainDb + coeff * (smoothedGainDb_ - targetGainDb);

    const auto gain = static_cast<float>(dbToLinear(smoothedGainDb_ + makeupDb_));
    for (std::uint32_t c = 0; c < channels; ++c) {
        frame[c] *= gain;
    }
}

}  // namespace audiolens::dsp
