#pragma once

namespace audiolens::dsp {

/// Second-order section coefficients, already normalized so that a0 == 1.
struct BiquadCoeffs {
    double b0 = 1.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// Filter designs follow the Audio EQ Cookbook (Robert Bristow-Johnson).
// `freqHz` is clamped below Nyquist, so a caller sweeping a cutoff upward
// cannot produce an unstable section.

BiquadCoeffs designHighpass(double freqHz, double q, double sampleRate);
BiquadCoeffs designLowShelf(double freqHz, double gainDb, double q, double sampleRate);
BiquadCoeffs designPeaking(double freqHz, double gainDb, double q, double sampleRate);
BiquadCoeffs designHighShelf(double freqHz, double gainDb, double q, double sampleRate);

/// Constant-skirt-gain bandpass, used by the analysis code to measure the
/// energy in a band without pulling in an FFT.
BiquadCoeffs designBandpass(double freqHz, double q, double sampleRate);

/// Magnitude of the transfer function at `freqHz`, in linear gain.
/// Lets tests assert on a filter's response without running signal through it.
double magnitudeAt(const BiquadCoeffs& coeffs, double freqHz, double sampleRate);

/// One second-order section in transposed direct form II.
///
/// State is kept in double precision: at a 30 Hz cutoff against a 48 kHz rate
/// the pole radius is close enough to 1 that single-precision state accumulates
/// audible error.
class Biquad {
public:
    void setCoeffs(const BiquadCoeffs& coeffs) noexcept { coeffs_ = coeffs; }
    const BiquadCoeffs& coeffs() const noexcept { return coeffs_; }

    void reset() noexcept {
        z1_ = 0.0;
        z2_ = 0.0;
    }

    float process(float input) noexcept {
        const double x = static_cast<double>(input);
        const double y = coeffs_.b0 * x + z1_;
        z1_ = coeffs_.b1 * x - coeffs_.a1 * y + z2_;
        z2_ = coeffs_.b2 * x - coeffs_.a2 * y;
        return static_cast<float>(y);
    }

private:
    BiquadCoeffs coeffs_;
    double z1_ = 0.0;
    double z2_ = 0.0;
};

}  // namespace audiolens::dsp
