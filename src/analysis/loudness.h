#pragma once

#include <cstdint>
#include <vector>

namespace audiolens::analysis {

struct LoudnessResult {
    /// Gated integrated loudness, ITU-R BS.1770-4. -inf when everything was
    /// below the absolute gate.
    double integratedLufs = -1e9;

    /// EBU R128 loudness range: the spread between the 10th and 95th percentile
    /// of short-term loudness. This is the number that says whether the quiet
    /// and loud passages have been brought closer together, which is exactly
    /// what the「音量差」slider claims to do.
    double loudnessRangeLu = 0.0;

    /// Loudest 400 ms block, ungated.
    double maxMomentaryLufs = -1e9;

    /// Largest absolute sample value, in dBFS.
    double samplePeakDbfs = -1e9;

    bool valid() const { return integratedLufs > -1e8; }
};

/// Measures interleaved float audio.
///
/// Channel weighting follows BS.1770 for up to five channels (L, R, C at 1.0,
/// surrounds at 1.41). Anything beyond that is weighted 1.0.
LoudnessResult measureLoudness(const std::vector<float>& interleaved, std::uint32_t channels,
                               std::uint32_t sampleRate);

/// Short-term loudness (3 s window, 100 ms hop), the series the loudness range
/// is computed from. Useful for plotting how a preset flattens a track.
std::vector<double> shortTermLoudnessSeries(const std::vector<float>& interleaved,
                                            std::uint32_t channels, std::uint32_t sampleRate);

/// RMS level in dBFS within one band, measured with a bandpass rather than an
/// FFT. Lets a test assert that an EQ moved the energy it was supposed to.
double bandLevelDbfs(const std::vector<float>& interleaved, std::uint32_t channels,
                     std::uint32_t sampleRate, double centreHz, double q);

}  // namespace audiolens::analysis
