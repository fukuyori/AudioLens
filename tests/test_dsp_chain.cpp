#include "test_support.h"

#include "analysis/loudness.h"
#include "core/preset.h"
#include "dsp/dsp_chain.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::builtinPresets;
using audiolens::findBuiltinPreset;
using audiolens::Preset;
using audiolens::resolveParameters;
using audiolens::SliderValues;
using audiolens::analysis::bandLevelDbfs;
using audiolens::analysis::measureLoudness;
using audiolens::dsp::DspChain;
using audiolens::dsp::DspParameters;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;

double dbToLinear(double db) { return std::pow(10.0, db / 20.0); }

std::vector<float> makeSine(double freqHz, double amplitude, double seconds) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> data(frames * kChannels);
    const double step = 2.0 * std::numbers::pi * freqHz / kRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            data[f * kChannels + c] = v;
        }
    }
    return data;
}

/// Deterministic broadband noise. A fixed generator keeps the tests
/// reproducible; std::random_device would make a failure impossible to repeat.
std::vector<float> makeNoise(double amplitude, double seconds, std::uint32_t seed = 12345) {
    const auto frames = static_cast<std::size_t>(seconds * kRate);
    std::vector<float> data(frames * kChannels);
    std::uint32_t state = seed;
    for (std::size_t f = 0; f < frames; ++f) {
        state = state * 1664525u + 1013904223u;
        const double v = (static_cast<double>(state >> 8) / 8388608.0 - 1.0) * amplitude;
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            data[f * kChannels + c] = static_cast<float>(v);
        }
    }
    return data;
}

/// Alternating quiet and loud sections: the shape of a film soundtrack, and the
/// input the levelling presets are meant to tame.
///
/// The 18 dB gap is deliberate. EBU R128 discards short-term blocks more than
/// 20 LU below the mean, so material with a wider spread than that has its
/// quiet passages gated out of the loudness range entirely and the figure stops
/// reflecting what a listener would complain about.
std::vector<float> makeDynamicMaterial() {
    std::vector<float> audio;
    for (int repeat = 0; repeat < 3; ++repeat) {
        for (const double levelDb : {-28.0, -10.0}) {
            const std::vector<float> section = makeNoise(dbToLinear(levelDb), 8.0,
                                                         static_cast<std::uint32_t>(repeat + 1));
            audio.insert(audio.end(), section.begin(), section.end());
        }
    }
    return audio;
}

/// RMS of the second half of a buffer, in dBFS. Skipping the first half lets
/// filter and envelope transients settle before anything is measured.
double settledRmsDbfs(const std::vector<float>& audio) {
    const std::size_t total = audio.size();
    const std::size_t start = total / 2;
    double sumOfSquares = 0.0;
    for (std::size_t i = start; i < total; ++i) {
        sumOfSquares += static_cast<double>(audio[i]) * audio[i];
    }
    const double meanSquare = sumOfSquares / static_cast<double>(total - start);
    return meanSquare > 0.0 ? 10.0 * std::log10(meanSquare) : -1e9;
}

std::vector<float> runChain(const DspParameters& params, std::vector<float> audio) {
    DspChain chain;
    chain.setParameters(params);
    chain.prepare(kRate, kChannels, 512);
    chain.process(audio.data(), audio.size() / kChannels, kChannels);
    return audio;
}

std::vector<float> runPreset(const Preset& preset, const SliderValues& sliders,
                             std::vector<float> audio) {
    return runChain(resolveParameters(preset, sliders), std::move(audio));
}

/// RMS of one channel of an interleaved buffer, in dBFS, over the settled half.
double settledChannelDbfs(const std::vector<float>& audio, std::uint32_t channel) {
    const std::size_t frames = audio.size() / kChannels;
    double sumOfSquares = 0.0;
    for (std::size_t f = frames / 2; f < frames; ++f) {
        const double s = audio[f * kChannels + channel];
        sumOfSquares += s * s;
    }
    const double meanSquare = sumOfSquares / static_cast<double>(frames - frames / 2);
    return meanSquare > 0.0 ? 10.0 * std::log10(meanSquare) : -1e9;
}

}  // namespace

