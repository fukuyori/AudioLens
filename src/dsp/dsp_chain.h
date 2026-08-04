#pragma once

#include "dsp/auto_gain.h"
#include "dsp/biquad.h"
#include "dsp/compressor.h"
#include "dsp/limiter.h"
#include "dsp/parameters.h"
#include "engine/audio_engine.h"

#include <array>
#include <atomic>
#include <cstdint>
#include <vector>

namespace audiolens::dsp {

/// What the meters show. Peaks are linear (1.0 == full scale) and already
/// carry their decay, so a UI polling at any rate sees sensible ballistics
/// rather than whatever happened to be in the last block it caught.
struct LevelSnapshot {
    float inputPeak = 0.0f;
    float outputPeak = 0.0f;
    float gainReductionDb = 0.0f;  ///< Never positive.
};

/// The AudioLens processing chain:
///
///     input gain -> highpass -> low shelf -> speech bands -> high shelf
///                -> compressor -> auto gain -> output gain -> limiter
///
/// Filters run per channel, except that the speech bands can run on the mid
/// channel alone (see DspParameters::midSideEnabled). The auto gain,
/// compressor and limiter are all channel-linked.
///
/// Parameters are published from the control thread with a seqlock and picked
/// up by the audio thread at sub-block boundaries. Control values are ramped
/// rather than applied at once, so moving a slider does not click.
class DspChain : public IAudioProcessor {
public:
    void prepare(std::uint32_t sampleRate, std::uint32_t channels,
                 std::uint32_t maxFramesPerBlock) override;

    void process(float* audio, std::size_t frames, std::uint32_t channels) noexcept override;

    /// Callable from any thread at any time, including while audio is running.
    void setParameters(const DspParameters& parameters) noexcept;

    /// Bypass keeps the signal path and its latency identical, so an A/B
    /// comparison is not confounded by a timing change.
    void setBypass(bool bypass) noexcept { bypass_.store(bypass, std::memory_order_relaxed); }
    bool bypassed() const noexcept { return bypass_.load(std::memory_order_relaxed); }

    /// Milliseconds the chain adds. Comes entirely from the limiter look-ahead.
    double latencyMs() const noexcept;

    /// Current compressor gain reduction in dB, for metering. Never positive.
    double gainReductionDb() const noexcept;

    /// What the slow levelling stage is doing: the gain it currently holds, and
    /// the short-term loudness it measured to arrive at it.
    double autoGainDb() const noexcept { return autoGain_.currentGainDb(); }
    double measuredLufs() const noexcept { return autoGain_.measuredLufs(); }

    /// Safe to call from any thread while audio is running.
    LevelSnapshot levels() const noexcept;

    std::uint64_t limiterClippedSamples() const noexcept;

private:
    /// Copies the published parameters if they changed. Returns false when the
    /// writer was mid-update, in which case the previous targets stay in force
    /// for this sub-block: the audio thread never waits on the control thread.
    bool fetchParameters() noexcept;

    void updateSmoothedTargets() noexcept;

    /// True when a smoothed control value has moved far enough since the last
    /// rebuild to be worth redesigning filters for. Once a ramp has settled,
    /// which is the normal state, this stays false and the per-sub-block cost
    /// drops to nothing.
    bool coefficientsNeedRebuild() const noexcept;

    void rebuildCoefficients() noexcept;

    /// Per-channel filter bank for one stage of the chain.
    struct FilterStage {
        std::vector<Biquad> perChannel;
        void prepare(std::uint32_t channels);
        void setCoeffs(const BiquadCoeffs& coeffs) noexcept;
        void reset() noexcept;
    };

    std::uint32_t sampleRate_ = 48000;
    std::uint32_t channels_ = 2;

    FilterStage highpass_;
    FilterStage lowShelf_;
    std::array<FilterStage, kMaxSpeechBands> speechBands_;
    FilterStage highShelf_;
    AutoGain autoGain_;
    Compressor compressor_;
    Limiter limiter_;

    // Seqlock: the control thread bumps `paramSeq_` to an odd value, writes
    // `sharedParams_`, then bumps it to the next even value. An odd value or a
    // changed value across the copy means the reader saw a torn write.
    alignas(64) std::atomic<std::uint32_t> paramSeq_{0};
    DspParameters sharedParams_{};

    std::atomic<bool> bypass_{false};
    std::atomic<bool> parametersDirty_{false};

    /// Where the smoothing is heading, refreshed from `sharedParams_`.
    DspParameters target_{};
    /// Where the smoothing has got to.
    DspParameters current_{};
    /// What the filter coefficients were last built from.
    DspParameters built_{};
    bool coefficientsValid_ = false;

    double smoothingCoeff_ = 0.0;
    float inputGainLinear_ = 1.0f;
    float outputGainLinear_ = 1.0f;
    float sideGainLinear_ = 1.0f;

    /// Whether the speech bands are currently running on the mid channel.
    /// Tracked separately from the parameter so that a change can reset the
    /// filters: their state would otherwise carry mid samples into the left
    /// channel, or the other way round, and that is heard as a click.
    bool midSideActive_ = false;

    double meterDecayPerSubBlock_ = 0.0;
    std::atomic<float> meterInputPeak_{0.0f};
    std::atomic<float> meterOutputPeak_{0.0f};
};

}  // namespace audiolens::dsp
