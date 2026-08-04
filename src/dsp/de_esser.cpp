#include "dsp/de_esser.h"

#include <algorithm>
#include <cmath>

namespace audiolens::dsp {
namespace {

/// Window the sibilance and broadband levels are measured over. Short enough
/// that a single /s/ registers, long enough not to follow the waveform itself.
constexpr double kDetectorMs = 3.0;

/// Stops the ratio blowing up during silence, where both terms go to zero.
constexpr double kFloor = 1e-12;

double onePoleCoeff(double ms, double sampleRate) {
    if (ms <= 0.0) return 0.0;
    return std::exp(-1.0 / (ms * 0.001 * sampleRate));
}

}  // namespace

void DeEsser::prepare(double sampleRate, std::uint32_t channels) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    channels_ = std::max<std::uint32_t>(1, channels);
    band_.assign(channels_, Biquad{});
    detectorCoeff_ = onePoleCoeff(kDetectorMs, sampleRate_);
    updateCoefficients();
    reset();
}

void DeEsser::reset() noexcept {
    for (Biquad& filter : band_) filter.reset();
    bandMeanSquare_ = 0.0;
    fullMeanSquare_ = 0.0;
    smoothedReductionDb_ = 0.0;
}

void DeEsser::setSettings(const DeEsserSettings& settings) noexcept {
    const bool filterChanged = settings.frequencyHz != settings_.frequencyHz ||
                               settings.q != settings_.q;
    settings_ = settings;
    attackCoeff_ = onePoleCoeff(settings_.attackMs, sampleRate_);
    releaseCoeff_ = onePoleCoeff(settings_.releaseMs, sampleRate_);
    if (filterChanged) {
        updateCoefficients();
    }
}

void DeEsser::updateCoefficients() noexcept {
    // Kept below Nyquist with room to spare: at 44.1 kHz a 6.5 kHz bandpass is
    // fine, but a preset asking for something much higher would otherwise
    // design a filter the rate cannot represent.
    const double freq = std::clamp(settings_.frequencyHz, 1000.0, sampleRate_ * 0.45);
    const BiquadCoeffs coeffs = designBandpass(freq, settings_.q, sampleRate_);
    for (Biquad& filter : band_) {
        filter.setCoeffs(coeffs);
    }
}

void DeEsser::processFrame(float* frame, std::uint32_t channels) noexcept {
    const std::uint32_t active = std::min(channels, channels_);
    if (!settings_.enabled || active == 0) {
        // Let any held reduction fall away rather than vanish, so switching the
        // stage off does not step the top end.
        smoothedReductionDb_ *= releaseCoeff_;
        return;
    }

    // The band is extracted first: the same samples serve as the detector's
    // input and as what gets subtracted, so the filter runs once.
    double bandSquare = 0.0;
    double fullSquare = 0.0;
    float banded[8];  // more channels than the chain ever runs
    const std::uint32_t kept = std::min<std::uint32_t>(active, 8);

    for (std::uint32_t c = 0; c < kept; ++c) {
        const float b = band_[c].process(frame[c]);
        banded[c] = b;
        bandSquare += static_cast<double>(b) * b;
        fullSquare += static_cast<double>(frame[c]) * frame[c];
    }

    bandMeanSquare_ = bandSquare + detectorCoeff_ * (bandMeanSquare_ - bandSquare);
    fullMeanSquare_ = fullSquare + detectorCoeff_ * (fullMeanSquare_ - fullSquare);

    // How much of the signal's energy is in the sibilant band, in dB. A vowel
    // puts almost none there; an /s/ puts most of it there.
    const double ratioDb =
        10.0 * std::log10((bandMeanSquare_ + kFloor) / (fullMeanSquare_ + kFloor));

    const double excess = ratioDb - settings_.thresholdDb;
    const double target =
        excess > 0.0 ? std::min(excess * settings_.amount, settings_.maxReductionDb) : 0.0;

    const double coeff = target > smoothedReductionDb_ ? attackCoeff_ : releaseCoeff_;
    smoothedReductionDb_ = target + coeff * (smoothedReductionDb_ - target);

    if (smoothedReductionDb_ <= 0.001) {
        return;
    }

    // `1 - 10^(-r/20)` is the fraction of the band to remove for r dB of
    // reduction: at the centre frequency the bandpass passes the input
    // unchanged, so subtracting that fraction of it leaves exactly `10^(-r/20)`
    // of the original behind.
    const auto g = static_cast<float>(1.0 - std::pow(10.0, -smoothedReductionDb_ / 20.0));
    for (std::uint32_t c = 0; c < kept; ++c) {
        frame[c] -= g * banded[c];
    }
}

}  // namespace audiolens::dsp
