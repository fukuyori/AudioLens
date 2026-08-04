#include "dsp/resampler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <numbers>

namespace audiolens::dsp {
namespace {

/// Leaves a little room below Nyquist so the transition band of the kernel has
/// somewhere to go instead of folding.
constexpr double kCutoffMargin = 0.92;

/// How far the ratio may be trimmed from its base. The anti-aliasing kernel is
/// designed for the base ratio; a few hundred ppm of drift correction does not
/// invalidate it, but a large change would.
constexpr double kMaxTrim = 0.02;  // ±2 %

double sinc(double x) {
    if (std::fabs(x) < 1e-9) {
        return 1.0;
    }
    const double pix = std::numbers::pi * x;
    return std::sin(pix) / pix;
}

/// Blackman-Harris, evaluated over t in [-1, 1]. Its -92 dB sidelobes are what
/// keep the stopband below audibility with only 32 taps.
double window(double t) {
    if (std::fabs(t) >= 1.0) {
        return 0.0;
    }
    const double phase = std::numbers::pi * (t + 1.0);  // 0 .. 2*pi
    return 0.35875 - 0.48829 * std::cos(phase) + 0.14128 * std::cos(2.0 * phase) -
           0.01168 * std::cos(3.0 * phase);
}

}  // namespace

void Resampler::prepare(std::uint32_t channels, double outputPerInputRatio,
                        std::size_t maxOutputFrames) {
    channels_ = std::max<std::uint32_t>(1, channels);
    baseRatio_ = outputPerInputRatio > 0.0 ? outputPerInputRatio : 1.0;
    halfTaps_ = kTaps / 2;

    buildKernel(baseRatio_);
    setRatioTrim(1.0);

    // Worst case per pull: the input needed for maxOutputFrames at the slowest
    // ratio, plus the taps that have to stay resident either side, plus one
    // whole pull's worth of slack so a caller that pushes generously is never
    // rejected.
    const double slowestRatio = baseRatio_ * (1.0 - kMaxTrim);
    const std::size_t worstInput =
        static_cast<std::size_t>(std::ceil(maxOutputFrames / slowestRatio)) + 2 * kTaps;
    capacityFrames_ = 2 * worstInput + maxOutputFrames + 2 * kTaps;

    fifo_.assign(capacityFrames_ * channels_, 0.0f);
    tapScratch_.assign(kTaps, 0.0f);

    reset();
}

void Resampler::buildKernel(double ratio) {
    // Downsampling has to band-limit to the *output* Nyquist, which is lower
    // than the input's; upsampling only needs the input's.
    const double cutoff = std::min(1.0, ratio) * kCutoffMargin;

    kernel_.assign((kPhases + 1) * kTaps, 0.0f);

    for (std::size_t phase = 0; phase <= kPhases; ++phase) {
        const double frac = static_cast<double>(phase) / kPhases;

        double sum = 0.0;
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            // Distance, in input samples, from the interpolation point to this
            // tap's sample.
            const double distance =
                static_cast<double>(tap) - static_cast<double>(halfTaps_) + 1.0 - frac;
            const double value =
                sinc(cutoff * distance) * cutoff * window(distance / static_cast<double>(halfTaps_));
            kernel_[phase * kTaps + tap] = static_cast<float>(value);
            sum += value;
        }

        // Normalize each phase to unity DC gain. Without this the output level
        // wobbles as the fractional position moves, which is audible as a
        // low-level warble on steady tones.
        if (sum > 1e-12) {
            const auto scale = static_cast<float>(1.0 / sum);
            for (std::size_t tap = 0; tap < kTaps; ++tap) {
                kernel_[phase * kTaps + tap] *= scale;
            }
        }
    }
}

void Resampler::reset() noexcept {
    std::fill(fifo_.begin(), fifo_.end(), 0.0f);
    readIndex_ = 0;
    writeIndex_ = 0;

    // Prime with the history the first output sample's taps will reach back
    // into, and start reading at the point those taps are centred on.
    writeIndex_ = halfTaps_;
    position_ = static_cast<double>(halfTaps_) - 1.0;
}

void Resampler::setRatioTrim(double trim) noexcept {
    trim_ = std::clamp(trim, 1.0 - kMaxTrim, 1.0 + kMaxTrim);
    const double effective = baseRatio_ * trim_;
    step_ = effective > 1e-9 ? 1.0 / effective : 1.0;
}

