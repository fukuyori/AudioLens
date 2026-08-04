#include "test_support.h"

#include "dsp/resampler.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::dsp::Resampler;

namespace {

constexpr std::uint32_t kChannels = 2;

std::vector<float> makeSine(double freqHz, double amplitude, double sampleRate,
                            std::size_t frames) {
    std::vector<float> data(frames * kChannels);
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            data[f * kChannels + c] = v;
        }
    }
    return data;
}

/// Amplitude of the component at `freqHz`, via a single-bin DFT. Measuring one
/// bin rather than the whole spectrum is enough here and needs no FFT.
double amplitudeAt(const std::vector<float>& interleaved, double freqHz, double sampleRate,
                   std::size_t skipFrames) {
    const std::size_t frames = interleaved.size() / kChannels;
    if (frames <= skipFrames) {
        return 0.0;
    }
    const std::size_t count = frames - skipFrames;
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;

    double re = 0.0;
    double im = 0.0;
    for (std::size_t f = 0; f < count; ++f) {
        const double v = interleaved[(f + skipFrames) * kChannels];
        re += v * std::cos(step * static_cast<double>(f));
        im += v * std::sin(step * static_cast<double>(f));
    }
    return 2.0 * std::sqrt(re * re + im * im) / static_cast<double>(count);
}

double rms(const std::vector<float>& interleaved, std::size_t skipFrames) {
    const std::size_t frames = interleaved.size() / kChannels;
    if (frames <= skipFrames) {
        return 0.0;
    }
    double sum = 0.0;
    for (std::size_t f = skipFrames; f < frames; ++f) {
        const double v = interleaved[f * kChannels];
        sum += v * v;
    }
    return std::sqrt(sum / static_cast<double>(frames - skipFrames));
}

/// Runs `input` through the resampler in small blocks, the way the render
/// thread does, and returns everything it produced.
std::vector<float> resampleAll(Resampler& resampler, const std::vector<float>& input,
                               std::size_t pullBlock = 480) {
    std::vector<float> output;
    std::vector<float> scratch(pullBlock * kChannels);

    const std::size_t inputFrames = input.size() / kChannels;
    std::size_t pushed = 0;

    for (;;) {
        const std::size_t wanted = resampler.inputFramesWanted(pullBlock);
        if (wanted > 0 && pushed < inputFrames) {
            const std::size_t chunk = std::min(wanted, inputFrames - pushed);
            resampler.pushInput(&input[pushed * kChannels], chunk);
            pushed += chunk;
        }

        // Always pull, even once the input is exhausted, so the tail is drained
        // rather than left inside. Stopping early would quantise the output
        // length to whole blocks and hide differences the tests are looking for.
        const std::size_t produced = resampler.pullOutput(scratch.data(), pullBlock);
        if (produced == 0) {
            break;
        }
        output.insert(output.end(), scratch.begin(), scratch.begin() + produced * kChannels);
    }
    return output;
}

}  // namespace

AL_TEST(Resampler_at_unity_ratio_preserves_the_signal) {
    Resampler resampler;
    resampler.prepare(kChannels, 1.0, 480);

    const std::vector<float> input = makeSine(1000.0, 0.5, 48000.0, 48000);
    const std::vector<float> output = resampleAll(resampler, input);

    CHECK(output.size() / kChannels > 47000);
    // Skip the group delay before measuring.
    CHECK_NEAR(amplitudeAt(output, 1000.0, 48000.0, 64), 0.5, 0.01);
}

AL_TEST(Resampler_has_unity_gain_at_dc) {
    // A constant in must give the same constant out: the kernel rows are
    // normalized precisely so the level does not wobble with the phase.
    Resampler resampler;
    resampler.prepare(kChannels, 44100.0 / 48000.0, 480);

    std::vector<float> input(48000 * kChannels, 0.25f);
    const std::vector<float> output = resampleAll(resampler, input);

    const std::size_t frames = output.size() / kChannels;
    CHECK(frames > 40000);
    for (std::size_t f = 200; f < frames - 200; ++f) {
        if (std::fabs(output[f * kChannels] - 0.25f) > 1e-3f) {
            ::altest::reportFailure(__FILE__, __LINE__,
                                    "DC gain deviated at frame " + std::to_string(f) + ": " +
                                        std::to_string(output[f * kChannels]));
            break;
        }
    }
}