AL_TEST(DspChain_with_neutral_parameters_barely_changes_the_signal) {
    DspParameters params;  // All gains zero, compressor off, highpass off.
    params.limiter.ceilingDb = -1.0;

    const std::vector<float> input = makeSine(1000.0, dbToLinear(-20.0), 2.0);
    const std::vector<float> output = runChain(params, input);

    const double before = measureLoudness(input, kChannels, kRate).integratedLufs;
    const double after = measureLoudness(output, kChannels, kRate).integratedLufs;
    CHECK_NEAR(after, before, 0.2);
}

// The EQ tests drive one sine at a time rather than noise through a bandpass.
// A second-order bandpass has skirts wide enough that neighbouring energy
// dominates the reading once a filter has cut 20 dB, which would make these
// assertions measure the probe rather than the chain.

AL_TEST(DspChain_bass_slider_removes_low_frequency_energy) {
    const Preset* preset = findBuiltinPreset("night");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const std::vector<float> low = makeSine(60.0, dbToLinear(-20.0), 2.0);
    const double lowOff = settledRmsDbfs(runPreset(*preset, {0, 0, 0}, low));
    const double lowOn = settledRmsDbfs(runPreset(*preset, {100, 0, 0}, low));

    // The bass slider must cut the bottom substantially...
    CHECK(lowOn < lowOff - 10.0);

    // ...without disturbing the range the voice lives in.
    const std::vector<float> mid = makeSine(1000.0, dbToLinear(-20.0), 2.0);
    const double midOff = settledRmsDbfs(runPreset(*preset, {0, 0, 0}, mid));
    const double midOn = settledRmsDbfs(runPreset(*preset, {100, 0, 0}, mid));
    CHECK_NEAR(midOn, midOff, 1.0);
}

AL_TEST(DspChain_clarity_slider_lifts_the_speech_range) {
    const Preset* preset = findBuiltinPreset("conversation");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const std::vector<float> speech = makeSine(2600.0, dbToLinear(-20.0), 2.0);
    const double speechFlat = settledRmsDbfs(runPreset(*preset, {0, 0, 0}, speech));
    const double speechLifted = settledRmsDbfs(runPreset(*preset, {0, 100, 0}, speech));
    CHECK(speechLifted > speechFlat + 3.0);

    // The clarity slider must not reach down into the bass.
    const std::vector<float> low = makeSine(150.0, dbToLinear(-20.0), 2.0);
    const double lowFlat = settledRmsDbfs(runPreset(*preset, {0, 0, 0}, low));
    const double lowLifted = settledRmsDbfs(runPreset(*preset, {0, 100, 0}, low));
    CHECK_NEAR(lowLifted, lowFlat, 1.0);
}

AL_TEST(DspChain_leveling_slider_narrows_the_loudness_range) {
    const Preset* preset = findBuiltinPreset("movie");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const std::vector<float> input = makeDynamicMaterial();

    const std::vector<float> off = runPreset(*preset, {0, 0, 0}, input);
    const std::vector<float> on = runPreset(*preset, {0, 0, 100}, input);

    const double rangeOff = measureLoudness(off, kChannels, kRate).loudnessRangeLu;
    const double rangeOn = measureLoudness(on, kChannels, kRate).loudnessRangeLu;

    // This is requirement F-04 and the「映画」success criterion, stated as a
    // number: the gap between the quiet and loud passages has to shrink.
    CHECK(rangeOff > 10.0);
    CHECK(rangeOn < rangeOff - 5.0);
}

