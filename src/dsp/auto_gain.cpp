#include "dsp/auto_gain.h"

#include "dsp/k_weighting.h"

#include <algorithm>
#include <cmath>

namespace audiolens::dsp {
namespace {

/// The offset BS.1770 puts in front of the log so that a 997 Hz sine at
/// -20 dBFS measures -20 LUFS.
constexpr double kLoudnessOffset = -0.691;

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

void AutoGain::prepare(std::uint32_t sampleRate, std::uint32_t channels) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    channels_ = std::max<std::uint32_t>(1, channels);

    shelf_.assign(channels_, Biquad{});
    highpass_.assign(channels_, Biquad{});

    const BiquadCoeffs shelfCoeffs = designKWeightingShelf(sampleRate_);
    const BiquadCoeffs highpassCoeffs = designKWeightingHighpass(sampleRate_);
    for (std::uint32_t c = 0; c < channels_; ++c) {
        shelf_[c].setCoeffs(shelfCoeffs);
        highpass_[c].setCoeffs(highpassCoeffs);
    }

    blockFrames_ = std::max<std::size_t>(
        1, static_cast<std::size_t>(kBlockSeconds * static_cast<double>(sampleRate_)));

    updateRates();
    resizeWindow();
    reset();
}

void AutoGain::setSettings(const AutoGainSettings& settings) noexcept {
    settings_ = settings;
    updateRates();
    resizeWindow();
}

void AutoGain::updateRates() noexcept {
    // dB per second turned into dB per frame. Clamped away from zero so that a
    // settings block left at its defaults cannot freeze the gain where it is.
    const double perFrame = 1.0 / static_cast<double>(sampleRate_);
    riseStepPerFrame_ = std::max(1e-9, settings_.riseDbPerSecond * perFrame);
    fallStepPerFrame_ = std::max(1e-9, settings_.fallDbPerSecond * perFrame);
}

void AutoGain::resizeWindow() noexcept {
    const auto blocks = static_cast<std::size_t>(
        std::max(1.0, std::round(settings_.windowSeconds / kBlockSeconds)));
    if (blocks == window_.size()) {
        return;
    }
    // Only reached when the window length actually changes, which happens on a
    // preset change and not while audio is flowing steadily. The old contents
    // describe a different span and cannot be carried over.
    window_.assign(blocks, 0.0);
    windowIndex_ = 0;
    windowFilled_ = 0;
    windowSum_ = 0.0;
}

void AutoGain::reset() noexcept {
    for (Biquad& stage : shelf_) stage.reset();
    for (Biquad& stage : highpass_) stage.reset();

    blockSquareSum_ = 0.0;
    blockFrameCount_ = 0;
    for (double& value : window_) value = 0.0;
    windowIndex_ = 0;
    windowFilled_ = 0;
    windowSum_ = 0.0;


    targetGainDb_ = 0.0;
    currentGainDb_ = 0.0;
    measuredLufs_ = -1e9;
}

void AutoGain::closeBlock() noexcept {
    // BS.1770 sums the per-channel mean squares; dividing the accumulated sum
    // of squares by the frame count does exactly that in one step.
    const double meanSquare = blockSquareSum_ / static_cast<double>(blockFrames_);
    blockSquareSum_ = 0.0;
    blockFrameCount_ = 0;

    if (window_.empty()) {
        return;
    }

    windowSum_ -= window_[windowIndex_];
    window_[windowIndex_] = meanSquare;
    windowSum_ += meanSquare;
    windowIndex_ = (windowIndex_ + 1) % window_.size();
    if (windowFilled_ < window_.size()) {
        ++windowFilled_;
    }

    // Nothing at all happens until the window is genuinely full.
    //
    // Acting on a partial average was tried and was worse than doing nothing:
    // three seconds in, the "programme level" is whichever passage happens to
    // be playing, so the stage commits to a gain that the next half minute then
    // has to undo. A levelling stage that moves before it knows the level is
    // not levelling, it is guessing. Waiting costs half a minute at the start
    // of a session and nothing afterwards.
    if (windowFilled_ < window_.size()) {
        return;
    }

    const double windowMean = windowSum_ / static_cast<double>(window_.size());
    if (windowMean <= 0.0) {
        return;
    }
    measuredLufs_ = kLoudnessOffset + 10.0 * std::log10(windowMean);

    // Silence and room tone are held, not lifted: see AutoGainSettings::gateLufs.
    if (measuredLufs_ < settings_.gateLufs) {
        return;
    }

    targetGainDb_ = std::clamp(settings_.targetLufs - measuredLufs_, -settings_.maxCutDb,
                               settings_.maxBoostDb);
}

float AutoGain::nextGain(const float* frame, std::uint32_t channels) noexcept {
    if (!settings_.enabled) {
        // Unwind rather than snap, so switching the stage off does not step the
        // level by however much gain it happened to be holding.
        currentGainDb_ -= std::clamp(currentGainDb_, -fallStepPerFrame_, fallStepPerFrame_);
        targetGainDb_ = 0.0;
        return static_cast<float>(dbToLinear(currentGainDb_));
    }

    const std::uint32_t active = std::min(channels, channels_);
    for (std::uint32_t c = 0; c < active; ++c) {
        const double weighted = highpass_[c].process(shelf_[c].process(frame[c]));
        blockSquareSum_ += weighted * weighted;
    }

    if (++blockFrameCount_ >= blockFrames_) {
        closeBlock();
    }

    const double error = targetGainDb_ - currentGainDb_;
    const double step = error > 0.0 ? riseStepPerFrame_ : fallStepPerFrame_;
    currentGainDb_ += std::clamp(error, -step, step);

    return static_cast<float>(dbToLinear(currentGainDb_));
}

}  // namespace audiolens::dsp