std::size_t Resampler::inputFramesWanted(std::size_t outputFrames) const noexcept {
    if (outputFrames == 0) {
        return 0;
    }
    // The last output frame sits at position_ + (outputFrames-1)*step_, and its
    // taps reach halfTaps_ frames beyond that.
    const double lastPosition = position_ + static_cast<double>(outputFrames - 1) * step_;
    const auto needed =
        static_cast<std::size_t>(std::floor(lastPosition)) + halfTaps_ + 1;
    const std::size_t have = availableInputFrames();
    return needed > have ? needed - have : 0;
}

void Resampler::compact() noexcept {
    // Slide the live region back to the front so the FIFO can keep being
    // written into linearly.
    const std::size_t shift = readIndex_;
    if (shift == 0) {
        return;
    }
    const std::size_t live = writeIndex_ - readIndex_;
    if (live > 0) {
        std::memmove(fifo_.data(), &fifo_[readIndex_ * channels_], live * channels_ * sizeof(float));
    }
    readIndex_ = 0;
    writeIndex_ = live;

    // position_ indexes the FIFO absolutely, so it has to move with the data.
    // Forgetting this reads from the wrong place after the first compaction,
    // which sounds like the stream jumping backwards.
    position_ -= static_cast<double>(shift);
}

void Resampler::pushInput(const float* interleaved, std::size_t frames) noexcept {
    if (frames == 0) {
        return;
    }
    if (writeIndex_ + frames > capacityFrames_) {
        compact();
    }
    const std::size_t writable = std::min(frames, capacityFrames_ - writeIndex_);
    std::memcpy(&fifo_[writeIndex_ * channels_], interleaved, writable * channels_ * sizeof(float));
    writeIndex_ += writable;
}

void Resampler::pushSilence(std::size_t frames) noexcept {
    if (frames == 0) {
        return;
    }
    if (writeIndex_ + frames > capacityFrames_) {
        compact();
    }
    const std::size_t writable = std::min(frames, capacityFrames_ - writeIndex_);
    std::memset(&fifo_[writeIndex_ * channels_], 0, writable * channels_ * sizeof(float));
    writeIndex_ += writable;
}

std::size_t Resampler::pullOutput(float* interleaved, std::size_t frames) noexcept {
    std::size_t produced = 0;

    for (; produced < frames; ++produced) {
        const auto base = static_cast<std::size_t>(std::floor(position_));

        // Stop when the taps ahead of the interpolation point would read past
        // what has been pushed.
        if (base + halfTaps_ >= writeIndex_) {
            break;
        }
        // ...and behind it, past what is still resident.
        if (base + 1 < halfTaps_ || base + 1 - halfTaps_ < readIndex_) {
            break;
        }

        const double frac = position_ - static_cast<double>(base);

        // Interpolate between the two nearest phase rows. 512 rows alone would
        // leave audible phase quantisation; blending them puts it well below
        // the noise floor for the cost of one lerp per tap.
        const double phasePosition = frac * static_cast<double>(kPhases);
        const auto phase = static_cast<std::size_t>(phasePosition);
        const auto phaseFrac = static_cast<float>(phasePosition - static_cast<double>(phase));

        const float* lower = &kernel_[phase * kTaps];
        const float* upper = &kernel_[(phase + 1) * kTaps];
        for (std::size_t tap = 0; tap < kTaps; ++tap) {
            tapScratch_[tap] = lower[tap] + (upper[tap] - lower[tap]) * phaseFrac;
        }

        const std::size_t first = base + 1 - halfTaps_;
        float* out = interleaved + produced * channels_;
        for (std::uint32_t c = 0; c < channels_; ++c) {
            float accumulator = 0.0f;
            for (std::size_t tap = 0; tap < kTaps; ++tap) {
                accumulator += fifo_[(first + tap) * channels_ + c] * tapScratch_[tap];
            }
            out[c] = accumulator;
        }

        position_ += step_;

        // Retire input the taps can no longer reach, so compact() has something
        // to reclaim.
        const auto newBase = static_cast<std::size_t>(std::floor(position_));
        if (newBase >= halfTaps_) {
            readIndex_ = std::max(readIndex_, newBase + 1 - halfTaps_);
        }
    }

    return produced;
}

}  // namespace audiolens::dsp