AL_TEST(DspChain_every_preset_does_what_its_category_promises) {
    const std::vector<float> input = makeDynamicMaterial();
    const double rangeBefore = measureLoudness(input, kChannels, kRate).loudnessRangeLu;

    // The two categories are held to opposite standards, because they are
    // trying to do opposite things. A speech preset that leaves the gap between
    // quiet and loud where it found it has not done its job; a music preset
    // that closes it has damaged the arrangement.
    for (const Preset& preset : builtinPresets()) {
        const std::vector<float> output = runPreset(preset, preset.sliders, input);
        const double rangeAfter = measureLoudness(output, kChannels, kRate).loudnessRangeLu;

        if (preset.category == audiolens::PresetCategory::Passthrough) {
            // The strictest of the three: not "barely changed" but unchanged.
            // A preset that claims to apply nothing has to survive the whole
            // chain — filters, dynamics, limiter — bit for bit, or the claim is
            // false and the user who selected it to get out of the way has not
            // got what they asked for.
            double worst = 0.0;
            for (std::size_t i = 0; i < input.size(); ++i) {
                worst = std::max(worst, static_cast<double>(std::abs(output[i] - input[i])));
            }
            if (worst != 0.0) {
                ::altest::reportFailure(__FILE__, __LINE__,
                                        "passthrough preset '" + preset.id +
                                            "' altered the signal by " + std::to_string(worst));
            }
        } else if (preset.category == audiolens::PresetCategory::Speech) {
            if (rangeAfter >= rangeBefore) {
                ::altest::reportFailure(__FILE__, __LINE__,
                                        "speech preset '" + preset.id +
                                            "' did not narrow the range: " +
                                            std::to_string(rangeBefore) + " -> " +
                                            std::to_string(rangeAfter));
            }
        } else {
            // Music presets are allowed to leave the range alone. What they are
            // not allowed to do is widen it: whatever else they are for, none
            // of them should make a record harder to listen to than it was.
            if (rangeAfter > rangeBefore + 0.5) {
                ::altest::reportFailure(__FILE__, __LINE__,
                                        "music preset '" + preset.id + "' widened the range: " +
                                            std::to_string(rangeBefore) + " -> " +
                                            std::to_string(rangeAfter));
            }
        }
    }
}

AL_TEST(DspChain_never_exceeds_the_limiter_ceiling) {
    // Every preset, driven with material hot enough to force the limiter to
    // work. N-05 says nothing may get past it, whatever the presets ask for.
    const std::vector<float> input = makeNoise(dbToLinear(-3.0), 4.0);

    for (const Preset& preset : builtinPresets()) {
        DspChain chain;
        chain.setParameters(resolveParameters(preset, {100, 100, 100}));
        chain.prepare(kRate, kChannels, 512);

        std::vector<float> audio = input;
        chain.process(audio.data(), audio.size() / kChannels, kChannels);

        double peak = 0.0;
        for (const float sample : audio) {
            peak = std::max(peak, static_cast<double>(std::fabs(sample)));
        }

        const double ceiling = dbToLinear(preset.mapping.limiterCeilingDb);
        if (peak > ceiling + 1e-6) {
            ::altest::reportFailure(__FILE__, __LINE__,
                                    "preset '" + preset.id + "' exceeded the ceiling: peak " +
                                        std::to_string(20.0 * std::log10(peak)) + " dBFS");
        }
        if (chain.limiterClippedSamples() != 0) {
            ::altest::reportFailure(__FILE__, __LINE__,
                                    "preset '" + preset.id + "' relied on the safety clip");
        }
    }
}

AL_TEST(DspChain_bypass_keeps_the_signal_intact) {
    const Preset* preset = findBuiltinPreset("night");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const std::vector<float> input = makeSine(80.0, dbToLinear(-20.0), 1.0);

    DspChain chain;
    chain.setParameters(resolveParameters(*preset));
    chain.prepare(kRate, kChannels, 512);
    chain.setBypass(true);

    std::vector<float> audio = input;
    chain.process(audio.data(), audio.size() / kChannels, kChannels);

    // The「深夜」preset cuts 80 Hz hard, so if bypass leaked the level would
    // collapse. Compare after the limiter's look-ahead delay.
    const auto latency = static_cast<std::size_t>(chain.latencyMs() * 0.001 * kRate);
    double maxDifference = 0.0;
    for (std::size_t f = latency; f < audio.size() / kChannels; ++f) {
        maxDifference = std::max(
            maxDifference,
            std::fabs(static_cast<double>(audio[f * kChannels]) - input[(f - latency) * kChannels]));
    }
    CHECK(maxDifference < 1e-5);
}

