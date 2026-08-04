#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audiolens::dsp {

/// Variable-ratio sample rate converter, windowed-sinc polyphase.
///
/// It does two jobs at once, which is why it exists in this shape:
///
///  1. Converts between a capture endpoint and a render endpoint running at
///     different rates. Without it the engine can only refuse to start, which
///     is what requirement N-03 rules out.
///  2. Absorbs clock drift. The two endpoints run on independent crystals, so
///     the ratio needed is never exactly nominal and never constant. Trimming
///     the ratio by a few parts per million is inaudible, where dropping or
///     repeating whole frames is a discontinuity.
///
/// Input is pushed in and output pulled out; the object owns the buffering
/// between them, so a caller does not have to work out how many input frames a
/// given number of output frames needs. Nothing allocates after prepare().
class Resampler {
public:
    /// `outputPerInputRatio` is outputRate / inputRate, e.g. 44100.0/48000.0.
    /// `maxOutputFrames` sizes the internal buffers for the largest single pull.
    void prepare(std::uint32_t channels, double outputPerInputRatio,
                 std::size_t maxOutputFrames);

    void reset() noexcept;

    /// Multiplies the base ratio. Clamped to a couple of percent: the
    /// anti-aliasing kernel is designed for the base ratio and stops being
    /// correct if that moves far.
    void setRatioTrim(double trim) noexcept;
    double ratioTrim() const noexcept { return trim_; }

    /// Input frames that must still be pushed before `outputFrames` can be
    /// pulled. Rounded up, so acting on it never leaves the resampler short.
    std::size_t inputFramesWanted(std::size_t outputFrames) const noexcept;

    void pushInput(const float* interleaved, std::size_t frames) noexcept;
    void pushSilence(std::size_t frames) noexcept;

    /// Produces up to `frames` output frames. Returns fewer when starved of
    /// input, which the caller should treat as an underrun.
    std::size_t pullOutput(float* interleaved, std::size_t frames) noexcept;

    /// Group delay, in input frames. Half the kernel width.
    std::size_t latencyFrames() const noexcept { return halfTaps_; }

    std::uint32_t channels() const noexcept { return channels_; }

private:
    void buildKernel(double ratio);
    void compact() noexcept;
    std::size_t availableInputFrames() const noexcept { return writeIndex_ - readIndex_; }

    /// Kernel taps per phase. 32 puts the stopband below -90 dB for the rate
    /// pairs consumer hardware actually uses.
    static constexpr std::size_t kTaps = 32;
    /// Phases in the table. Adjacent phases are interpolated, so this does not
    /// have to be large enough to make quantisation inaudible on its own.
    static constexpr std::size_t kPhases = 512;

    std::uint32_t channels_ = 2;
    std::size_t halfTaps_ = kTaps / 2;

    double baseRatio_ = 1.0;
    double trim_ = 1.0;
    double step_ = 1.0;  ///< Input frames advanced per output frame.

    /// kernel_[phase * kTaps + tap]. One extra phase row so interpolating at
    /// the last phase needs no wrap-around branch.
    std::vector<float> kernel_;

    /// Interleaved input FIFO. `readIndex_` and `writeIndex_` are frame indices
    /// into it; compact() slides the live region back to the front.
    std::vector<float> fifo_;
    std::size_t readIndex_ = 0;
    std::size_t writeIndex_ = 0;
    std::size_t capacityFrames_ = 0;

    /// Fractional read position as an absolute frame index into `fifo_`.
    /// compact() shifts it along with the data.
    double position_ = 0.0;

    std::vector<float> tapScratch_;
};

}  // namespace audiolens::dsp
