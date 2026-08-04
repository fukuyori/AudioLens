#pragma once

#include "dsp/biquad.h"

#include <cstdint>
#include <vector>

namespace audiolens::dsp {

struct DeEsserSettings {
    bool enabled = false;

    /// Centre of the band that gets taken out. Sibilance in speech sits roughly
    /// between 5 and 9 kHz; the exact place varies by voice and by microphone.
    double frequencyHz = 6500.0;
    double q = 1.0;

    /// How much louder the sibilant band has to be, relative to the whole
    /// signal, before anything happens.
    ///
    /// Deliberately a *ratio* rather than an absolute level. An absolute
    /// threshold would make the stage fire on anything loud and sit idle on
    /// anything quiet, which has nothing to do with whether the sound is harsh;
    /// a quiet /s/ is as sharp as a loud one. The ratio is what actually
    /// distinguishes a sibilant from a vowel, and it does not move when the
    /// programme level does.
    double thresholdDb = -14.0;

    /// Fraction of the excess above the threshold that is removed.
    double amount = 0.7;

    double maxReductionDb = 8.0;

    /// Fast enough to catch the start of an /s/, slow enough to let go before
    /// the following vowel — a release that hangs on turns every word after a
    /// sibilant dull.
    double attackMs = 1.0;
    double releaseMs = 40.0;

    bool operator==(const DeEsserSettings&) const = default;
};

/// Takes the edge off sibilants, without dulling anything else.
///
/// This exists because of what the rest of the chain does. Lifting the speech
/// range is how AudioLens makes consonants easier to make out, and the presets
/// that lift it hardest add six decibels around 2.6 kHz and another few above
/// that. Those are the same frequencies an /s/ lives in, so every preset that
/// improves intelligibility also sharpens sibilants, and an hour of listening
/// to sharpened sibilants is tiring. The「講義」preset promises the opposite of
/// that in its own description.
///
/// One bandpass does both jobs: its output is the detector's input *and* the
/// signal subtracted from the original. Removing a scaled copy of a fixed band
/// gives a notch whose depth varies per sample without redesigning any filter —
/// at the centre frequency, `x - g·x` is simply `(1-g)·x`.
///
/// Detection is channel-linked, like the compressor and for the same reason:
/// a sibilant ducked on one side only would pull the image sideways.
class DeEsser {
public:
    void prepare(double sampleRate, std::uint32_t channels);
    void reset() noexcept;

    void setSettings(const DeEsserSettings& settings) noexcept;
    const DeEsserSettings& settings() const noexcept { return settings_; }

    /// Processes one interleaved frame in place.
    void processFrame(float* frame, std::uint32_t channels) noexcept;

    /// Reduction currently applied to the band, in dB. Never positive.
    double currentReductionDb() const noexcept { return -smoothedReductionDb_; }

private:
    void updateCoefficients() noexcept;

    DeEsserSettings settings_{};
    double sampleRate_ = 48000.0;
    std::uint32_t channels_ = 2;

    std::vector<Biquad> band_;

    /// Mean squares, one-pole smoothed. Both use the same short time constant
    /// so the ratio between them is not distorted by one lagging the other.
    double bandMeanSquare_ = 0.0;
    double fullMeanSquare_ = 0.0;
    double detectorCoeff_ = 0.0;

    double smoothedReductionDb_ = 0.0;  ///< Positive; negated for reporting.
    double attackCoeff_ = 0.0;
    double releaseCoeff_ = 0.0;
};

}  // namespace audiolens::dsp