AL_TEST(DspChain_bypass_preserves_latency) {
    // A/B comparison is only fair if switching does not shift the timing.
    const Preset* preset = findBuiltinPreset("movie");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    DspChain processing;
    processing.setParameters(resolveParameters(*preset));
    processing.prepare(kRate, kChannels, 512);

    DspChain bypassed;
    bypassed.setParameters(resolveParameters(*preset));
    bypassed.prepare(kRate, kChannels, 512);
    bypassed.setBypass(true);

    CHECK_NEAR(bypassed.latencyMs(), processing.latencyMs(), 1e-9);
}

AL_TEST(DspChain_block_size_does_not_change_the_result) {
    // The chain re-evaluates parameters on sub-block boundaries, so the caller's
    // block size must not leak into the output.
    const Preset* preset = findBuiltinPreset("conversation");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const std::vector<float> input = makeNoise(dbToLinear(-18.0), 2.0);
    const DspParameters params = resolveParameters(*preset);

    const std::vector<float> wholeBuffer = runChain(params, input);

    DspChain chain;
    chain.setParameters(params);
    chain.prepare(kRate, kChannels, 512);
    std::vector<float> chunked = input;
    const std::size_t frames = chunked.size() / kChannels;
    for (std::size_t offset = 0; offset < frames;) {
        const std::size_t count = std::min<std::size_t>(37, frames - offset);  // deliberately odd
        chain.process(&chunked[offset * kChannels], count, kChannels);
        offset += count;
    }

    double maxDifference = 0.0;
    for (std::size_t i = 0; i < wholeBuffer.size(); ++i) {
        maxDifference =
            std::max(maxDifference, std::fabs(static_cast<double>(wholeBuffer[i] - chunked[i])));
    }
    CHECK(maxDifference < 1e-6);
}

AL_TEST(DspChain_produces_finite_output_for_extreme_settings) {
    const std::vector<float> input = makeNoise(1.0, 2.0);

    for (const Preset& preset : builtinPresets()) {
        for (const SliderValues sliders : {SliderValues{0, 0, 0}, SliderValues{100, 100, 100}}) {
            const std::vector<float> output = runPreset(preset, sliders, input);
            for (const float sample : output) {
                if (!std::isfinite(sample)) {
                    ::altest::reportFailure(__FILE__, __LINE__,
                                            "preset '" + preset.id + "' produced a non-finite sample");
                    break;
                }
            }
        }
    }
}

AL_TEST(Presets_expose_sensible_metadata) {
    CHECK(builtinPresets().size() >= 6);
    for (const Preset& preset : builtinPresets()) {
        CHECK(!preset.id.empty());
        CHECK(!preset.name.empty());
        CHECK(!preset.description.empty());
        CHECK(preset.sliders.bass >= 0 && preset.sliders.bass <= 100);
        CHECK(preset.sliders.clarity >= 0 && preset.sliders.clarity <= 100);
        CHECK(preset.sliders.leveling >= 0 && preset.sliders.leveling <= 100);
        CHECK(!preset.mapping.speechBandsAt100.empty());
    }

    for (const char* id : {"standard", "conversation", "lecture", "movie", "night", "game"}) {
        CHECK(findBuiltinPreset(id) != nullptr);
    }
    CHECK(findBuiltinPreset("does_not_exist") == nullptr);
}

AL_TEST(Presets_clamp_out_of_range_sliders) {
    const Preset* preset = findBuiltinPreset("standard");
    CHECK(preset != nullptr);
    if (preset == nullptr) return;

    const DspParameters low = resolveParameters(*preset, {-50, -50, -50});
    const DspParameters high = resolveParameters(*preset, {500, 500, 500});

    CHECK(!low.highpassEnabled);
    CHECK(!low.compressorEnabled);
    CHECK(high.highpassEnabled);
    CHECK(high.compressorEnabled);
    CHECK_NEAR(high.highpassFreqHz, preset->mapping.highpassFreqAt100Hz, 1e-9);
}

