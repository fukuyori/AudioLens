#include "test_support.h"

#include "dsp/biquad.h"
#include "dsp/compressor.h"
#include "dsp/limiter.h"

#include <cmath>
#include <numbers>
#include <vector>

using namespace audiolens::dsp;

namespace {

constexpr double kRate = 48000.0;

double toDb(double linear) { return 20.0 * std::log10(linear); }

/// Runs a sine through `filter` and returns the output/input amplitude ratio in
/// dB, measured after the transient has passed. Proves the response the
/// coefficients describe is the response the running filter actually has.
double measuredGainDb(Biquad filter, double freqHz, double sampleRate) {
    const std::size_t settle = static_cast<std::size_t>(sampleRate * 0.5);
    const std::size_t measure = static_cast<std::size_t>(sampleRate * 0.5);
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;

    double phase = 0.0;
    for (std::size_t i = 0; i < settle; ++i) {
        filter.process(static_cast<float>(std::sin(phase)));
        phase += step;
    }

    double peak = 0.0;
    for (std::size_t i = 0; i < measure; ++i) {
        const double out = filter.process(static_cast<float>(std::sin(phase)));
        peak = std::max(peak, std::fabs(out));
        phase += step;
    }
    return toDb(peak);
}

std::vector<float> makeSine(double freqHz, double amplitude, double seconds, unsigned channels,
                            double sampleRate) {
    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    std::vector<float> data(frames * channels);
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        for (unsigned c = 0; c < channels; ++c) {
            data[f * channels + c] = v;
        }
    }
    return data;
}

}  // namespace

// ---------------------------------------------------------------- biquad ---

AL_TEST(Biquad_highpass_attenuates_below_and_passes_above) {
    const BiquadCoeffs c = designHighpass(200.0, 0.707, kRate);

    // A Butterworth highpass is 3 dB down at its corner.
    CHECK_NEAR(toDb(magnitudeAt(c, 200.0, kRate)), -3.0, 0.3);
    // 12 dB/octave: two octaves below the corner is roughly 24 dB down.
    CHECK_NEAR(toDb(magnitudeAt(c, 50.0, kRate)), -24.0, 1.5);
    CHECK_NEAR(toDb(magnitudeAt(c, 2000.0, kRate)), 0.0, 0.2);
}

AL_TEST(Biquad_peaking_hits_its_gain_at_the_centre) {
    for (const double gainDb : {-6.0, -3.0, 3.0, 6.0, 12.0}) {
        const BiquadCoeffs c = designPeaking(2500.0, gainDb, 1.0, kRate);
        CHECK_NEAR(toDb(magnitudeAt(c, 2500.0, kRate)), gainDb, 0.05);
        // Far from the centre a peaking filter must be transparent.
        CHECK_NEAR(toDb(magnitudeAt(c, 60.0, kRate)), 0.0, 0.3);
    }
}

AL_TEST(Biquad_shelves_reach_their_gain_in_the_shelf_band) {
    const BiquadCoeffs low = designLowShelf(200.0, -6.0, 0.707, kRate);
    CHECK_NEAR(toDb(magnitudeAt(low, 20.0, kRate)), -6.0, 0.5);
    CHECK_NEAR(toDb(magnitudeAt(low, 5000.0, kRate)), 0.0, 0.2);

    const BiquadCoeffs high = designHighShelf(6000.0, 4.0, 0.707, kRate);
    CHECK_NEAR(toDb(magnitudeAt(high, 20000.0, kRate)), 4.0, 0.5);
    CHECK_NEAR(toDb(magnitudeAt(high, 200.0, kRate)), 0.0, 0.2);
}

AL_TEST(Biquad_running_response_matches_the_computed_response) {
    // The transfer function is only useful if the implementation agrees with it.
    struct Case {
        BiquadCoeffs coeffs;
        double freqHz;
    };
    const Case cases[] = {
        {designPeaking(2500.0, 6.0, 1.0, kRate), 2500.0},
        {designPeaking(1000.0, -6.0, 1.0, kRate), 1000.0},
        {designHighpass(200.0, 0.707, kRate), 100.0},
        {designLowShelf(220.0, -8.0, 0.707, kRate), 40.0},
        {designHighShelf(6000.0, 4.0, 0.707, kRate), 15000.0},
    };

    for (const Case& c : cases) {
        Biquad filter;
        filter.setCoeffs(c.coeffs);
        CHECK_NEAR(measuredGainDb(filter, c.freqHz, kRate),
                   toDb(magnitudeAt(c.coeffs, c.freqHz, kRate)), 0.1);
    }
}

