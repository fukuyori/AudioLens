#pragma once

#include "dsp/auto_gain.h"
#include "dsp/compressor.h"
#include "dsp/limiter.h"

#include <cstdint>

namespace audiolens::dsp {

/// Peaking bands used to lift the speech range. Three is enough to shape the
/// 1-4 kHz region without the result sounding like a formant filter.
inline constexpr int kMaxSpeechBands = 3;

struct SpeechBand {
    double freqHz = 2000.0;
    double q = 1.0;
    double gainDb = 0.0;
};

/// Fully resolved DSP settings: what the chain actually runs, with every slider
/// and preset already folded in. Deliberately a fixed-size POD so it can be
/// handed to the audio thread without allocating.
struct DspParameters {
    double inputGainDb = 0.0;

    bool highpassEnabled = false;
    double highpassFreqHz = 80.0;
    double highpassQ = 0.707;

    double lowShelfFreqHz = 200.0;
    double lowShelfGainDb = 0.0;
    double lowShelfQ = 0.707;

    int speechBandCount = 0;
    SpeechBand speechBands[kMaxSpeechBands]{};

    /// Run the speech bands on the mid channel only, and trim the side channel
    /// by `sideGainDb`. Stereo only; ignored otherwise.
    ///
    /// Dialogue in film and broadcast is almost always panned dead centre,
    /// while music and effects are spread across the image. Splitting the pair
    /// into mid (L+R)/2 and side (L-R)/2 therefore separates speech from most
    /// of what competes with it, for the cost of two adds. Lifting the speech
    /// range in the mid channel alone means the same lift no longer drags the
    /// music up with it, which is what happens when the bands run on L and R.
    ///
    /// Mono material has no side channel at all, so this reduces to the plain
    /// per-channel path on its own.
    bool midSideEnabled = false;

    /// Negative pushes the centre forward by holding everything else back.
    /// A few dB is plenty: past that the image collapses towards mono and, on
    /// headphones especially, that is more noticeable than the benefit.
    double sideGainDb = 0.0;

    double highShelfFreqHz = 8000.0;
    double highShelfGainDb = 0.0;
    double highShelfQ = 0.707;

    /// Slow levelling, ahead of the compressor. The two work on different time
    /// scales on purpose: this one on scene-to-scene differences, the
    /// compressor on the dynamics inside a phrase.
    AutoGainSettings autoGain{};

    bool compressorEnabled = false;
    CompressorSettings compressor{};

    double outputGainDb = 0.0;
    LimiterSettings limiter{};
};

}  // namespace audiolens::dsp
