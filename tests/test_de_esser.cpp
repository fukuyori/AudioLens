// Sibilance control (dsp::DeEsser).
//
// The stage has to do two things, and the second is the harder one: take the
// edge off an /s/, and leave everything that is not an /s/ alone. A de-esser
// that fails the second is just a treble cut, and would undo the clarity the
// rest of the chain exists to provide.

#include "test_support.h"

#include "analysis/loudness.h"
#include "dsp/de_esser.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::analysis::bandLevelDbfs;
using audiolens::dsp::DeEsser;
using audiolens::dsp::DeEsserSettings;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;

/// Deterministic noise limited to roughly one octave around `centreHz`, which
/// is what a sibilant looks like to a detector: most of the energy in a narrow
/// high band.
std::vector<float> makeBandNoise(double centreHz, double amplitude, double seconds,
                                 std::uint32_t seed = 7) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> data(frames * kChannels);

    std::uint32_t state = seed;
    // A one-pole pair either side of the centre is enough shaping for a test.
    double lowState = 0.0;
    double highState = 0.0;
    const double lowCoeff = std::exp(-2.0 * std::numbers::pi * (centreHz * 0.5) / kRate);
    const double highCoeff = std::exp(-2.0 * std::numbers::pi * (centreHz * 2.0) / kRate);

    for (std::size_t f = 0; f < frames; ++f) {
        state = state * 1664525u + 1013904223u;
        const double white = static_cast<double>(state >> 8) / 8388608.0 - 1.0;
        highState = white + highCoeff * (highState - white);   // low-passed
        lowState = highState + lowCoeff * (lowState - highState);
        const auto v = static_cast<float>((highState - lowState) * amplitude * 4.0);
        data[f * kChannels + 0] = v;
        data[f * kChannels + 1] = v;
    }
    return data;
}

std::vector<float> makeSine(double freqHz, double amplitude, double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> data(frames * kChannels);
    const double step = 2.0 * std::numbers::pi * freqHz / kRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        data[f * kChannels + 0] = v;
        data[f * kChannels + 1] = v;
    }
    return data;
}

DeEsserSettings defaultSettings() {
    DeEsserSettings s;
    s.enabled = true;
    return s;
}

std::vector<float> run(const DeEsserSettings& settings, std::vector<float> audio) {
    DeEsser deEsser;
    deEsser.prepare(kRate, kChannels);
    deEsser.setSettings(settings);
    for (std::size_t f = 0; f < audio.size() / kChannels; ++f) {
        deEsser.processFrame(audio.data() + f * kChannels, kChannels);
    }
    return audio;
}

}  // namespace

AL_TEST(DeEsser_reduces_a_sibilant_band) {
    const auto sibilant = makeBandNoise(6500.0, 0.2, 1.0);
    const double before = bandLevelDbfs(sibilant, kChannels, kRate, 6500.0, 1.0);

    const auto after = run(defaultSettings(), sibilant);
    const double level = bandLevelDbfs(after, kChannels, kRate, 6500.0, 1.0);

    CHECK(level < before - 3.0);
}

AL_TEST(DeEsser_leaves_a_vowel_alone) {
    // Nearly all the energy low down, which is what a vowel looks like. The
    // sibilant band holds almost nothing, so the ratio never crosses the
    // threshold and the stage must stay out of the way.
    const auto vowel = makeSine(700.0, 0.2, 0.5);
    const auto after = run(defaultSettings(), vowel);

    // Measured as a level rather than sample by sample. The onset of any sound
    // is a step, and a step has energy everywhere including the sibilant band,
    // so the detector does briefly see one at the very first samples. Every
    // de-esser does; what matters is whether the note itself comes out intact.
    const double before = bandLevelDbfs(vowel, kChannels, kRate, 700.0, 2.0);
    const double level = bandLevelDbfs(after, kChannels, kRate, 700.0, 2.0);
    CHECK_NEAR(level, before, 0.2);
}

AL_TEST(DeEsser_judges_by_ratio_not_by_level) {
    // The same sibilant material 20 dB apart. An absolute threshold would act
    // on one and not the other, which would be wrong: a quiet /s/ is every bit
    // as sharp as a loud one.
    const auto loud = makeBandNoise(6500.0, 0.3, 1.0);
    const auto quiet = makeBandNoise(6500.0, 0.03, 1.0);

    const double loudGain = bandLevelDbfs(run(defaultSettings(), loud), kChannels, kRate, 6500.0,
                                          1.0) -
                            bandLevelDbfs(loud, kChannels, kRate, 6500.0, 1.0);
    const double quietGain = bandLevelDbfs(run(defaultSettings(), quiet), kChannels, kRate, 6500.0,
                                           1.0) -
                             bandLevelDbfs(quiet, kChannels, kRate, 6500.0, 1.0);

    CHECK(loudGain < -3.0);
    CHECK_NEAR(loudGain, quietGain, 1.0);
}

AL_TEST(DeEsser_honours_its_reduction_limit) {
    DeEsserSettings s = defaultSettings();
    s.maxReductionDb = 3.0;
    s.amount = 1.0;
    s.thresholdDb = -30.0;  // guarantees the limit is what stops it, not the input

    const auto sibilant = makeBandNoise(6500.0, 0.2, 1.0);
    const double before = bandLevelDbfs(sibilant, kChannels, kRate, 6500.0, 1.0);
    const double after = bandLevelDbfs(run(s, sibilant), kChannels, kRate, 6500.0, 1.0);

    CHECK(after > before - 3.5);
}

AL_TEST(DeEsser_is_inert_when_disabled) {
    DeEsserSettings s = defaultSettings();
    s.enabled = false;

    const auto sibilant = makeBandNoise(6500.0, 0.2, 0.5);
    const auto after = run(s, sibilant);

    double worst = 0.0;
    for (std::size_t i = 0; i < sibilant.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(after[i] - sibilant[i])));
    }
    CHECK(worst < 1e-9);
}