AL_TEST(Presets_at_zero_leveling_disable_the_compressor) {
    for (const Preset& preset : builtinPresets()) {
        const DspParameters params = resolveParameters(preset, {50, 50, 0});
        if (params.compressorEnabled) {
            ::altest::reportFailure(__FILE__, __LINE__,
                                    "preset '" + preset.id + "' left the compressor on at 0");
        }
    }
}

AL_TEST(DspChain_adds_no_latency_when_nothing_is_applied) {
    // The look-ahead limiter is the only thing in the chain that delays
    // anything, and it is skipped when there is no gain of ours for it to
    // catch. "No processing" therefore has to mean no delay either; a preset
    // that applies nothing but still costs two milliseconds would not be the
    // passthrough it claims to be.
    const Preset* standard = findBuiltinPreset("standard");
    CHECK(standard != nullptr);
    if (standard == nullptr) return;

    DspChain chain;
    chain.setParameters(resolveParameters(*standard, standard->sliders));
    chain.prepare(kRate, kChannels, 512);
    CHECK_NEAR(chain.latencyMs(), 0.0, 1e-9);

    // Moving one amount off zero brings the processing, and its delay, back.
    chain.setParameters(resolveParameters(*standard, SliderValues{0, 50, 0}));
    std::vector<float> audio = makeNoise(dbToLinear(-20.0), 0.2);
    chain.process(audio.data(), audio.size() / kChannels, kChannels);
    CHECK(chain.latencyMs() > 0.5);
}

AL_TEST(OutputVolume_maps_to_a_perceptual_taper) {
    using audiolens::outputVolumeToDb;

    CHECK_NEAR(outputVolumeToDb(100), 0.0, 1e-9);   // unity, and never above it
    CHECK_NEAR(outputVolumeToDb(50), -12.0, 0.1);   // halfway reads as "half as loud"
    CHECK_NEAR(outputVolumeToDb(25), -24.1, 0.2);
    CHECK(outputVolumeToDb(0) < -100.0);            // effectively silent

    // Monotonic, and out-of-range values are clamped rather than extrapolated.
    CHECK(outputVolumeToDb(80) < outputVolumeToDb(90));
    CHECK_NEAR(outputVolumeToDb(150), 0.0, 1e-9);
}

AL_TEST(OutputVolume_attenuates_without_costing_the_passthrough_its_latency) {
    // The master has to work on the preset that applies nothing, and it must not
    // drag the limiter back in to do it: an attenuation cannot clip, so there is
    // nothing for a limiter to catch. Losing the zero latency here would mean
    // "no processing" quietly stopped being true the moment somebody turned the
    // volume down.
    const Preset* standard = findBuiltinPreset("standard");
    CHECK(standard != nullptr);
    if (standard == nullptr) return;

    DspParameters parameters = resolveParameters(*standard, standard->sliders);
    CHECK(parameters.passthrough);
    parameters.outputGainDb += audiolens::outputVolumeToDb(50);

    DspChain chain;
    chain.setParameters(parameters);
    chain.prepare(kRate, kChannels, 512);
    CHECK_NEAR(chain.latencyMs(), 0.0, 1e-9);

    std::vector<float> audio = makeSine(1000.0, 0.2, 0.5);
    const double before = bandLevelDbfs(audio, kChannels, kRate, 1000.0, 2.0);
    chain.process(audio.data(), audio.size() / kChannels, kChannels);
    const double after = bandLevelDbfs(audio, kChannels, kRate, 1000.0, 2.0);

    CHECK_NEAR(after - before, -12.0, 0.5);
}

