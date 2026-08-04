// Slow levelling (dsp::AutoGain).
//
// The stage claims to walk a broadband gain towards whatever puts short-term
// loudness on target, slowly enough not to be heard as movement, and to leave
// silence alone. Each of those is a separate test, because each fails in a way
// the others would not catch: aiming at the wrong level, lurching, or hauling
// up room tone between scenes.

#include "test_support.h"

#include "analysis/loudness.h"
#include "dsp/auto_gain.h"

#include <cmath>
#include <numbers>
#include <vector>

using audiolens::analysis::measureLoudness;
using audiolens::dsp::AutoGain;
using audiolens::dsp::AutoGainSettings;

namespace {

constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kChannels = 2;

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

AutoGainSettings settingsWithTarget(double targetLufs) {
    AutoGainSettings s;
    s.enabled = true;
    s.targetLufs = targetLufs;
    s.maxBoostDb = 20.0;
    s.maxCutDb = 20.0;
    // Faster than any preset uses, so a test does not have to run for a minute
    // of audio to watch the gain arrive.
    s.riseDbPerSecond = 20.0;
    s.fallDbPerSecond = 20.0;
    // Likewise the window: the stage does nothing at all until it has a full
    // one, so a test that used the 30 s default would need 30 s of audio before
    // anything happened.
    s.windowSeconds = 3.0;
    return s;
}

/// Runs the stage over `audio` in place and returns the gain it settled on.
double applyAndSettle(AutoGain& gain, std::vector<float>& audio) {
    for (std::size_t f = 0; f < audio.size() / kChannels; ++f) {
        float* frame = audio.data() + f * kChannels;
        const float g = gain.nextGain(frame, kChannels);
        for (std::uint32_t c = 0; c < kChannels; ++c) {
            frame[c] *= g;
        }
    }
    return gain.currentGainDb();
}

}  // namespace

AL_TEST(AutoGain_brings_a_quiet_source_up_to_the_target) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);
    gain.setSettings(settingsWithTarget(-18.0));

    // -30 dBFS sine measures near -30 LUFS, so about 12 dB of lift is wanted.
    auto audio = makeSine(1000.0, std::pow(10.0, -30.0 / 20.0), 12.0);
    const double settled = applyAndSettle(gain, audio);

    CHECK_NEAR(settled, 12.0, 1.5);

    // And the tail of the output should actually be on target, which is the
    // claim that matters rather than the internal gain figure.
    std::vector<float> tail(audio.end() - static_cast<std::ptrdiff_t>(4 * kRate * kChannels),
                            audio.end());
    const auto measured = measureLoudness(tail, kChannels, kRate);
    CHECK(measured.valid());
    CHECK_NEAR(measured.integratedLufs, -18.0, 1.5);
}

AL_TEST(AutoGain_pulls_a_loud_source_down_to_the_target) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);
    gain.setSettings(settingsWithTarget(-18.0));

    auto audio = makeSine(1000.0, std::pow(10.0, -6.0 / 20.0), 12.0);
    const double settled = applyAndSettle(gain, audio);

    CHECK(settled < -8.0);
    CHECK(settled > -16.0);
}

AL_TEST(AutoGain_does_not_lift_silence) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);
    gain.setSettings(settingsWithTarget(-18.0));

    // Digital black is far below the gate. Lifting it would be lifting the
    // hiss and room tone that sit underneath it in real material.
    std::vector<float> audio(static_cast<std::size_t>(10.0 * kRate) * kChannels, 0.0f);
    const double settled = applyAndSettle(gain, audio);

    CHECK_NEAR(settled, 0.0, 0.01);
}

AL_TEST(AutoGain_respects_its_rate_limit) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);

    AutoGainSettings s = settingsWithTarget(-18.0);
    s.riseDbPerSecond = 1.0;  // the rate the presets actually use
    gain.setSettings(s);

    // Two seconds of a source that wants 12 dB of lift. At 1 dB/s it must not
    // have got more than about 2 dB of the way there.
    auto audio = makeSine(1000.0, std::pow(10.0, -30.0 / 20.0), 5.0);
    const double settled = applyAndSettle(gain, audio);

    // The first 3 s are spent filling the measurement window, so only the
    // remaining 2 s can have moved the gain at all.
    CHECK(settled > 0.5);
    CHECK(settled < 2.5);
}

AL_TEST(AutoGain_honours_its_range_limits) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);

    AutoGainSettings s = settingsWithTarget(-18.0);
    s.maxBoostDb = 4.0;
    gain.setSettings(s);

    auto audio = makeSine(1000.0, std::pow(10.0, -40.0 / 20.0), 12.0);
    const double settled = applyAndSettle(gain, audio);

    // 22 dB of lift is wanted; 4 dB is all it is allowed to give. A levelling
    // stage without a ceiling turns a quiet passage into a noise floor.
    CHECK_NEAR(settled, 4.0, 0.2);
}

AL_TEST(AutoGain_is_inert_when_disabled) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);

    AutoGainSettings s = settingsWithTarget(-18.0);
    s.enabled = false;
    gain.setSettings(s);

    auto audio = makeSine(1000.0, std::pow(10.0, -30.0 / 20.0), 6.0);
    const auto before = audio;
    applyAndSettle(gain, audio);

    double worst = 0.0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(audio[i] - before[i])));
    }
    CHECK(worst < 1e-6);
}

AL_TEST(AutoGain_does_nothing_until_it_has_a_full_window) {
    AutoGain gain;
    gain.prepare(kRate, kChannels);

    AutoGainSettings s = settingsWithTarget(-18.0);
    s.windowSeconds = 30.0;
    gain.setSettings(s);

    // Ten seconds of a source wanting 12 dB of lift, against a 30 s window.
    // Moving on a third of an average is guessing, and guessing measured worse
    // than waiting: it commits to a level the rest of the window then has to
    // undo, and the correction is itself a spread of levels.
    auto audio = makeSine(1000.0, std::pow(10.0, -30.0 / 20.0), 10.0);
    const auto before = audio;
    const double settled = applyAndSettle(gain, audio);

    CHECK_NEAR(settled, 0.0, 0.01);

    double worst = 0.0;
    for (std::size_t i = 0; i < audio.size(); ++i) {
        worst = std::max(worst, static_cast<double>(std::abs(audio[i] - before[i])));
    }
    CHECK(worst < 1e-6);
}
