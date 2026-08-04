#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audiolens::dsp {

struct LimiterSettings {
    double ceilingDb = -1.0;
    double lookaheadMs = 2.0;
    double releaseMs = 60.0;

    bool operator==(const LimiterSettings&) const = default;
};

/// Look-ahead brickwall limiter, channel-linked.
///
/// This is the safety device required by N-05: nothing downstream of it may
/// exceed the ceiling, whatever the presets ask for. It works by delaying the
/// audio by the look-ahead window and driving the gain from the *minimum*
/// required gain over that window, so the reduction is already fully applied by
/// the time the peak it was computed for reaches the output.
class Limiter {
public:
    /// Allocates for a look-ahead of up to `kMaxLookaheadMs`. Later changes to
    /// LimiterSettings::lookaheadMs re-use that allocation, which keeps
    /// setSettings() callable from the audio thread.
    void prepare(double sampleRate, std::uint32_t channels);
    void reset() noexcept;

    /// Safe to call from the audio thread: never allocates.
    void setSettings(const LimiterSettings& settings) noexcept;
    const LimiterSettings& settings() const noexcept { return settings_; }

    /// Processes one interleaved frame in place. Output is delayed by exactly
    /// `latencyFrames()` relative to input.
    void processFrame(float* frame, std::uint32_t channels) noexcept;

    std::size_t latencyFrames() const noexcept { return lookaheadFrames_; }

    /// Samples the final hard clip had to catch. A correct limiter leaves this
    /// at zero; a non-zero count means the gain computation let something
    /// through and is worth investigating rather than silently tolerating.
    std::uint64_t clippedSamples() const noexcept { return clippedSamples_; }

private:
    /// Pushes one gain value into the sliding window and returns the minimum
    /// across it. Backed by a monotonic deque, so each sample costs amortised
    /// O(1) rather than O(window).
    double pushAndGetMinimum(double gain) noexcept;

    static constexpr double kMaxLookaheadMs = 20.0;

    LimiterSettings settings_;
    double sampleRate_ = 48000.0;
    std::uint32_t channels_ = 2;
    double ceilingLinear_ = 1.0;
    double releaseCoeff_ = 0.0;
    double attackStep_ = 1.0;

    std::size_t maxLookaheadFrames_ = 0;
    std::size_t lookaheadFrames_ = 0;

    /// Interleaved, exactly `lookaheadFrames_` frames long in use. Reading a
    /// slot before overwriting it yields the frame from that many frames ago,
    /// which is what makes the delay equal the look-ahead window rather than
    /// one more than it.
    std::vector<float> delayLine_;
    std::size_t delayWrite_ = 0;

    // Monotonic deque of (gain, arrival index), increasing in gain from front.
    std::vector<double> queueGain_;
    std::vector<std::uint64_t> queueIndex_;
    std::size_t queueHead_ = 0;
    std::size_t queueTail_ = 0;
    std::uint64_t sampleIndex_ = 0;

    double currentGain_ = 1.0;
    std::uint64_t clippedSamples_ = 0;
};

}  // namespace audiolens::dsp