AL_TEST(GamePreset_tilts_the_spectrum_up_and_shortens_the_limiter) {
    const Preset* game = findBuiltinPreset("game");
    CHECK(game != nullptr);
    if (game == nullptr) return;

    const DspParameters params = resolveParameters(*game, game->sliders);

    // 低域を削り、中高域を際立たせる. Both halves are checked, because a low cut
    // on its own would only make everything quieter.
    CHECK(params.highpassEnabled);
    CHECK(params.highpassFreqHz > 150.0);
    CHECK(params.lowShelfGainDb < -4.0);
    CHECK(params.speechBandCount == 3);
    for (int i = 0; i < params.speechBandCount; ++i) {
        CHECK(params.speechBands[i].freqHz >= 2000.0);
        CHECK(params.speechBands[i].gainDb > 2.0);
    }

    // The only preset that widens instead of narrowing: the left/right
    // difference is what a player localises with.
    CHECK(params.midSideEnabled);
    CHECK(params.sideGainDb > 0.0);
    for (const Preset& other : builtinPresets()) {
        if (other.id == "game") continue;
        CHECK(resolveParameters(other, other.sliders).sideGainDb <= 0.0);
    }

    // 遅延を極力なくす. The look-ahead is the only latency a preset can affect,
    // so this is the whole of it: a quarter of what every other preset costs.
    DspChain chain;
    chain.setParameters(params);
    chain.prepare(kRate, kChannels, 512);
    CHECK(chain.latencyMs() < 0.75);

    const Preset* movie = findBuiltinPreset("movie");
    CHECK(movie != nullptr);
    if (movie == nullptr) return;
    DspChain reference;
    reference.setParameters(resolveParameters(*movie, movie->sliders));
    reference.prepare(kRate, kChannels, 512);
    CHECK(chain.latencyMs() < reference.latencyMs());
}

AL_TEST(Balance_maps_the_slider_to_a_signed_offset) {
    using audiolens::balanceToOffset;

    CHECK_NEAR(balanceToOffset(0), 0.0, 1e-12);
    CHECK_NEAR(balanceToOffset(-50), -1.0, 1e-12);
    CHECK_NEAR(balanceToOffset(50), 1.0, 1e-12);
    CHECK_NEAR(balanceToOffset(25), 0.5, 1e-12);

    // Out of range is clamped, not extrapolated: a balance past ±1 would invert
    // the far channel rather than silence it.
    CHECK_NEAR(balanceToOffset(-500), -1.0, 1e-12);
    CHECK_NEAR(balanceToOffset(500), 1.0, 1e-12);
}

AL_TEST(Balance_turns_the_far_channel_down_and_leaves_the_near_one_alone) {
    // The whole point of doing it by attenuation: whichever way the control is
    // moved, one channel is untouched and the other only ever gets quieter.
    // Nothing can end up louder than it arrived, so nothing needs a limiter.
    DspParameters params;
    params.limiter.ceilingDb = -1.0;
    const std::vector<float> input = makeSine(1000.0, dbToLinear(-20.0), 1.0);

    const double reference = settledChannelDbfs(input, 0);

    // Half way left: the right channel drops 6 dB, the left does not move.
    params.balance = audiolens::balanceToOffset(-25);
    const std::vector<float> left = runChain(params, input);
    CHECK_NEAR(settledChannelDbfs(left, 0), reference, 0.05);
    CHECK_NEAR(settledChannelDbfs(left, 1) - reference, -6.02, 0.2);

    // And symmetrically the other way.
    params.balance = audiolens::balanceToOffset(25);
    const std::vector<float> right = runChain(params, input);
    CHECK_NEAR(settledChannelDbfs(right, 1), reference, 0.05);
    CHECK_NEAR(settledChannelDbfs(right, 0) - reference, -6.02, 0.2);
}

AL_TEST(Balance_at_the_extreme_silences_one_channel) {
    DspParameters params;
    params.limiter.ceilingDb = -1.0;
    params.balance = audiolens::balanceToOffset(-50);

    const std::vector<float> output = runChain(params, makeSine(1000.0, dbToLinear(-20.0), 1.0));
    CHECK(settledChannelDbfs(output, 1) < -100.0);
    CHECK(settledChannelDbfs(output, 0) > -25.0);
}