AL_TEST(Resampler_converts_48k_to_44k1_preserving_tone) {
    Resampler resampler;
    resampler.prepare(kChannels, 44100.0 / 48000.0, 480);

    const std::vector<float> input = makeSine(1000.0, 0.5, 48000.0, 48000);
    const std::vector<float> output = resampleAll(resampler, input);

    const std::size_t frames = output.size() / kChannels;
    // One second in at 48 kHz should be one second out at 44.1 kHz.
    CHECK(frames > 43500 && frames < 44500);

    // The tone stays at 1 kHz in absolute terms, which at the new rate means
    // the same frequency argument against the new sample rate.
    CHECK_NEAR(amplitudeAt(output, 1000.0, 44100.0, 64), 0.5, 0.01);
}

AL_TEST(Resampler_converts_44k1_to_48k_preserving_tone) {
    Resampler resampler;
    resampler.prepare(kChannels, 48000.0 / 44100.0, 480);

    const std::vector<float> input = makeSine(1000.0, 0.5, 44100.0, 44100);
    const std::vector<float> output = resampleAll(resampler, input);

    const std::size_t frames = output.size() / kChannels;
    CHECK(frames > 47500 && frames < 48500);
    CHECK_NEAR(amplitudeAt(output, 1000.0, 48000.0, 64), 0.5, 0.01);
}

AL_TEST(Resampler_attenuates_content_above_the_output_nyquist) {
    // Halving the rate is where aliasing would actually be audible: without
    // band-limiting, 16 kHz would fold to 24000 - 16000 = 8 kHz, right in the
    // middle of the range AudioLens exists to clean up.
    //
    // 48 kHz -> 44.1 kHz is a poor test of this. Its Nyquist only drops to
    // 22.05 kHz, so the only content that can fold is 22.05-24 kHz, which lands
    // back at 20-22 kHz and is inaudible either way; and 23 kHz sits in the
    // kernel's transition band rather than its stopband.
    Resampler passing;
    passing.prepare(kChannels, 0.5, 480);
    const std::vector<float> below = makeSine(4000.0, 0.5, 48000.0, 48000);
    const std::vector<float> belowOut = resampleAll(passing, below);

    Resampler blocking;
    blocking.prepare(kChannels, 0.5, 480);
    const std::vector<float> above = makeSine(16000.0, 0.5, 48000.0, 48000);
    const std::vector<float> aboveOut = resampleAll(blocking, above);

    const double keptRms = rms(belowOut, 500);
    const double rejectedRms = rms(aboveOut, 500);

    CHECK(keptRms > 0.3);
    // Better than -40 dB: whatever leaks through is far below anything a
    // listener could pick out of the programme material.
    CHECK(rejectedRms < keptRms * 0.01);
}

AL_TEST(Resampler_ratio_trim_changes_the_output_length) {
    const std::size_t inputFrames = 48000;

    const auto producedWithTrim = [&](double trim) {
        Resampler resampler;
        resampler.prepare(kChannels, 1.0, 480);
        resampler.setRatioTrim(trim);
        const std::vector<float> input = makeSine(1000.0, 0.5, 48000.0, inputFrames);
        return resampleAll(resampler, input).size() / kChannels;
    };

    const std::size_t neutral = producedWithTrim(1.0);
    const std::size_t faster = producedWithTrim(1.01);
    const std::size_t slower = producedWithTrim(0.99);

    // Trimming up asks for more output frames from the same input, and vice
    // versa. This is the mechanism the engine uses to absorb clock drift.
    CHECK(faster > neutral);
    CHECK(slower < neutral);
    CHECK_NEAR(static_cast<double>(faster) / static_cast<double>(neutral), 1.01, 0.005);
    CHECK_NEAR(static_cast<double>(slower) / static_cast<double>(neutral), 0.99, 0.005);
}

