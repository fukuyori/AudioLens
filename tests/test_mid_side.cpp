// Mid/side speech lift (DspParameters::midSideEnabled).
//
// The claim being tested is the one the feature exists for: a lift applied in
// the mid channel raises centred material without raising what is spread across
// the image. Testing that means feeding the chain two signals that differ only
// in where they sit in the stereo field.

#include "test_support.h"

#include "analysis/loudness.h"
#include "dsp/dsp_chain.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::analysis::bandLevelDbfs;
using audiolens::dsp::DspChain;
using audiolens::dsp::DspParameters;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;

/// `pan` of 0 puts the tone dead centre (L == R, so the side channel is empty);
/// 1 puts it entirely in the left channel, which is as much side as it is mid.
std::vector<float> makePannedSine(double freqHz, double amplitude, double seconds, double pan) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> data(frames * kChannels);
    const double step = 2.0 * std::numbers::pi * freqHz / kRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const double v = std::sin(step * static_cast<double>(f)) * amplitude;
        data[f * kChannels + 0] = static_cast<float>(v);
        data[f * kChannels + 1] = static_cast<float>(v * (1.0 - pan));
    }
    return data;
}

DspParameters makeLiftParameters(double speechGainDb, double sideGainDb, bool midSide) {
    DspParameters p;
    p.highpassEnabled = false;
    p.lowShelfGainDb = 0.0;
    p.highShelfGainDb = 0.0;
    p.speechBandCount = 1;
    p.speechBands[0] = {2000.0, 1.0, speechGainDb};
    p.midSideEnabled = midSide;
    p.sideGainDb = sideGainDb;
    p.compressorEnabled = false;
    p.limiter.ceilingDb = 0.0;
    return p;
}

std::vector<float> run(const DspParameters& parameters, std::vector<float> audio) {
    DspChain chain;
    chain.setParameters(parameters);
    chain.prepare(kRate, kChannels, 512);
    chain.process(audio.data(), audio.size() / kChannels, kChannels);
    return audio;
}

}  // namespace

AL_TEST(MidSide_lifts_centred_material_more_than_panned_material) {
    const DspParameters parameters = makeLiftParameters(8.0, 0.0, /*midSide=*/true);

    const auto centred = makePannedSine(2000.0, 0.2, 1.0, 0.0);
    const auto panned = makePannedSine(2000.0, 0.2, 1.0, 1.0);

    const double centredBefore = bandLevelDbfs(centred, kChannels, kRate, 2000.0, 2.0);
    const double pannedBefore = bandLevelDbfs(panned, kChannels, kRate, 2000.0, 2.0);

    const auto centredAfter = run(parameters, centred);
    const auto pannedAfter = run(parameters, panned);

    const double centredGain =
        bandLevelDbfs(centredAfter, kChannels, kRate, 2000.0, 2.0) - centredBefore;
    const double pannedGain =
        bandLevelDbfs(pannedAfter, kChannels, kRate, 2000.0, 2.0) - pannedBefore;

    // Centred material is all mid, so it gets the whole lift. Hard-panned
    // material is half mid and half side, so only half of it is lifted.
    CHECK_NEAR(centredGain, 8.0, 0.5);
    CHECK(pannedGain < centredGain - 2.0);
}

AL_TEST(MidSide_leaves_mono_material_untouched_by_the_side_trim) {
    // L == R means the side channel is exactly zero, so no amount of side gain
    // can change anything. This is what makes the stage safe to leave on.
    const auto mono = makePannedSine(1000.0, 0.2, 0.5, 0.0);

    const auto noTrim = run(makeLiftParameters(0.0, 0.0, true), mono);
    const auto heavyTrim = run(makeLiftParameters(0.0, -12.0, true), mono);

    double worst = 0.0;
    for (std::size_t i = 0; i < mono.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(noTrim[i] - heavyTrim[i])));
    }
    CHECK(worst < 1e-6);
}

AL_TEST(MidSide_reconstructs_exactly_when_it_has_nothing_to_do) {
    // Flat bands and unity side gain: the conversion to mid/side and back must
    // be transparent, or every preset that enables the stage pays for it with a
    // change it never asked for.
    const auto input = makePannedSine(700.0, 0.25, 0.25, 0.6);

    const auto through = run(makeLiftParameters(0.0, 0.0, true), input);
    const auto direct = run(makeLiftParameters(0.0, 0.0, false), input);

    double worst = 0.0;
    for (std::size_t i = 0; i < input.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(through[i] - direct[i])));
    }
    CHECK(worst < 1e-5);
}

AL_TEST(MidSide_side_trim_narrows_a_panned_signal) {
    const auto panned = makePannedSine(1000.0, 0.2, 0.5, 1.0);

    const auto trimmed = run(makeLiftParameters(0.0, -6.0, true), panned);

    // The signal was entirely in the left channel: mid and side were equal.
    // Halving the side must therefore have put a quarter of the original
    // amplitude into the right channel, in antiphase to what is left of it.
    double leftPeak = 0.0;
    double rightPeak = 0.0;
    for (std::size_t f = 0; f < trimmed.size() / kChannels; ++f) {
        leftPeak = std::max(leftPeak, static_cast<double>(std::abs(trimmed[f * kChannels + 0])));
        rightPeak = std::max(rightPeak, static_cast<double>(std::abs(trimmed[f * kChannels + 1])));
    }
    CHECK(rightPeak > 0.01);           // bled into the other channel
    CHECK(rightPeak < leftPeak * 0.6);  // but the image did not collapse
}