AL_TEST(Balance_works_on_the_passthrough_without_costing_it_its_latency) {
    // Same bargain as the master volume, and the same reason: the passthrough
    // preset would stop being one if reaching for the balance quietly switched
    // the look-ahead limiter back in.
    const Preset* standard = findBuiltinPreset("standard");
    CHECK(standard != nullptr);
    if (standard == nullptr) return;

    DspParameters params = resolveParameters(*standard, standard->sliders);
    CHECK(params.passthrough);
    params.balance = audiolens::balanceToOffset(-25);

    DspChain chain;
    chain.setParameters(params);
    chain.prepare(kRate, kChannels, 512);
    CHECK_NEAR(chain.latencyMs(), 0.0, 1e-9);

    const std::vector<float> input = makeSine(1000.0, dbToLinear(-20.0), 1.0);
    std::vector<float> audio = input;
    chain.process(audio.data(), audio.size() / kChannels, kChannels);

    // The near channel is untouched sample for sample, not merely close: on the
    // passthrough path there is no filter state to settle.
    double worst = 0.0;
    for (std::size_t f = 0; f < audio.size() / kChannels; ++f) {
        worst = std::max(worst, std::fabs(static_cast<double>(audio[f * kChannels]) -
                                          input[f * kChannels]));
    }
    CHECK(worst == 0.0);
    CHECK_NEAR(settledChannelDbfs(audio, 1) - settledChannelDbfs(input, 1), -6.02, 0.2);
}

AL_TEST(Balance_centred_changes_nothing_at_all) {
    const Preset* standard = findBuiltinPreset("standard");
    CHECK(standard != nullptr);
    if (standard == nullptr) return;

    DspParameters params = resolveParameters(*standard, standard->sliders);
    params.balance = audiolens::balanceToOffset(0);

    const std::vector<float> input = makeDynamicMaterial();
    const std::vector<float> output = runChain(params, input);
    CHECK(output == input);
}

AL_TEST(Balance_is_ignored_on_mono_material) {
    // A mono stream has no far channel to turn down, and applying the trim to
    // its only channel would read as the volume dropping on its own. Both paths
    // through the chain have to make the same decision, so both are checked.
    const auto frames = static_cast<std::size_t>(0.5 * kRate);
    std::vector<float> input(frames, 0.0f);
    const double step = 2.0 * std::numbers::pi * 1000.0 / kRate;
    for (std::size_t f = 0; f < frames; ++f) {
        input[f] = static_cast<float>(std::sin(step * static_cast<double>(f)) * dbToLinear(-20.0));
    }

    auto rms = [](const std::vector<float>& mono) {
        double sumOfSquares = 0.0;
        for (std::size_t i = mono.size() / 2; i < mono.size(); ++i) {
            sumOfSquares += static_cast<double>(mono[i]) * mono[i];
        }
        return 10.0 * std::log10(sumOfSquares / static_cast<double>(mono.size() - mono.size() / 2));
    };

    DspParameters params;
    params.limiter.ceilingDb = -1.0;
    params.balance = audiolens::balanceToOffset(-50);

    // Passthrough path: untouched sample for sample.
    {
        DspParameters passthroughParams = params;
        passthroughParams.passthrough = true;
        DspChain chain;
        chain.setParameters(passthroughParams);
        chain.prepare(kRate, 1, 512);

        std::vector<float> audio = input;
        chain.process(audio.data(), frames, 1);
        CHECK(audio == input);
    }

    // Full path: the limiter delays it, so the level is what can be compared.
    {
        DspChain chain;
        chain.setParameters(params);
        chain.prepare(kRate, 1, 512);

        std::vector<float> audio = input;
        chain.process(audio.data(), frames, 1);
        CHECK_NEAR(rms(audio), rms(input), 0.1);
    }
}
