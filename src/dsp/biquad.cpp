#include "dsp/biquad.h"

#include <algorithm>
#include <cmath>
#include <complex>
#include <numbers>

namespace audiolens::dsp {
namespace {

/// Keeps the design frequency below Nyquist. Sliders that sweep a cutoff toward
/// the top of the range would otherwise produce a section with poles outside
/// the unit circle.
double clampFrequency(double freqHz, double sampleRate) {
    const double maximum = sampleRate * 0.49;
    return std::clamp(freqHz, 1.0, maximum);
}

double clampQ(double q) { return std::max(q, 0.01); }

}  // namespace

BiquadCoeffs designHighpass(double freqHz, double q, double sampleRate) {
    const double w0 = 2.0 * std::numbers::pi * clampFrequency(freqHz, sampleRate) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * clampQ(q));

    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = ((1.0 + cosW0) / 2.0) / a0;
    c.b1 = (-(1.0 + cosW0)) / a0;
    c.b2 = ((1.0 + cosW0) / 2.0) / a0;
    c.a1 = (-2.0 * cosW0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

BiquadCoeffs designLowShelf(double freqHz, double gainDb, double q, double sampleRate) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * clampFrequency(freqHz, sampleRate) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * clampQ(q));
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double a0 = (A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha;
    BiquadCoeffs c;
    c.b0 = (A * ((A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha)) / a0;
    c.b1 = (2.0 * A * ((A - 1.0) - (A + 1.0) * cosW0)) / a0;
    c.b2 = (A * ((A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha)) / a0;
    c.a1 = (-2.0 * ((A - 1.0) + (A + 1.0) * cosW0)) / a0;
    c.a2 = ((A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
    return c;
}

BiquadCoeffs designPeaking(double freqHz, double gainDb, double q, double sampleRate) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * clampFrequency(freqHz, sampleRate) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * clampQ(q));

    const double a0 = 1.0 + alpha / A;
    BiquadCoeffs c;
    c.b0 = (1.0 + alpha * A) / a0;
    c.b1 = (-2.0 * cosW0) / a0;
    c.b2 = (1.0 - alpha * A) / a0;
    c.a1 = (-2.0 * cosW0) / a0;
    c.a2 = (1.0 - alpha / A) / a0;
    return c;
}

BiquadCoeffs designHighShelf(double freqHz, double gainDb, double q, double sampleRate) {
    const double A = std::pow(10.0, gainDb / 40.0);
    const double w0 = 2.0 * std::numbers::pi * clampFrequency(freqHz, sampleRate) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double alpha = std::sin(w0) / (2.0 * clampQ(q));
    const double twoSqrtAAlpha = 2.0 * std::sqrt(A) * alpha;

    const double a0 = (A + 1.0) - (A - 1.0) * cosW0 + twoSqrtAAlpha;
    BiquadCoeffs c;
    c.b0 = (A * ((A + 1.0) + (A - 1.0) * cosW0 + twoSqrtAAlpha)) / a0;
    c.b1 = (-2.0 * A * ((A - 1.0) + (A + 1.0) * cosW0)) / a0;
    c.b2 = (A * ((A + 1.0) + (A - 1.0) * cosW0 - twoSqrtAAlpha)) / a0;
    c.a1 = (2.0 * ((A - 1.0) - (A + 1.0) * cosW0)) / a0;
    c.a2 = ((A + 1.0) - (A - 1.0) * cosW0 - twoSqrtAAlpha) / a0;
    return c;
}

BiquadCoeffs designBandpass(double freqHz, double q, double sampleRate) {
    const double w0 = 2.0 * std::numbers::pi * clampFrequency(freqHz, sampleRate) / sampleRate;
    const double cosW0 = std::cos(w0);
    const double sinW0 = std::sin(w0);
    const double alpha = sinW0 / (2.0 * clampQ(q));

    const double a0 = 1.0 + alpha;
    BiquadCoeffs c;
    c.b0 = alpha / a0;
    c.b1 = 0.0;
    c.b2 = -alpha / a0;
    c.a1 = (-2.0 * cosW0) / a0;
    c.a2 = (1.0 - alpha) / a0;
    return c;
}

double magnitudeAt(const BiquadCoeffs& coeffs, double freqHz, double sampleRate) {
    const double w = 2.0 * std::numbers::pi * freqHz / sampleRate;
    const std::complex<double> z1 = std::polar(1.0, -w);
    const std::complex<double> z2 = std::polar(1.0, -2.0 * w);

    const std::complex<double> numerator = coeffs.b0 + coeffs.b1 * z1 + coeffs.b2 * z2;
    const std::complex<double> denominator = 1.0 + coeffs.a1 * z1 + coeffs.a2 * z2;
    return std::abs(numerator / denominator);
}

}  // namespace audiolens::dsp
