#pragma once

#include "dsp/parameters.h"

#include <string>
#include <vector>

namespace audiolens {

/// What the user sees: three plain-language amounts, each 0-100.
/// Nothing in this struct mentions a frequency or a decibel.
struct SliderValues {
    int bass = 50;      ///< 「低音」  0 = 低音そのまま、100 = 濁りを強く抑える
    int clarity = 50;   ///< 「声の明瞭さ」
    int leveling = 50;  ///< 「音量差」  0 = 整えない、100 = 強く揃える

    SliderValues clamped() const;
};

/// How a preset turns the three sliders into DSP settings.
///
/// Every field here is an endpoint of an interpolation: a slider at 0 gives the
/// `...At0` value, at 100 the `...At100` value, and in between a straight line.
/// Keeping the mapping as data rather than code is what lets a preset be
/// retuned, and eventually loaded from JSON, without touching the DSP.
struct PresetMapping {
    // --- 低音 ---
    /// Highpass corner at slider 100. The filter is off at slider 0.
    double highpassFreqAt100Hz = 120.0;
    double highpassQ = 0.707;
    double lowShelfFreqHz = 220.0;
    double lowShelfGainAt0Db = 0.0;
    double lowShelfGainAt100Db = -3.0;
    double lowShelfQ = 0.707;

    // --- 声の明瞭さ ---
    /// Peaking bands lifted in proportion to the clarity slider. `gainDb` in
    /// each band is the gain at slider 100.
    std::vector<dsp::SpeechBand> speechBandsAt100;
    double highShelfFreqHz = 7000.0;
    double highShelfGainAt0Db = 0.0;
    double highShelfGainAt100Db = 0.0;
    double highShelfQ = 0.707;

    /// Lift the speech range in the mid channel only, and hold the sides back.
    /// Worth having wherever the source is stereo with centred dialogue; not
    /// worth it for material that is mono to begin with, where it does nothing.
    bool midSideEnabled = false;
    /// Side level at a clarity slider of 100. Negative brings the centre
    /// forward relative to everything else.
    double sideGainAt100Db = 0.0;

    // --- 音量差 (遅い側) ---
    /// Scene-to-scene levelling, ahead of the compressor. Presets meant for
    /// long-form material want it; a preset for a short call does not gain much.
    bool autoGainEnabled = false;
    double autoGainTargetLufs = -18.0;
    /// How far the slow gain may travel at a leveling slider of 100, in either
    /// direction. At slider 0 the range is zero, which is the same as off.
    double autoGainRangeAt100Db = 9.0;
    double autoGainRiseDbPerSecond = 0.05;
    double autoGainFallDbPerSecond = 0.10;

    /// Sibilance control, driven by the same slider that creates the problem.
    /// At a clarity of 0 the speech bands are flat and there is nothing extra
    /// to tame, so the stage scales with the slider rather than sitting at a
    /// fixed strength.
    bool deEsserEnabled = false;
    double deEsserFreqHz = 6500.0;
    double deEsserQ = 1.0;
    double deEsserThresholdDb = -14.0;
    double deEsserAmountAt100 = 0.7;
    double deEsserMaxReductionAt100Db = 8.0;

    // --- 音量差 ---
    double compressorThresholdAt0Db = -12.0;
    double compressorThresholdAt100Db = -32.0;
    double compressorRatioAt0 = 1.0;
    double compressorRatioAt100 = 6.0;
    double compressorKneeDb = 8.0;
    double compressorAttackMs = 10.0;
    double compressorReleaseMs = 250.0;
    double compressorMakeupReferenceDb = -18.0;

    // --- 固定 ---
    double outputGainDb = 0.0;
    double limiterCeilingDb = -1.0;
    double limiterLookaheadMs = 2.0;
    double limiterReleaseMs = 60.0;
};

/// What a preset is trying to do, which decides what counts as it working.
///
/// A speech preset has failed if it leaves the gap between quiet and loud
/// passages where it found it. A music preset has failed if it closes that gap,
/// because on a record the gap is the arrangement. The two cannot be held to
/// the same standard, and pretending otherwise would mean compressing music to
/// satisfy a test written for dialogue.
enum class PresetCategory {
    Speech,
    Music,
    /// Applies nothing at all. Held to the strictest standard of the three:
    /// the output must be the input, sample for sample, with no added latency.
    Passthrough,
};

struct Preset {
    std::string id;
    std::string name;         ///< Display name, Japanese.
    std::string description;  ///< One line explaining when to reach for it.
    PresetCategory category = PresetCategory::Speech;
    SliderValues sliders;
    PresetMapping mapping;
};

/// Folds a preset and a set of slider positions into the values the DSP chain
/// runs on.
dsp::DspParameters resolveParameters(const Preset& preset, const SliderValues& sliders);

/// Master output level as a gain in dB (requirement F-22).
///
/// 100 is unity and 0 is silence, with a taper that puts the halfway point
/// around -12 dB — roughly where a listener would call it "half as loud", which
/// a linear scale badly fails to do.
double outputVolumeToDb(int volume);

/// Left/right balance as the DSP chain takes it (requirement F-24).
///
/// `balance` is the slider position: -50 fully left, 0 centred, +50 fully
/// right. Returns -1.0 .. +1.0.
///
/// Like the master volume this sits outside the preset. A preset says how the
/// sound should be shaped; balance corrects the listener's ears or a speaker
/// that sits closer than its pair, and neither of those changes when the user
/// picks a different preset.
double balanceToOffset(int balance);

/// Same, using the preset's own slider positions.
dsp::DspParameters resolveParameters(const Preset& preset);

/// The presets shipped with AudioLens, in the order they should be offered.
const std::vector<Preset>& builtinPresets();

/// Looks a preset up by id (e.g. "movie"). Returns nullptr if there is none.
const Preset* findBuiltinPreset(const std::string& id);

}  // namespace audiolens