AL_TEST(Biquad_stays_stable_at_extreme_design_frequencies) {
    // A slider sweeping a cutoff must not be able to produce an unstable filter,
    // so designs are clamped below Nyquist and above DC.
    for (const double freq : {0.5, 1.0, 20.0, 23999.0, 24000.0, 96000.0}) {
        Biquad filter;
        filter.setCoeffs(designHighpass(freq, 0.707, kRate));

        double peak = 0.0;
        for (int i = 0; i < 20000; ++i) {
            const auto input = static_cast<float>((i % 2 == 0) ? 0.5 : -0.5);
            peak = std::max(peak, std::fabs(static_cast<double>(filter.process(input))));
        }
        CHECK(std::isfinite(peak));
        CHECK(peak < 100.0);
    }
}

// ------------------------------------------------------------ compressor ---

AL_TEST(Compressor_leaves_signals_below_the_knee_alone) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -20.0;
    s.ratio = 4.0;
    s.kneeDb = 6.0;
    comp.setSettings(s);

    // Below threshold minus half the knee the curve is exactly unity.
    CHECK_NEAR(comp.staticGainDb(-40.0), 0.0, 1e-9);
    CHECK_NEAR(comp.staticGainDb(-23.0), 0.0, 1e-9);
}

AL_TEST(Compressor_applies_the_ratio_above_the_knee) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -20.0;
    s.ratio = 4.0;
    s.kneeDb = 6.0;
    comp.setSettings(s);

    // 20 dB over threshold at 4:1 should come out 5 dB over, i.e. -15 dB of gain.
    CHECK_NEAR(comp.staticGainDb(0.0), -15.0, 1e-9);
    // 8 dB over -> 2 dB over -> -6 dB of gain.
    CHECK_NEAR(comp.staticGainDb(-12.0), -6.0, 1e-9);
}

AL_TEST(Compressor_knee_is_continuous_and_monotonic) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -20.0;
    s.ratio = 4.0;
    s.kneeDb = 10.0;
    comp.setSettings(s);

    // Walking across the knee, the output level must never step or go backwards:
    // a discontinuity here would be audible as a click on every crossing.
    double previousOutput = -1e9;
    for (double input = -40.0; input <= 0.0; input += 0.1) {
        const double output = input + comp.staticGainDb(input);
        CHECK(output >= previousOutput - 1e-9);
        if (output < previousOutput - 1e-9) {
            break;
        }
        previousOutput = output;
    }

    // The curve is tangent to the straight segments at both knee edges.
    CHECK_NEAR(comp.staticGainDb(-25.0), 0.0, 1e-6);
    CHECK_NEAR(comp.staticGainDb(-15.0), -3.75, 1e-6);
}

AL_TEST(Compressor_ratio_of_one_is_a_no_op) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -30.0;
    s.ratio = 1.0;
    s.kneeDb = 6.0;
    comp.setSettings(s);

    for (const double level : {-60.0, -30.0, -10.0, 0.0}) {
        CHECK_NEAR(comp.staticGainDb(level), 0.0, 1e-9);
    }
}

/// Runs a steady sine at the given RMS level through `comp` and returns the
/// settled output RMS in dBFS. The detector works in RMS, so the tests have to
/// speak the same units or the numbers will not line up.
double settledOutputRmsDb(Compressor& comp, double inputRmsDb, double freqHz = 1000.0) {
    // A sine's RMS is its amplitude over root two.
    const double amplitude = std::pow(10.0, inputRmsDb / 20.0) * std::numbers::sqrt2;
    std::vector<float> audio = makeSine(freqHz, amplitude, 2.0, 2, kRate);
    const std::size_t frames = audio.size() / 2;
    for (std::size_t f = 0; f < frames; ++f) {
        comp.processFrame(&audio[f * 2], 2);
    }

    double sumOfSquares = 0.0;
    const std::size_t start = frames / 2;
    for (std::size_t f = start; f < frames; ++f) {
        sumOfSquares += static_cast<double>(audio[f * 2]) * audio[f * 2];
    }
    return 10.0 * std::log10(sumOfSquares / static_cast<double>(frames - start));
}

AL_TEST(Compressor_makeup_restores_level_at_the_reference) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -30.0;
    s.ratio = 4.0;
    s.kneeDb = 4.0;
    s.attackMs = 1.0;
    s.releaseMs = 20.0;
    s.makeupReferenceDb = -18.0;
    comp.setSettings(s);

    // Material sitting at exactly the reference level must come back out at the
    // level it went in at: that is what auto makeup is calibrated to do.
    CHECK_NEAR(settledOutputRmsDb(comp, -18.0), -18.0, 0.2);
}

