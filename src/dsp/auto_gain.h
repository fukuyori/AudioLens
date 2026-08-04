#pragma once

#include "dsp/biquad.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audiolens::dsp {

/// Settings for the slow levelling stage. Times are in seconds' worth of dB.
struct AutoGainSettings {
    bool enabled = false;

    /// Where the programme's average loudness should end up, in LUFS.
    double targetLufs = -18.0;

    double maxBoostDb = 12.0;
    double maxCutDb = 12.0;

    /// Deliberately glacial, and that is the whole design.
    ///
    /// Any level-following gain that can move as fast as the programme does
    /// will move *against* it: the measurement lags, so the gain arrives after
    /// the passage that provoked it and lands on the next one instead. Measured
    /// on material alternating every four seconds, a stage at 1 dB/s made the
    /// loudness range wider than leaving it switched off.
    ///
    /// The scale that works is minutes, not seconds. Even with a half-minute
    /// average behind it, a fifth of a dB per second was still quick enough to
    /// follow the ripple left in that average by ordinary programme dynamics,
    /// and following it widened the range. Three dB per minute cannot: across
    /// any one passage it is nothing, while over a couple of minutes it still
    /// covers the ten dB between one programme and the next, which is the only
    /// difference this stage exists to remove.
    ///
    /// Turning down stays quicker than turning up: a late reduction is heard as
    /// a blast, a late increase merely as taking a moment to come up.
    double riseDbPerSecond = 0.05;
    double fallDbPerSecond = 0.10;

    /// Below this the gain is held rather than adjusted. Without it, silence
    /// between scenes would be treated as something to lift, and the room tone
    /// and hiss underneath it would be hauled up to conversational level.
    double gateLufs = -55.0;

    /// How much audio the loudness average is taken over.
    ///
    /// This is the setting that decides what the stage is *for*, and getting it
    /// wrong is not a matter of taste. Built first on BS.1770's three second
    /// short-term window — convenient, and wrong — it saw every swing inside
    /// the programme and chased them, arriving late enough to add gain to the
    /// loud passages and take it off the quiet ones. Measured on material
    /// alternating every four seconds it *widened* the loudness range, and no
    /// rate limit fixed that, because the fault was in what was being measured
    /// rather than how fast the answer was acted on.
    ///
    /// Half a minute is long enough that ordinary programme dynamics average
    /// out and what is left is the level of the programme itself, which is the
    /// only thing this stage should ever respond to.
    double windowSeconds = 30.0;

    friend bool operator==(const AutoGainSettings&, const AutoGainSettings&) = default;
};

/// Slow, loudness-targeting gain — the other half of "even out the volume".
///
/// The compressor already in the chain works on a 10 ms window, which is the
/// right scale for the dynamics inside a phrase. It is the wrong scale for the
/// thing listeners actually complain about: a quiet dialogue scene followed by
/// an action scene twenty seconds later. No attack and release short enough to
/// shape a syllable can also span half a minute.
///
/// Broadcast levelling always uses two stages for this reason, and this is the
/// slow one. It takes the K-weighted mean square of the last half minute — the
/// programme's own level, with its internal dynamics averaged away — and walks
/// a broadband gain towards whatever would put that on target, at a rate slow
/// enough to be inaudible as movement.
///
/// The measurement is feedforward: it looks at what arrives, not at what leaves
/// after the gain has been applied. A feedback arrangement would have to be
/// tuned against its own loop, and there is nothing to gain from that here.
class AutoGain {
public:
    void prepare(std::uint32_t sampleRate, std::uint32_t channels);

    /// Safe to call from the audio thread: it only recomputes two scalars.
    void setSettings(const AutoGainSettings& settings) noexcept;

    void reset() noexcept;

    /// Measures one frame and returns the linear gain that frame should get.
    /// The caller applies it; this way the meter sees the measurement and the
    /// gain from the same instant.
    float nextGain(const float* frame, std::uint32_t channels) noexcept;

    double currentGainDb() const noexcept { return currentGainDb_; }

    /// The loudness average the stage is currently aiming at, or a very
    /// negative number before enough audio has accumulated to have one.
    double measuredLufs() const noexcept { return measuredLufs_; }

private:
    void closeBlock() noexcept;
    void updateRates() noexcept;
    void resizeWindow() noexcept;

    AutoGainSettings settings_{};

    std::uint32_t sampleRate_ = 48000;
    std::uint32_t channels_ = 2;

    // K-weighting, one pair of stages per channel.
    std::vector<Biquad> shelf_;
    std::vector<Biquad> highpass_;

    /// The average is kept as a ring of 100 ms block mean squares rather than a
    /// ring of samples: three hundred doubles instead of half a minute of
    /// audio, and the total updates in constant time however long the window.
    static constexpr double kBlockSeconds = 0.1;

    double blockSquareSum_ = 0.0;
    std::size_t blockFrames_ = 4800;
    std::size_t blockFrameCount_ = 0;

    std::vector<double> window_;
    std::size_t windowIndex_ = 0;
    std::size_t windowFilled_ = 0;
    double windowSum_ = 0.0;

    double targetGainDb_ = 0.0;
    double currentGainDb_ = 0.0;
    double riseStepPerFrame_ = 0.0;
    double fallStepPerFrame_ = 0.0;
    double measuredLufs_ = -1e9;
};

}  // namespace audiolens::dsp
