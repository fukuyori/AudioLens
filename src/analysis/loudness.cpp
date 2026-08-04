#include "analysis/loudness.h"

#include "dsp/biquad.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace audiolens::analysis {
namespace {

using dsp::Biquad;
using dsp::BiquadCoeffs;

constexpr double kNegativeInfinityLufs = -1e9;

/// BS.1770-4 defines the K-weighting coefficients only at 48 kHz. These are the
/// analog prototype parameters they were derived from, so the same filters can
/// be built for any rate; at 48 kHz they reproduce the published values.
BiquadCoeffs designKWeightingShelf(double sampleRate) {
    constexpr double f0 = 1681.974450955533;
    constexpr double gainDb = 3.999843853973347;
    constexpr double q = 0.7071752369554196;

    const double k = std::tan(std::numbers::pi * f0 / sampleRate);
    const double vh = std::pow(10.0, gainDb / 20.0);
    const double vb = std::pow(vh, 0.4996667741545416);
    const double a0 = 1.0 + k / q + k * k;

    BiquadCoeffs c;
    c.b0 = (vh + vb * k / q + k * k) / a0;
    c.b1 = 2.0 * (k * k - vh) / a0;
    c.b2 = (vh - vb * k / q + k * k) / a0;
    c.a1 = 2.0 * (k * k - 1.0) / a0;
    c.a2 = (1.0 - k / q + k * k) / a0;
    return c;
}

BiquadCoeffs designKWeightingHighpass(double sampleRate) {
    constexpr double f0 = 38.13547087602444;
    constexpr double q = 0.5003270373238773;

    const double k = std::tan(std::numbers::pi * f0 / sampleRate);
    const double denominator = 1.0 + k / q + k * k;

    BiquadCoeffs c;
    c.b0 = 1.0;
    c.b1 = -2.0;
    c.b2 = 1.0;
    c.a1 = 2.0 * (k * k - 1.0) / denominator;
    c.a2 = (1.0 - k / q + k * k) / denominator;
    return c;
}

double channelWeight(std::uint32_t channel, std::uint32_t channels) {
    // BS.1770 weights the surround channels up by +1.5 dB. The layout is only
    // knowable for the standard 5-channel order; anything else gets unity.
    if (channels == 5 && (channel == 3 || channel == 4)) {
        return 1.41;
    }
    if (channels == 6 && (channel == 4 || channel == 5)) {
        return 1.41;  // 5.1 with LFE at index 3, which BS.1770 excludes.
    }
    return 1.0;
}

bool contributesToLoudness(std::uint32_t channel, std::uint32_t channels) {
    // The LFE channel is not part of the loudness measurement.
    return !(channels == 6 && channel == 3);
}

/// Mean square of each 400 ms block, K-weighted and channel-summed, with 75 %
/// overlap. Every loudness figure BS.1770 defines is derived from this series.
std::vector<double> blockMeanSquares(const std::vector<float>& interleaved, std::uint32_t channels,
                                     std::uint32_t sampleRate, double blockSeconds,
                                     double hopSeconds) {
    std::vector<double> result;
    if (channels == 0 || sampleRate == 0 || interleaved.empty()) {
        return result;
    }

    const std::size_t frames = interleaved.size() / channels;
    const auto blockFrames = static_cast<std::size_t>(blockSeconds * sampleRate);
    const auto hopFrames = static_cast<std::size_t>(hopSeconds * sampleRate);
    if (blockFrames == 0 || hopFrames == 0 || frames < blockFrames) {
        return result;
    }

    // K-weight the whole signal once, per channel, then take windows of it.
    std::vector<float> weighted(interleaved.size());
    const BiquadCoeffs shelf = designKWeightingShelf(sampleRate);
    const BiquadCoeffs highpass = designKWeightingHighpass(sampleRate);

    for (std::uint32_t c = 0; c < channels; ++c) {
        Biquad stage1;
        Biquad stage2;
        stage1.setCoeffs(shelf);
        stage2.setCoeffs(highpass);
        for (std::size_t f = 0; f < frames; ++f) {
            const std::size_t i = f * channels + c;
            weighted[i] = stage2.process(stage1.process(interleaved[i]));
        }
    }

    for (std::size_t start = 0; start + blockFrames <= frames; start += hopFrames) {
        double sum = 0.0;
        for (std::uint32_t c = 0; c < channels; ++c) {
            if (!contributesToLoudness(c, channels)) {
                continue;
            }
            double channelSum = 0.0;
            for (std::size_t f = 0; f < blockFrames; ++f) {
                const double v = weighted[(start + f) * channels + c];
                channelSum += v * v;
            }
            sum += channelWeight(c, channels) * channelSum / static_cast<double>(blockFrames);
        }
        result.push_back(sum);
    }

    return result;
}

double meanSquareToLufs(double meanSquare) {
    if (meanSquare <= 0.0) {
        return kNegativeInfinityLufs;
    }
    return -0.691 + 10.0 * std::log10(meanSquare);
}

double lufsToMeanSquare(double lufs) { return std::pow(10.0, (lufs + 0.691) / 10.0); }

/// Linear-interpolated percentile of an already-sorted series.
double percentile(const std::vector<double>& sorted, double fraction) {
    if (sorted.empty()) {
        return 0.0;
    }
    if (sorted.size() == 1) {
        return sorted.front();
    }
    const double position = fraction * static_cast<double>(sorted.size() - 1);
    const auto lower = static_cast<std::size_t>(std::floor(position));
    const std::size_t upper = std::min(lower + 1, sorted.size() - 1);
    const double t = position - static_cast<double>(lower);
    return sorted[lower] + (sorted[upper] - sorted[lower]) * t;
}

}  // namespace

