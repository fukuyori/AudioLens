#pragma once

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

    double highShelfFreqHz = 8000.0;
    double highShelfGainDb = 0.0;
    double highShelfQ = 0.707;

    bool compressorEnabled = false;
    CompressorSettings compressor{};

    double outputGainDb = 0.0;
    LimiterSettings limiter{};
};

}  // namespace audiolens::dsp
