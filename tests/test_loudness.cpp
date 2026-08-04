#include "test_support.h"

#include "analysis/loudness.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::analysis::bandLevelDbfs;
using audiolens::analysis::LoudnessResult;
using audiolens::analysis::measureLoudness;

namespace {

constexpr std::uint32_t kRate = 48000;

std::vector<float> makeSine(double freqHz, double amplitude, double seconds,
                            std::uint32_t channels, std::uint32_t sampleRate = kRate) {
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> data(frames * channels);
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        for (std::uint32_t c = 0; c < channels; ++c) {
            data[f * channels + c] = v;
        }
    }
    return data;
}

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

}  // namespace

AL_TEST(Loudness_matches_the_BS1770_reference_for_a_1kHz_tone) {
    // BS.1770-4 calibrates the -0.691 offset so that a 1 kHz sine at 0 dBFS on
    // a single channel reads -3.01 LKFS. Two identical channels sum to 0.0.
    // This one number validates the K-weighting, the block gating and the
    // scaling all at once.
    const std::vector<float> stereo = makeSine(1000.0, 1.0, 5.0, 2);
    const LoudnessResult r = measureLoudness(stereo, 2, kRate);
    CHECK(r.valid());
    CHECK_NEAR(r.integratedLufs, 0.0, 0.1);

    const std::vector<float> mono = makeSine(1000.0, 1.0, 5.0, 1);
    CHECK_NEAR(measureLoudness(mono, 1, kRate).integratedLufs, -3.01, 0.1);
}

AL_TEST(Loudness_tracks_amplitude_one_for_one) {
    for (const double levelDb : {-40.0, -23.0, -20.0, -10.0}) {
        const std::vector<float> audio = makeSine(1000.0, dbToLinear(levelDb), 5.0, 2);
        CHECK_NEAR(measureLoudness(audio, 2, kRate).integratedLufs, levelDb, 0.1);
    }
}

AL_TEST(Loudness_applies_K_weighting_across_frequency) {
    // K-weighting is deliberately not flat, and the expected numbers come from
    // the two filters BS.1770 defines rather than from taste:
    //
    //   40 Hz  : RLB highpass (f0 38.1 Hz, Q 0.50) gives -5.6 dB; shelf ~0 dB
    //   1 kHz  : RLB ~0 dB; shelf gives +0.67 dB
    //   10 kHz : RLB ~0 dB; shelf at its full +4 dB
    //
    // so 40 Hz sits 6.3 dB below 1 kHz, and 10 kHz sits 3.3 dB above it.
    const std::vector<float> low = makeSine(40.0, dbToLinear(-20.0), 5.0, 2);
    const std::vector<float> mid = makeSine(1000.0, dbToLinear(-20.0), 5.0, 2);
    const std::vector<float> high = makeSine(10000.0, dbToLinear(-20.0), 5.0, 2);

    const double lowLufs = measureLoudness(low, 2, kRate).integratedLufs;
    const double midLufs = measureLoudness(mid, 2, kRate).integratedLufs;
    const double highLufs = measureLoudness(high, 2, kRate).integratedLufs;

    CHECK_NEAR(midLufs - lowLufs, 6.27, 0.5);
    CHECK_NEAR(highLufs - midLufs, 3.33, 0.5);
}

AL_TEST(Loudness_reports_sample_peak) {
    const std::vector<float> audio = makeSine(1000.0, 0.5, 1.0, 2);
    CHECK_NEAR(measureLoudness(audio, 2, kRate).samplePeakDbfs, -6.02, 0.1);
}

AL_TEST(Loudness_gate_ignores_silence) {
    // A tone followed by silence must read the same as the tone alone: without
    // the gate, the silence would drag the integrated figure down.
    std::vector<float> toneOnly = makeSine(1000.0, dbToLinear(-20.0), 5.0, 2);
    std::vector<float> tonePlusSilence = toneOnly;
    tonePlusSilence.resize(tonePlusSilence.size() * 3, 0.0f);

    const double a = measureLoudness(toneOnly, 2, kRate).integratedLufs;
    const double b = measureLoudness(tonePlusSilence, 2, kRate).integratedLufs;
    CHECK_NEAR(b, a, 0.5);
}

AL_TEST(Loudness_returns_invalid_for_silence) {
    const std::vector<float> silence(kRate * 2 * 2, 0.0f);
    CHECK(!measureLoudness(silence, 2, kRate).valid());
}

AL_TEST(Loudness_range_is_near_zero_for_a_steady_tone) {
    const std::vector<float> audio = makeSine(1000.0, dbToLinear(-20.0), 20.0, 2);
    const LoudnessResult r = measureLoudness(audio, 2, kRate);
    CHECK(r.loudnessRangeLu < 1.0);
}

AL_TEST(Loudness_range_grows_when_levels_alternate) {
    // Ten-second stretches at -30 and -12 dBFS: a 18 dB swing, which is the
    // shape of the problem the「音量差」slider exists to solve.
    std::vector<float> audio;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const double levelDb : {-30.0, -12.0}) {
            const std::vector<float> section = makeSine(1000.0, dbToLinear(levelDb), 10.0, 2);
            audio.insert(audio.end(), section.begin(), section.end());
        }
    }

    const LoudnessResult r = measureLoudness(audio, 2, kRate);
    CHECK(r.loudnessRangeLu > 12.0);
}

AL_TEST(Loudness_works_at_44100) {
    // The K-weighting coefficients in the standard are given for 48 kHz only;
    // these are derived from the analog prototype, so other rates must agree.
    const std::vector<float> audio = makeSine(1000.0, 1.0, 5.0, 2, 44100);
    CHECK_NEAR(measureLoudness(audio, 2, 44100).integratedLufs, 0.0, 0.15);
}

AL_TEST(BandLevel_finds_energy_where_the_tone_is) {
    const std::vector<float> audio = makeSine(1000.0, dbToLinear(-12.0), 2.0, 2);

    const double atTone = bandLevelDbfs(audio, 2, kRate, 1000.0, 1.4);
    const double wellBelow = bandLevelDbfs(audio, 2, kRate, 100.0, 1.4);
    const double wellAbove = bandLevelDbfs(audio, 2, kRate, 8000.0, 1.4);

    CHECK(atTone > wellBelow + 20.0);
    CHECK(atTone > wellAbove + 20.0);
}
