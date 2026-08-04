#include "core/preset.h"

#include <algorithm>

namespace audiolens {
namespace {

/// Slider position as a 0..1 amount.
double amount(int slider) { return std::clamp(slider, 0, 100) / 100.0; }

double lerp(double at0, double at100, double t) { return at0 + (at100 - at0) * t; }

Preset makeConversation() {
    Preset p;
    p.id = "conversation";
    p.name = "会話";
    p.description = "通話や会議の声を前に出し、音量差を強く揃えます。";
    p.sliders = {60, 75, 80};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 140.0;  // Rumble and handling noise carry nothing useful here.
    m.lowShelfGainAt100Db = -4.0;
    m.speechBandsAt100 = {
        {1200.0, 1.0, 3.0},
        {2600.0, 1.1, 6.0},  // Consonant definition: where intelligibility lives.
        {4200.0, 1.2, 3.5},
    };
    m.highShelfGainAt100Db = 1.5;
    m.compressorThresholdAt100Db = -30.0;
    m.compressorRatioAt100 = 6.0;
    m.compressorAttackMs = 8.0;
    m.compressorReleaseMs = 180.0;
    return p;
}

Preset makeLecture() {
    Preset p;
    p.id = "lecture";
    p.name = "講義";
    p.description = "長時間聞いても疲れにくいよう、明瞭さは中程度、高域の刺激を抑えます。";
    p.sliders = {50, 55, 55};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 110.0;
    m.lowShelfGainAt100Db = -2.5;
    m.speechBandsAt100 = {
        {1400.0, 1.0, 2.5},
        {2800.0, 1.2, 4.0},
    };
    // Rolling the top off is what makes an hour of listening tolerable.
    m.highShelfFreqHz = 8000.0;
    m.highShelfGainAt100Db = -2.0;
    m.compressorThresholdAt100Db = -26.0;
    m.compressorRatioAt100 = 4.0;
    m.compressorAttackMs = 15.0;
    m.compressorReleaseMs = 300.0;
    return p;
}

Preset makeMovie() {
    Preset p;
    p.id = "movie";
    p.name = "映画";
    p.description = "セリフと効果音の音量差を縮めます。低音の迫力は残します。";
    p.sliders = {35, 55, 80};

    PresetMapping& m = p.mapping;
    // Films need their low end, so the highpass stays low and the shelf gentle.
    m.highpassFreqAt100Hz = 80.0;
    m.lowShelfGainAt100Db = -2.0;
    m.speechBandsAt100 = {
        {1800.0, 1.0, 3.0},
        {3200.0, 1.2, 4.5},
    };
    m.highShelfGainAt100Db = 1.0;
    // A low threshold with a slow release is what actually closes the gap
    // between whispered dialogue and an explosion.
    m.compressorThresholdAt100Db = -34.0;
    m.compressorRatioAt100 = 7.0;
    m.compressorAttackMs = 12.0;
    m.compressorReleaseMs = 400.0;
    m.compressorMakeupReferenceDb = -20.0;
    return p;
}

Preset makeNight() {
    Preset p;
    p.id = "night";
    p.name = "深夜";
    p.description = "小さな音量でも聞こえるよう最大限に揃え、低音を強く抑えます。";
    p.sliders = {85, 65, 100};

    PresetMapping& m = p.mapping;
    // Low frequencies are what travels through walls, so they go first.
    m.highpassFreqAt100Hz = 180.0;
    m.lowShelfGainAt100Db = -8.0;
    m.speechBandsAt100 = {
        {1500.0, 1.0, 3.5},
        {3000.0, 1.2, 5.0},
    };
    m.highShelfGainAt100Db = 0.5;
    m.compressorThresholdAt100Db = -40.0;
    m.compressorRatioAt100 = 10.0;
    m.compressorAttackMs = 8.0;
    m.compressorReleaseMs = 350.0;
    m.compressorMakeupReferenceDb = -24.0;
    return p;
}

Preset makeOldRecording() {
    Preset p;
    p.id = "old_recording";
    p.name = "古い録音";
    p.description = "帯域の狭い古い音源を、聞き取りやすい方向へ補正します。";
    p.sliders = {70, 80, 65};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 150.0;
    m.lowShelfGainAt100Db = -3.0;
    m.speechBandsAt100 = {
        {1000.0, 0.9, 2.5},
        {2400.0, 1.0, 5.5},
        {4500.0, 1.0, 4.0},
    };
    // Old sources roll off early; a shelf restores some of the air they lack.
    m.highShelfFreqHz = 6000.0;
    m.highShelfGainAt100Db = 4.0;
    m.compressorThresholdAt100Db = -28.0;
    m.compressorRatioAt100 = 5.0;
    m.compressorAttackMs = 12.0;
    m.compressorReleaseMs = 250.0;
    return p;
}

Preset makeStandard() {
    Preset p;
    p.id = "standard";
    p.name = "標準";
    p.description = "常用できる軽い補正です。";
    p.sliders = {30, 35, 35};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 90.0;
    m.lowShelfGainAt100Db = -2.0;
    m.speechBandsAt100 = {
        {2000.0, 1.0, 3.0},
    };
    m.highShelfGainAt100Db = 0.5;
    m.compressorThresholdAt100Db = -24.0;
    m.compressorRatioAt100 = 3.0;
    return p;
}

}  // namespace

SliderValues SliderValues::clamped() const {
    return {std::clamp(bass, 0, 100), std::clamp(clarity, 0, 100), std::clamp(leveling, 0, 100)};
}

dsp::DspParameters resolveParameters(const Preset& preset, const SliderValues& rawSliders) {
    const SliderValues sliders = rawSliders.clamped();
    const PresetMapping& m = preset.mapping;

    const double bass = amount(sliders.bass);
    const double clarity = amount(sliders.clarity);
    const double leveling = amount(sliders.leveling);

    dsp::DspParameters p;

    // --- 低音 ---
    // The corner sweeps up from 20 Hz (effectively inaudible, so "off") to the
    // preset's maximum. Below a few percent the filter is disabled outright
    // rather than left sitting at the bottom of the audio band.
    p.highpassEnabled = bass > 0.02;
    p.highpassFreqHz = lerp(20.0, m.highpassFreqAt100Hz, bass);
    p.highpassQ = m.highpassQ;
    p.lowShelfFreqHz = m.lowShelfFreqHz;
    p.lowShelfGainDb = lerp(m.lowShelfGainAt0Db, m.lowShelfGainAt100Db, bass);
    p.lowShelfQ = m.lowShelfQ;

    // --- 声の明瞭さ ---
    const auto bandCount =
        std::min<std::size_t>(m.speechBandsAt100.size(), dsp::kMaxSpeechBands);
    p.speechBandCount = static_cast<int>(bandCount);
    for (std::size_t i = 0; i < bandCount; ++i) {
        const dsp::SpeechBand& source = m.speechBandsAt100[i];
        p.speechBands[i].freqHz = source.freqHz;
        p.speechBands[i].q = source.q;
        p.speechBands[i].gainDb = source.gainDb * clarity;
    }
    p.highShelfFreqHz = m.highShelfFreqHz;
    p.highShelfGainDb = lerp(m.highShelfGainAt0Db, m.highShelfGainAt100Db, clarity);
    p.highShelfQ = m.highShelfQ;

    // --- 音量差 ---
    // At a slider of 0 the ratio interpolates to 1:1, which is a no-op, so the
    // compressor is switched out entirely rather than run for nothing.
    p.compressorEnabled = leveling > 0.02;
    p.compressor.thresholdDb =
        lerp(m.compressorThresholdAt0Db, m.compressorThresholdAt100Db, leveling);
    p.compressor.ratio = lerp(m.compressorRatioAt0, m.compressorRatioAt100, leveling);
    p.compressor.kneeDb = m.compressorKneeDb;
    p.compressor.attackMs = m.compressorAttackMs;
    p.compressor.releaseMs = m.compressorReleaseMs;
    p.compressor.makeupReferenceDb = m.compressorMakeupReferenceDb;

    // --- 固定 ---
    p.inputGainDb = 0.0;
    p.outputGainDb = m.outputGainDb;
    p.limiter.ceilingDb = m.limiterCeilingDb;
    p.limiter.lookaheadMs = m.limiterLookaheadMs;
    p.limiter.releaseMs = m.limiterReleaseMs;

    return p;
}

dsp::DspParameters resolveParameters(const Preset& preset) {
    return resolveParameters(preset, preset.sliders);
}

const std::vector<Preset>& builtinPresets() {
    static const std::vector<Preset> presets = {
        makeStandard(), makeConversation(), makeLecture(),
        makeMovie(),    makeNight(),        makeOldRecording(),
    };
    return presets;
}

const Preset* findBuiltinPreset(const std::string& id) {
    for (const Preset& preset : builtinPresets()) {
        if (preset.id == id) {
            return &preset;
        }
    }
    return nullptr;
}

}  // namespace audiolens
