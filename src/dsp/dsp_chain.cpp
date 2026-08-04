#include "dsp/dsp_chain.h"

#include <algorithm>
#include <cmath>

namespace audiolens::dsp {
namespace {

/// Control values are re-evaluated this often. At 48 kHz a 32-frame sub-block
/// is 0.67 ms, so a 40 ms ramp lands in ~60 steps: fine enough that a slider
/// sweep is inaudible, coarse enough that recomputing coefficients is cheap.
constexpr std::size_t kSubBlockFrames = 32;

/// Time constant for ramping control values toward their targets.
constexpr double kSmoothingMs = 40.0;

float dbToLinear(double db) { return static_cast<float>(std::pow(10.0, db / 20.0)); }

void approach(double& value, double target, double coeff) {
    value = target + coeff * (value - target);
}

bool nearlyEqual(double a, double b, double tolerance) { return std::fabs(a - b) < tolerance; }

/// How long a meter takes to fall by roughly 63 %. Slow enough that a transient
/// stays visible at any polling rate a UI would use.
constexpr double kMeterDecayMs = 300.0;

float blockPeak(const float* audio, std::size_t frames, std::uint32_t channels) noexcept {
    float peak = 0.0f;
    for (std::size_t i = 0; i < frames * channels; ++i) {
        peak = std::max(peak, std::fabs(audio[i]));
    }
    return peak;
}

/// Holds the highest recent value, letting it fall away over time.
void updateMeter(std::atomic<float>& meter, float peak, double decay) noexcept {
    const float previous = meter.load(std::memory_order_relaxed);
    meter.store(std::max(peak, static_cast<float>(previous * decay)), std::memory_order_relaxed);
}

}  // namespace

void DspChain::FilterStage::prepare(std::uint32_t channels) {
    perChannel.assign(channels, Biquad{});
}

void DspChain::FilterStage::setCoeffs(const BiquadCoeffs& coeffs) noexcept {
    for (Biquad& filter : perChannel) {
        filter.setCoeffs(coeffs);
    }
}

void DspChain::FilterStage::reset() noexcept {
    for (Biquad& filter : perChannel) {
        filter.reset();
    }
}

void DspChain::prepare(std::uint32_t sampleRate, std::uint32_t channels,
                       std::uint32_t /*maxFramesPerBlock*/) {
    sampleRate_ = sampleRate > 0 ? sampleRate : 48000;
    channels_ = std::max<std::uint32_t>(1, channels);

    highpass_.prepare(channels_);
    lowShelf_.prepare(channels_);
    for (FilterStage& stage : speechBands_) {
        stage.prepare(channels_);
    }
    highShelf_.prepare(channels_);

    deEsser_.prepare(sampleRate_, channels_);
    autoGain_.prepare(sampleRate_, channels_);
    compressor_.prepare(sampleRate_);
    limiter_.prepare(sampleRate_, channels_);

    // One smoothing step per sub-block, not per sample.
    const double stepsPerSecond = static_cast<double>(sampleRate_) / kSubBlockFrames;
    smoothingCoeff_ = std::exp(-1.0 / (kSmoothingMs * 0.001 * stepsPerSecond));
    meterDecayPerSubBlock_ = std::exp(-1.0 / (kMeterDecayMs * 0.001 * stepsPerSecond));

    // Start already settled on the published parameters so the first block is
    // not a 40 ms ramp up from silence.
    fetchParameters();
    current_ = target_;
    coefficientsValid_ = false;
    rebuildCoefficients();

    inputGainLinear_ = dbToLinear(current_.inputGainDb);
    outputGainLinear_ = dbToLinear(current_.outputGainDb);
}

void DspChain::setParameters(const DspParameters& parameters) noexcept {
    const std::uint32_t seq = paramSeq_.load(std::memory_order_relaxed);
    paramSeq_.store(seq + 1, std::memory_order_release);  // odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);

    sharedParams_ = parameters;

    std::atomic_thread_fence(std::memory_order_release);
    paramSeq_.store(seq + 2, std::memory_order_release);  // even: readable again
    parametersDirty_.store(true, std::memory_order_release);
}

bool DspChain::fetchParameters() noexcept {
    const std::uint32_t before = paramSeq_.load(std::memory_order_acquire);
    if ((before & 1u) != 0u) {
        return false;  // Writer is mid-update; keep the previous targets.
    }

    const DspParameters copy = sharedParams_;

    std::atomic_thread_fence(std::memory_order_acquire);
    if (paramSeq_.load(std::memory_order_relaxed) != before) {
        return false;  // Torn read; the next sub-block will pick it up.
    }

    target_ = copy;
    return true;
}

void DspChain::updateSmoothedTargets() noexcept {
    const double c = smoothingCoeff_;

    approach(current_.inputGainDb, target_.inputGainDb, c);
    approach(current_.outputGainDb, target_.outputGainDb, c);
    approach(current_.highpassFreqHz, target_.highpassFreqHz, c);
    approach(current_.lowShelfGainDb, target_.lowShelfGainDb, c);
    approach(current_.lowShelfFreqHz, target_.lowShelfFreqHz, c);
    approach(current_.highShelfGainDb, target_.highShelfGainDb, c);
    approach(current_.highShelfFreqHz, target_.highShelfFreqHz, c);
    approach(current_.sideGainDb, target_.sideGainDb, c);

    for (int i = 0; i < kMaxSpeechBands; ++i) {
        approach(current_.speechBands[i].gainDb, target_.speechBands[i].gainDb, c);
        approach(current_.speechBands[i].freqHz, target_.speechBands[i].freqHz, c);
        current_.speechBands[i].q = target_.speechBands[i].q;
    }

    // Ramping a highpass through the audio band sounds worse than switching it,
    // and the gain either side of the switch is the same, so these follow the
    // target immediately.
    current_.highpassEnabled = target_.highpassEnabled;
    current_.highpassQ = target_.highpassQ;
    current_.lowShelfQ = target_.lowShelfQ;
    current_.highShelfQ = target_.highShelfQ;
    current_.speechBandCount = target_.speechBandCount;

    current_.midSideEnabled = target_.midSideEnabled;

    // The compressor smooths its own gain with attack and release, the auto
    // gain has its own rate limits, and the limiter is a safety device, so all
    // three take settings changes directly.
    current_.compressorEnabled = target_.compressorEnabled;
    current_.compressor = target_.compressor;
    current_.deEsser = target_.deEsser;
    current_.autoGain = target_.autoGain;
    current_.limiter = target_.limiter;
}

bool DspChain::coefficientsNeedRebuild() const noexcept {
    if (!coefficientsValid_) {
        return true;
    }

    // 0.01 dB and 0.1 Hz are both far below audibility, so stopping there costs
    // nothing and saves redesigning six filters on every sub-block.
    constexpr double kGainTolerance = 0.01;
    constexpr double kFreqTolerance = 0.1;

    if (current_.highpassEnabled != built_.highpassEnabled ||
        current_.speechBandCount != built_.speechBandCount ||
        current_.compressorEnabled != built_.compressorEnabled ||
        current_.midSideEnabled != built_.midSideEnabled ||
        !(current_.autoGain == built_.autoGain) || !(current_.deEsser == built_.deEsser) ||
        !(current_.compressor == built_.compressor) ||
        !(current_.limiter == built_.limiter)) {
        return true;
    }

    if (!nearlyEqual(current_.inputGainDb, built_.inputGainDb, kGainTolerance) ||
        !nearlyEqual(current_.outputGainDb, built_.outputGainDb, kGainTolerance) ||
        !nearlyEqual(current_.lowShelfGainDb, built_.lowShelfGainDb, kGainTolerance) ||
        !nearlyEqual(current_.highShelfGainDb, built_.highShelfGainDb, kGainTolerance) ||
        !nearlyEqual(current_.sideGainDb, built_.sideGainDb, kGainTolerance)) {
        return true;
    }

    if (!nearlyEqual(current_.highpassFreqHz, built_.highpassFreqHz, kFreqTolerance) ||
        !nearlyEqual(current_.lowShelfFreqHz, built_.lowShelfFreqHz, kFreqTolerance) ||
        !nearlyEqual(current_.highShelfFreqHz, built_.highShelfFreqHz, kFreqTolerance) ||
        !nearlyEqual(current_.highpassQ, built_.highpassQ, kGainTolerance) ||
        !nearlyEqual(current_.lowShelfQ, built_.lowShelfQ, kGainTolerance) ||
        !nearlyEqual(current_.highShelfQ, built_.highShelfQ, kGainTolerance)) {
        return true;
    }

    for (int i = 0; i < kMaxSpeechBands; ++i) {
        if (!nearlyEqual(current_.speechBands[i].gainDb, built_.speechBands[i].gainDb,
                         kGainTolerance) ||
            !nearlyEqual(current_.speechBands[i].freqHz, built_.speechBands[i].freqHz,
                         kFreqTolerance) ||
            !nearlyEqual(current_.speechBands[i].q, built_.speechBands[i].q, kGainTolerance)) {
            return true;
        }
    }

    return false;
}

void DspChain::rebuildCoefficients() noexcept {
    const auto rate = static_cast<double>(sampleRate_);

    if (current_.highpassEnabled) {
        highpass_.setCoeffs(designHighpass(current_.highpassFreqHz, current_.highpassQ, rate));
    }
    lowShelf_.setCoeffs(
        designLowShelf(current_.lowShelfFreqHz, current_.lowShelfGainDb, current_.lowShelfQ, rate));

    for (int i = 0; i < kMaxSpeechBands; ++i) {
        const SpeechBand& band = current_.speechBands[i];
        speechBands_[static_cast<std::size_t>(i)].setCoeffs(
            designPeaking(band.freqHz, band.gainDb, band.q, rate));
    }

    highShelf_.setCoeffs(designHighShelf(current_.highShelfFreqHz, current_.highShelfGainDb,
                                         current_.highShelfQ, rate));

    deEsser_.setSettings(current_.deEsser);
    autoGain_.setSettings(current_.autoGain);
    compressor_.setSettings(current_.compressor);
    limiter_.setSettings(current_.limiter);

    inputGainLinear_ = dbToLinear(current_.inputGainDb);
    outputGainLinear_ = dbToLinear(current_.outputGainDb);
    sideGainLinear_ = dbToLinear(current_.sideGainDb);

    // Switching the speech bands between L/R and mid leaves their state holding
    // samples from the wrong signal. Clearing is a discontinuity of one sample
    // at most; carrying it over is a click.
    const bool midSide = current_.midSideEnabled && channels_ == 2;
    if (midSide != midSideActive_) {
        midSideActive_ = midSide;
        for (FilterStage& stage : speechBands_) {
            stage.reset();
        }
    }

    built_ = current_;
    coefficientsValid_ = true;
}

void DspChain::process(float* audio, std::size_t frames, std::uint32_t channels) noexcept {
    const bool bypass = bypass_.load(std::memory_order_relaxed);
    const std::uint32_t active = std::min(channels, channels_);

    std::size_t offset = 0;
    while (offset < frames) {
        const std::size_t count = std::min(kSubBlockFrames, frames - offset);

        if (parametersDirty_.load(std::memory_order_acquire)) {
            if (fetchParameters()) {
                parametersDirty_.store(false, std::memory_order_relaxed);
            }
        }
        updateSmoothedTargets();
        if (coefficientsNeedRebuild()) {
            rebuildCoefficients();
        }

        float* block = audio + offset * channels;
        updateMeter(meterInputPeak_, blockPeak(block, count, channels), meterDecayPerSubBlock_);

        if (bypass) {
            // Still run the limiter, and only the limiter, so that bypass keeps
            // the chain's latency and its safety guarantee. Its gain sits at
            // unity unless the source itself is over the ceiling.
            for (std::size_t f = 0; f < count; ++f) {
                limiter_.processFrame(block + f * channels, channels);
            }
            updateMeter(meterOutputPeak_, blockPeak(block, count, channels),
                        meterDecayPerSubBlock_);
            offset += count;
            continue;
        }

        const int bands = std::min(current_.speechBandCount, kMaxSpeechBands);

        for (std::size_t f = 0; f < count; ++f) {
            float* frame = block + f * channels;

            // --- bass shaping, always per channel ---
            for (std::uint32_t c = 0; c < active; ++c) {
                float sample = frame[c] * inputGainLinear_;
                if (current_.highpassEnabled) {
                    sample = highpass_.perChannel[c].process(sample);
                }
                frame[c] = lowShelf_.perChannel[c].process(sample);
            }

            // --- speech bands, on the mid channel when asked ---
            // `active` is re-checked here and not only in rebuildCoefficients:
            // the caller supplies the channel count per call, and reading
            // frame[1] on a mono block would run off the end of it.
            if (midSideActive_ && active == 2) {
                float mid = 0.5f * (frame[0] + frame[1]);
                float side = 0.5f * (frame[0] - frame[1]);

                for (int b = 0; b < bands; ++b) {
                    mid = speechBands_[static_cast<std::size_t>(b)].perChannel[0].process(mid);
                }
                side *= sideGainLinear_;

                frame[0] = mid + side;
                frame[1] = mid - side;
            } else {
                for (std::uint32_t c = 0; c < active; ++c) {
                    float sample = frame[c];
                    for (int b = 0; b < bands; ++b) {
                        sample =
                            speechBands_[static_cast<std::size_t>(b)].perChannel[c].process(sample);
                    }
                    frame[c] = sample;
                }
            }

            // --- air, always per channel ---
            for (std::uint32_t c = 0; c < active; ++c) {
                frame[c] = highShelf_.perChannel[c].process(frame[c]);
            }

            // --- undo the sharpness the speech bands just added ---
            deEsser_.processFrame(frame, active);

            // --- levelling: fast first, then slow ---
            // The order matters and it is not the obvious one. Putting the slow
            // stage first — normalising the level going into the compressor —
            // measured *worse* than no slow stage at all: with material that
            // alternates every few seconds, a stage working on a three second
            // window arrives half a cycle late and adds gain to the loud part
            // and takes it off the quiet one, which is the opposite of the job.
            //
            // Behind the compressor there is no such trap. The fast stage has
            // already flattened everything inside its own reach, so what
            // reaches the slow stage varies only over the long haul, which is
            // the only thing it was ever meant to correct.
            if (current_.compressorEnabled) {
                compressor_.processFrame(frame, active);
            }

            const float slowGain = autoGain_.nextGain(frame, active);
            for (std::uint32_t c = 0; c < active; ++c) {
                frame[c] *= slowGain * outputGainLinear_;
            }

            limiter_.processFrame(frame, channels);
        }

        updateMeter(meterOutputPeak_, blockPeak(block, count, channels), meterDecayPerSubBlock_);
        offset += count;
    }
}

LevelSnapshot DspChain::levels() const noexcept {
    LevelSnapshot snapshot;
    snapshot.inputPeak = meterInputPeak_.load(std::memory_order_relaxed);
    snapshot.outputPeak = meterOutputPeak_.load(std::memory_order_relaxed);
    snapshot.gainReductionDb = static_cast<float>(compressor_.currentGainDb());
    return snapshot;
}

double DspChain::latencyMs() const noexcept {
    return 1000.0 * static_cast<double>(limiter_.latencyFrames()) / sampleRate_;
}

double DspChain::gainReductionDb() const noexcept { return compressor_.currentGainDb(); }

std::uint64_t DspChain::limiterClippedSamples() const noexcept {
    return limiter_.clippedSamples();
}

}  // namespace audiolens::dsp