AL_TEST(Compressor_follows_its_static_curve_on_steady_material) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -30.0;
    s.ratio = 4.0;
    s.kneeDb = 4.0;
    s.attackMs = 1.0;
    s.releaseMs = 20.0;
    s.makeupReferenceDb = -18.0;
    comp.setSettings(s);

    // The makeup at the reference level is what the curve removes there, so the
    // settled output is the curve plus that constant.
    const double makeup = -comp.staticGainDb(-18.0);
    for (const double inputDb : {-40.0, -24.0, -18.0, -12.0, -6.0}) {
        comp.reset();
        const double expected = inputDb + comp.staticGainDb(inputDb) + makeup;
        CHECK_NEAR(settledOutputRmsDb(comp, inputDb), expected, 0.3);
    }
}

AL_TEST(Compressor_reduces_the_gap_between_loud_and_quiet) {
    Compressor comp;
    comp.prepare(kRate);
    CompressorSettings s;
    s.thresholdDb = -34.0;
    s.ratio = 7.0;
    s.kneeDb = 8.0;
    s.attackMs = 5.0;
    s.releaseMs = 100.0;
    comp.setSettings(s);

    comp.reset();
    const double quiet = settledOutputRmsDb(comp, -40.0);
    comp.reset();
    const double loud = settledOutputRmsDb(comp, -6.0);

    // 34 dB apart going in; the ratio should pull them to roughly a third of that.
    const double gap = loud - quiet;
    CHECK(gap < 20.0);
    CHECK(gap > 0.0);
}

// --------------------------------------------------------------- limiter ---

AL_TEST(Limiter_never_exceeds_its_ceiling) {
    Limiter limiter;
    LimiterSettings s;
    s.ceilingDb = -1.0;
    s.lookaheadMs = 2.0;
    s.releaseMs = 50.0;
    limiter.setSettings(s);
    limiter.prepare(kRate, 2);
    limiter.setSettings(s);

    const double ceiling = std::pow(10.0, -1.0 / 20.0);

    // Alternating quiet passages and sudden bursts well over full scale: the
    // hardest case for a limiter, because the gain has to be down before the
    // burst arrives and back up quickly afterwards.
    std::vector<float> audio = makeSine(300.0, 0.2, 3.0, 2, kRate);
    const std::size_t frames = audio.size() / 2;
    for (std::size_t f = 0; f < frames; ++f) {
        if ((f / 4800) % 3 == 1) {
            audio[f * 2] *= 20.0f;
            audio[f * 2 + 1] *= 20.0f;
        }
    }

    double peak = 0.0;
    for (std::size_t f = 0; f < frames; ++f) {
        limiter.processFrame(&audio[f * 2], 2);
        peak = std::max(peak, static_cast<double>(std::fabs(audio[f * 2])));
        peak = std::max(peak, static_cast<double>(std::fabs(audio[f * 2 + 1])));
    }

    CHECK(peak <= ceiling + 1e-6);
    // The look-ahead gain should have done the work; the hard clip is only a
    // backstop and reaching it would mean the gain computation is wrong.
    CHECK_EQ(limiter.clippedSamples(), std::uint64_t{0});
}

AL_TEST(Limiter_passes_quiet_material_through_unchanged) {
    Limiter limiter;
    LimiterSettings s;
    s.ceilingDb = -1.0;
    s.lookaheadMs = 2.0;
    limiter.setSettings(s);
    limiter.prepare(kRate, 2);
    limiter.setSettings(s);

    std::vector<float> audio = makeSine(1000.0, 0.25, 0.5, 2, kRate);
    const std::vector<float> original = audio;
    const std::size_t frames = audio.size() / 2;
    const std::size_t latency = limiter.latencyFrames();

    for (std::size_t f = 0; f < frames; ++f) {
        limiter.processFrame(&audio[f * 2], 2);
    }

    // Output is the input delayed by the look-ahead, untouched otherwise.
    for (std::size_t f = latency; f < frames; ++f) {
        CHECK_NEAR(audio[f * 2], original[(f - latency) * 2], 1e-6);
    }
}

AL_TEST(Limiter_reports_its_latency) {
    Limiter limiter;
    LimiterSettings s;
    s.lookaheadMs = 2.0;
    limiter.setSettings(s);
    limiter.prepare(kRate, 2);

    CHECK_EQ(limiter.latencyFrames(), std::size_t{96});  // 2 ms at 48 kHz
}
