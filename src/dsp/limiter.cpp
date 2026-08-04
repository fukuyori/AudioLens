#include "dsp/limiter.h"

#include <algorithm>
#include <cmath>

namespace audiolens::dsp {
namespace {

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

void Limiter::prepare(double sampleRate, std::uint32_t channels) {
    sampleRate_ = sampleRate > 0.0 ? sampleRate : 48000.0;
    channels_ = std::max<std::uint32_t>(1, channels);

    maxLookaheadFrames_ = static_cast<std::size_t>(
        std::max(1.0, std::ceil(kMaxLookaheadMs * 0.001 * sampleRate_)));

    delayLine_.assign(maxLookaheadFrames_ * channels_, 0.0f);
    // The window holds at most one entry per sample, plus a slot kept free so
    // a full deque is distinguishable from an empty one.
    queueGain_.assign(maxLookaheadFrames_ + 2, 1.0);
    queueIndex_.assign(maxLookaheadFrames_ + 2, 0);

    lookaheadFrames_ = 0;  // Force setSettings to install the window.
    setSettings(settings_);
    reset();
}

void Limiter::reset() noexcept {
    std::fill(delayLine_.begin(), delayLine_.end(), 0.0f);
    delayWrite_ = 0;
    queueHead_ = 0;
    queueTail_ = 0;
    sampleIndex_ = 0;
    currentGain_ = 1.0;
    clippedSamples_ = 0;
}

void Limiter::setSettings(const LimiterSettings& settings) noexcept {
    settings_ = settings;
    ceilingLinear_ = dbToLinear(settings_.ceilingDb);

    releaseCoeff_ = settings_.releaseMs > 0.0
                        ? std::exp(-1.0 / (settings_.releaseMs * 0.001 * sampleRate_))
                        : 0.0;

    if (maxLookaheadFrames_ == 0) {
        return;  // prepare() has not run yet.
    }

    const auto requested = static_cast<std::size_t>(
        std::max(1.0, std::round(settings_.lookaheadMs * 0.001 * sampleRate_)));
    const std::size_t clamped = std::min(requested, maxLookaheadFrames_);

    // Changing the window length re-indexes the delay line, so the old contents
    // would come out in the wrong order. Only pay that cost on an actual change:
    // this is called once per sub-block while parameters sit still.
    if (clamped != lookaheadFrames_) {
        lookaheadFrames_ = clamped;
        reset();
    }

    // Reaching the target within half the look-ahead window leaves margin, so
    // the gain is always fully applied before the peak it was computed for
    // arrives at the output.
    attackStep_ = 1.0 / std::max(1.0, static_cast<double>(lookaheadFrames_) / 2.0);
}

double Limiter::pushAndGetMinimum(double gain) noexcept {
    const std::size_t capacity = queueGain_.size();

    // Drop entries that this one supersedes: anything not smaller than the
    // incoming gain can never be the window minimum again.
    while (queueTail_ != queueHead_) {
        const std::size_t back = (queueTail_ + capacity - 1) % capacity;
        if (queueGain_[back] >= gain) {
            queueTail_ = back;
        } else {
            break;
        }
    }

    queueGain_[queueTail_] = gain;
    queueIndex_[queueTail_] = sampleIndex_;
    queueTail_ = (queueTail_ + 1) % capacity;

    // Retire entries that have fallen out of the window. The entry just pushed
    // can never be retired, so the deque cannot run empty here.
    while (queueIndex_[queueHead_] + lookaheadFrames_ < sampleIndex_) {
        queueHead_ = (queueHead_ + 1) % capacity;
    }

    return queueGain_[queueHead_];
}

void Limiter::processFrame(float* frame, std::uint32_t channels) noexcept {
    const std::uint32_t activeChannels = std::min(channels, channels_);

    double peak = 0.0;
    for (std::uint32_t c = 0; c < activeChannels; ++c) {
        peak = std::max(peak, static_cast<double>(std::fabs(frame[c])));
    }

    const double requiredGain = peak > ceilingLinear_ ? ceilingLinear_ / peak : 1.0;

    // The window spans input samples [i - lookahead, i], and the frame leaving
    // the delay line is input sample i - lookahead: the oldest member of the
    // window. That is the invariant the whole limiter rests on.
    const double targetGain = pushAndGetMinimum(requiredGain);

    if (targetGain < currentGain_) {
        currentGain_ = std::max(targetGain, currentGain_ - attackStep_);
    } else {
        currentGain_ = targetGain + releaseCoeff_ * (currentGain_ - targetGain);
    }

    const std::size_t slot = delayWrite_ * channels_;
    for (std::uint32_t c = 0; c < activeChannels; ++c) {
        const float delayed = delayLine_[slot + c];
        delayLine_[slot + c] = frame[c];

        auto out = static_cast<float>(delayed * currentGain_);
        if (out > ceilingLinear_) {
            out = static_cast<float>(ceilingLinear_);
            ++clippedSamples_;
        } else if (out < -ceilingLinear_) {
            out = static_cast<float>(-ceilingLinear_);
            ++clippedSamples_;
        }
        frame[c] = out;
    }
    for (std::uint32_t c = activeChannels; c < channels; ++c) {
        frame[c] = 0.0f;
    }

    delayWrite_ = (delayWrite_ + 1) % lookaheadFrames_;
    ++sampleIndex_;
}

}  // namespace audiolens::dsp
