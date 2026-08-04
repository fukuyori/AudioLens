#pragma once

#include <cstdint>

namespace audiolens::dsp {

struct CompressorSettings {
    /// Levels here are RMS, not peak: the detector measures loudness rather
    /// than waveform excursion. Protecting against excursion is the limiter's
    /// job; this stage exists to even out how loud things *sound*.
    double thresholdDb = -28.0;
    double ratio = 4.0;
    double kneeDb = 8.0;  ///< Total width of the soft knee, centred on the threshold.
    double attackMs = 10.0;
    double releaseMs = 250.0;

    /// Programme level the auto makeup gain is calibrated for. Makeup is set to
    /// exactly undo the compression applied at this level, so material sitting
    /// around it comes out at the loudness it went in at while the peaks above
    /// and the dips below are pulled toward it.
    double makeupReferenceDb = -18.0;

    bool operator==(const CompressorSettings&) const = default;
};

/// Wideband, channel-linked, soft-knee compressor.
///
/// Channel linking (one gain derived from the summed channels and applied to
/// all) is what keeps the stereo image from wandering when something loud
/// happens on one side, which matters for the dialogue-levelling presets.
class Compressor {
public:
    void prepare(double sampleRate) noexcept;
    void reset() noexcept;

    void setSettings(const CompressorSettings& settings) noexcept;
    const CompressorSettings& settings() const noexcept { return settings_; }

    /// Processes one interleaved frame in place.
    void processFrame(float* frame, std::uint32_t channels) noexcept;

    /// Gain currently applied, in dB (negative means reduction). For metering.
    double currentGainDb() const noexcept { return smoothedGainDb_; }

    /// Static transfer curve: the gain, in dB, that a steady input at RMS level
    /// `inputDb` eventually receives. Independent of attack and release, which
    /// makes it directly testable.
    double staticGainDb(double inputDb) const noexcept;

private:
    void updateCoefficients() noexcept;

    /// RMS window. Short enough that the attack and release settings still
    /// govern how the compressor feels, long enough that a bass note does not
    /// make the gain follow its waveform.
    static constexpr double kDetectorMs = 10.0;

    CompressorSettings settings_;
    double sampleRate_ = 48000.0;
    double detectorCoeff_ = 0.0;
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
    double makeupDb_ = 0.0;
    double meanSquare_ = 0.0;
    double smoothedGainDb_ = 0.0;
};

}  // namespace audiolens::dsp