AL_TEST(Resampler_trim_is_clamped_to_a_safe_range) {
    Resampler resampler;
    resampler.prepare(kChannels, 1.0, 480);

    resampler.setRatioTrim(10.0);
    CHECK(resampler.ratioTrim() <= 1.021);
    resampler.setRatioTrim(0.0);
    CHECK(resampler.ratioTrim() >= 0.979);
}

AL_TEST(Resampler_reports_short_when_starved) {
    Resampler resampler;
    resampler.prepare(kChannels, 1.0, 480);

    std::vector<float> scratch(480 * kChannels, 0.0f);
    // Nothing pushed yet: it must say so rather than inventing samples.
    CHECK_EQ(resampler.pullOutput(scratch.data(), 480), std::size_t{0});

    const std::vector<float> input = makeSine(1000.0, 0.5, 48000.0, 100);
    resampler.pushInput(input.data(), 100);
    const std::size_t produced = resampler.pullOutput(scratch.data(), 480);
    CHECK(produced > 0);
    CHECK(produced < 480);
}

AL_TEST(Resampler_inputFramesWanted_is_sufficient) {
    Resampler resampler;
    resampler.prepare(kChannels, 44100.0 / 48000.0, 480);

    const std::vector<float> input = makeSine(1000.0, 0.5, 48000.0, 48000);
    std::vector<float> scratch(480 * kChannels);
    std::size_t pushed = 0;

    // Pushing exactly what it asks for must always be enough to pull a full
    // block; if it were not, the render thread would underrun every callback.
    for (int round = 0; round < 50; ++round) {
        const std::size_t wanted = resampler.inputFramesWanted(480);
        if (wanted > 0) {
            CHECK(pushed + wanted <= 48000);
            resampler.pushInput(&input[pushed * kChannels], wanted);
            pushed += wanted;
        }
        CHECK_EQ(resampler.pullOutput(scratch.data(), 480), std::size_t{480});
    }
}

AL_TEST(Resampler_survives_buffer_compaction) {
    // Long enough to force the internal FIFO to be compacted many times. A
    // compaction that failed to move the read position with the data would
    // show up here as a broken tone, not as a crash.
    Resampler resampler;
    resampler.prepare(kChannels, 44100.0 / 48000.0, 256);

    const std::vector<float> input = makeSine(997.0, 0.5, 48000.0, 48000 * 5);
    const std::vector<float> output = resampleAll(resampler, input, 256);

    const std::size_t frames = output.size() / kChannels;
    CHECK(frames > 44100 * 4);

    // Measure over the last second only: any positional slip accumulated over
    // the preceding four would have destroyed the tone by then.
    const std::size_t lastSecond = frames > 44100 ? frames - 44100 : 0;
    CHECK_NEAR(amplitudeAt(output, 997.0, 44100.0, lastSecond), 0.5, 0.02);
}

AL_TEST(Resampler_keeps_channels_independent) {
    Resampler resampler;
    resampler.prepare(kChannels, 44100.0 / 48000.0, 480);

    // Left carries the tone, right is silent. Any cross-talk means the
    // interleaved indexing is wrong.
    std::vector<float> input(48000 * kChannels, 0.0f);
    const double step = 2.0 * std::numbers::pi * 1000.0 / 48000.0;
    for (std::size_t f = 0; f < 48000; ++f) {
        input[f * kChannels] = static_cast<float>(std::sin(step * static_cast<double>(f)) * 0.5);
    }

    const std::vector<float> output = resampleAll(resampler, input);
    const std::size_t frames = output.size() / kChannels;

    double rightPeak = 0.0;
    for (std::size_t f = 0; f < frames; ++f) {
        rightPeak = std::max(rightPeak, std::fabs(static_cast<double>(output[f * kChannels + 1])));
    }
    CHECK(rightPeak < 1e-6);
    CHECK_NEAR(amplitudeAt(output, 1000.0, 44100.0, 64), 0.5, 0.01);
}