std::vector<double> shortTermLoudnessSeries(const std::vector<float>& interleaved,
                                            std::uint32_t channels, std::uint32_t sampleRate) {
    const std::vector<double> squares =
        blockMeanSquares(interleaved, channels, sampleRate, 3.0, 0.1);

    std::vector<double> result;
    result.reserve(squares.size());
    for (const double square : squares) {
        result.push_back(meanSquareToLufs(square));
    }
    return result;
}

LoudnessResult measureLoudness(const std::vector<float>& interleaved, std::uint32_t channels,
                               std::uint32_t sampleRate) {
    LoudnessResult result;
    if (channels == 0 || interleaved.empty()) {
        return result;
    }

    double peak = 0.0;
    for (const float sample : interleaved) {
        peak = std::max(peak, static_cast<double>(std::fabs(sample)));
    }
    result.samplePeakDbfs = peak > 0.0 ? 20.0 * std::log10(peak) : kNegativeInfinityLufs;

    // --- Integrated loudness: 400 ms blocks, absolute then relative gate. ---
    const std::vector<double> momentary =
        blockMeanSquares(interleaved, channels, sampleRate, 0.4, 0.1);
    if (momentary.empty()) {
        return result;
    }

    for (const double square : momentary) {
        result.maxMomentaryLufs = std::max(result.maxMomentaryLufs, meanSquareToLufs(square));
    }

    constexpr double kAbsoluteGateLufs = -70.0;
    const double absoluteGate = lufsToMeanSquare(kAbsoluteGateLufs);

    double sum = 0.0;
    std::size_t count = 0;
    for (const double square : momentary) {
        if (square > absoluteGate) {
            sum += square;
            ++count;
        }
    }
    if (count == 0) {
        return result;
    }

    // The relative gate sits 10 LU below the absolutely-gated mean, which stops
    // long quiet passages from dragging the figure down.
    const double relativeGate = lufsToMeanSquare(meanSquareToLufs(sum / count) - 10.0);

    sum = 0.0;
    count = 0;
    for (const double square : momentary) {
        if (square > absoluteGate && square > relativeGate) {
            sum += square;
            ++count;
        }
    }
    result.integratedLufs =
        count == 0 ? kNegativeInfinityLufs : meanSquareToLufs(sum / static_cast<double>(count));

    // --- Loudness range (EBU Tech 3342): 3 s blocks, gate 20 LU down. ---
    const std::vector<double> shortTerm =
        blockMeanSquares(interleaved, channels, sampleRate, 3.0, 0.1);
    if (shortTerm.size() >= 2) {
        double stSum = 0.0;
        std::size_t stCount = 0;
        for (const double square : shortTerm) {
            if (square > absoluteGate) {
                stSum += square;
                ++stCount;
            }
        }
        if (stCount > 0) {
            const double stGate = lufsToMeanSquare(meanSquareToLufs(stSum / stCount) - 20.0);

            std::vector<double> gated;
            gated.reserve(shortTerm.size());
            for (const double square : shortTerm) {
                if (square > absoluteGate && square > stGate) {
                    gated.push_back(meanSquareToLufs(square));
                }
            }
            if (gated.size() >= 2) {
                std::sort(gated.begin(), gated.end());
                result.loudnessRangeLu = percentile(gated, 0.95) - percentile(gated, 0.10);
            }
        }
    }

    return result;
}

double bandLevelDbfs(const std::vector<float>& interleaved, std::uint32_t channels,
                     std::uint32_t sampleRate, double centreHz, double q) {
    if (channels == 0 || interleaved.empty()) {
        return kNegativeInfinityLufs;
    }

    const std::size_t frames = interleaved.size() / channels;
    const BiquadCoeffs coeffs = dsp::designBandpass(centreHz, q, sampleRate);

    double sumOfSquares = 0.0;
    for (std::uint32_t c = 0; c < channels; ++c) {
        Biquad filter;
        filter.setCoeffs(coeffs);
        for (std::size_t f = 0; f < frames; ++f) {
            const double v = filter.process(interleaved[f * channels + c]);
            sumOfSquares += v * v;
        }
    }

    const double meanSquare = sumOfSquares / static_cast<double>(frames * channels);
    return meanSquare > 0.0 ? 10.0 * std::log10(meanSquare) : kNegativeInfinityLufs;
}

}  // namespace audiolens::analysis
