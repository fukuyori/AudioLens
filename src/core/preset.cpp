#include "core/preset.h"

#include <algorithm>
#include <cmath>

namespace audiolens {
namespace {

/// Slider position as a 0..1 amount.
double amount(int slider) { return std::clamp(slider, 0, 100) / 100.0; }

double lerp(double at0, double at100, double t) { return at0 + (at100 - at0) * t; }

Preset makeConversation() {
    Preset p;
    p.id = "conversation";
    p.name = "Conversation";
    p.description = "Brings voices in calls and meetings forward and evens out loudness firmly.";
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
    // Conference audio is usually near-mono, so the sides hold little. Lifting
    // the speech range in the mid channel still keeps whatever room noise is
    // spread across the image from coming up with the voice.
    m.midSideEnabled = true;
    m.sideGainAt100Db = -3.0;
    // Calls swing between a close talker and a distant one, over seconds.
    m.autoGainEnabled = true;
    m.autoGainTargetLufs = -18.0;
    m.autoGainRangeAt100Db = 10.0;
    // The heaviest speech lift of any preset (+6 dB at 2.6 kHz), so the most
    // sibilance to take back out.
    m.deEsserEnabled = true;
    m.deEsserFreqHz = 6800.0;
    m.deEsserAmountAt100 = 0.8;
    m.deEsserMaxReductionAt100Db = 9.0;
    m.compressorThresholdAt100Db = -30.0;
    m.compressorRatioAt100 = 6.0;
    m.compressorAttackMs = 8.0;
    m.compressorReleaseMs = 180.0;
    return p;
}

Preset makeLecture() {
    Preset p;
    p.id = "lecture";
    p.name = "Lecture";
    p.description = "Moderate clarity with the harsh top held back, so long listening stays comfortable.";
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
    m.midSideEnabled = true;
    m.sideGainAt100Db = -2.0;
    // A lecture runs for an hour and the speaker drifts from the microphone.
    // Slow and gentle: nothing here should be audible as movement.
    m.autoGainEnabled = true;
    m.autoGainTargetLufs = -19.0;
    m.autoGainRangeAt100Db = 8.0;
    m.autoGainRiseDbPerSecond = 0.04;
    m.autoGainFallDbPerSecond = 0.08;
    // This preset's whole promise is that an hour of it is not tiring, and
    // sharpened sibilants are the fastest way to break that promise.
    m.deEsserEnabled = true;
    m.deEsserThresholdDb = -15.0;
    m.compressorThresholdAt100Db = -26.0;
    m.compressorRatioAt100 = 4.0;
    m.compressorAttackMs = 15.0;
    m.compressorReleaseMs = 300.0;
    return p;
}

Preset makeMovie() {
    Preset p;
    p.id = "movie";
    p.name = "Film";
    p.description = "Narrows the gap between dialogue and effects while keeping the weight of the bass.";
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
    // This is where mid/side earns its place. Film dialogue is mixed dead
    // centre and the score and effects are spread wide, so lifting the speech
    // range in the mid channel raises the dialogue without dragging the music
    // up with it — which is exactly what the same lift does on L and R.
    m.midSideEnabled = true;
    m.sideGainAt100Db = -4.0;
    // A quiet dialogue scene and the action scene twenty seconds later are
    // minutes apart in the compressor's terms. Only the slow stage can see
    // that far.
    m.autoGainEnabled = true;
    m.autoGainTargetLufs = -20.0;
    m.autoGainRangeAt100Db = 12.0;
    m.autoGainRiseDbPerSecond = 0.05;
    m.autoGainFallDbPerSecond = 0.10;
    // Lighter than the speech presets: a film mix has usually been de-essed
    // once already, and taking more out of it dulls the effects track too.
    m.deEsserEnabled = true;
    m.deEsserAmountAt100 = 0.5;
    m.deEsserMaxReductionAt100Db = 5.0;
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
    p.name = "Late night";
    p.description = "Levels as far as it goes so quiet playback stays audible, and cuts the bass hard.";
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
    m.midSideEnabled = true;
    m.sideGainAt100Db = -5.0;
    // At night the whole point is that nothing is ever loud, so the slow stage
    // gets the widest range of any preset and a low target to aim at.
    m.autoGainEnabled = true;
    m.autoGainTargetLufs = -22.0;
    m.autoGainRangeAt100Db = 14.0;
    m.autoGainRiseDbPerSecond = 0.08;
    m.autoGainFallDbPerSecond = 0.16;
    m.deEsserEnabled = true;
    m.compressorThresholdAt100Db = -40.0;
    m.compressorRatioAt100 = 10.0;
    m.compressorAttackMs = 8.0;
    m.compressorReleaseMs = 350.0;
    m.compressorMakeupReferenceDb = -24.0;
    return p;
}

Preset makeGame() {
    Preset p;
    p.id = "game";
    p.name = "Game";
    p.description = "Cuts the lows and lifts the upper mids, making quiet sounds and their direction clear.";
    p.sliders = {80, 85, 55};

    PresetMapping& m = p.mapping;
    // The most aggressive low cut in the set, and deliberately so. Explosions,
    // engines and ambience all live below 200 Hz and none of them carry
    // information a player acts on; a footstep does, and it does not. Taking
    // the bottom out is what stops the rest from being masked by it.
    m.highpassFreqAt100Hz = 200.0;
    m.lowShelfFreqHz = 320.0;
    m.lowShelfGainAt100Db = -7.0;
    // Footsteps, reloads and cloth movement sit here, and so do the spectral
    // cues the outer ear uses to tell front from behind and above from below.
    // Three bands rather than one broad lift: a single wide bump would bring up
    // the gunfire in the same range along with them.
    m.speechBandsAt100 = {
        {2200.0, 1.1, 4.0},
        {4000.0, 1.2, 5.0},
        {6500.0, 1.2, 3.5},
    };
    m.highShelfFreqHz = 9000.0;
    m.highShelfGainAt100Db = 2.0;
    // The one preset that *widens* rather than narrows. Everywhere else the
    // side channel is held back to push a centred voice forward; here the side
    // channel is the whole point, because the left/right difference is what a
    // player localises with. Raising it exaggerates that difference.
    //
    // +2.5 dB and no more. Past roughly 3 dB the image stops widening and
    // starts hollowing out: centre content begins to cancel, and in a game the
    // centre is where the enemy directly ahead is.
    m.midSideEnabled = true;
    m.sideGainAt100Db = 2.5;
    // Off, and this is the one place it costs something to say no to. The slow
    // stage rides the overall gain over minutes, and level is one of the cues a
    // player judges distance by — a footstep that gets louder because the last
    // thirty seconds were quiet is a footstep that reads as closer than it is.
    // Making quiet sounds audible is the compressor's job here, inside the
    // event, where it does not move the reference.
    m.autoGainEnabled = false;
    // Off, and this is the only preset in the set where it is. The de-esser
    // exists to tame the sharpness the speech bands add to a *voice*; in a game
    // the same 6-8 kHz region is carrying glass, casings and distant fire, and
    // those are direction, not sibilance. Measured on the dynamic test material,
    // with it on 8 kHz landed 0.7 dB *below* the preset's own overall gain while
    // 2.5 and 4 kHz were 2 dB above it — the stage was undoing the band above
    // it. Off, 8 kHz sits 0.7 dB above instead, a 1.4 dB swing.
    m.deEsserEnabled = false;
    // A low threshold brings the quiet detail up, but the ratio stays moderate:
    // at 8:1 a gunshot and a footstep end up the same size, and then there is
    // no loudness left to judge distance with at all.
    m.compressorThresholdAt100Db = -34.0;
    m.compressorRatioAt100 = 3.5;
    m.compressorAttackMs = 5.0;
    m.compressorReleaseMs = 150.0;
    m.compressorMakeupReferenceDb = -20.0;
    // The only latency this app's DSP has is the limiter's look-ahead, so this
    // is the only latency a preset can do anything about: 2.0 ms becomes
    // 0.5 ms. The window still spans 24 samples at 48 kHz and the gain still
    // reaches its target inside it, so it is a shorter limiter, not a broken
    // one.
    //
    // Worth being clear about the size of this. End to end the app runs around
    // 60 ms and nearly all of it is the WASAPI buffer, which this cannot touch
    // and must not: buffer margin is what prevents dropouts, and dropouts are
    // worse than latency. 1.5 ms is what is honestly available.
    m.limiterLookaheadMs = 0.5;
    m.limiterReleaseMs = 40.0;
    return p;
}

Preset makeStandard() {
    Preset p;
    p.id = "standard";
    p.category = PresetCategory::Passthrough;
    p.name = "Standard";
    p.description = "No correction. The sound passes through untouched, with no processing and no latency.";
    // Every amount at zero. "標準" is the setting you leave selected when you
    // want AudioLens out of the way: the chain detects that nothing is being
    // applied and hands the audio straight back, adding no latency at all.
    // Moving any slider off zero switches the processing — and its 2 ms of
    // limiter look-ahead — back on.
    p.sliders = {0, 0, 0};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 90.0;
    m.lowShelfGainAt100Db = -2.0;
    m.speechBandsAt100 = {
        {2000.0, 1.0, 3.0},
    };
    m.highShelfGainAt100Db = 0.5;
    // Light everywhere, including here: the standard preset is meant to be the
    // one you can leave on without ever noticing it.
    m.midSideEnabled = true;
    m.sideGainAt100Db = -1.5;
    m.autoGainEnabled = true;
    m.autoGainTargetLufs = -18.0;
    m.autoGainRangeAt100Db = 6.0;
    m.autoGainRiseDbPerSecond = 0.04;
    m.autoGainFallDbPerSecond = 0.08;
    m.deEsserEnabled = true;
    m.deEsserAmountAt100 = 0.5;
    m.deEsserMaxReductionAt100Db = 5.0;
    m.compressorThresholdAt100Db = -24.0;
    m.compressorRatioAt100 = 3.0;
    return p;
}

// --- 音楽 -------------------------------------------------------------------
//
// Music presets are mostly about restraint. Everything else here exists to make
// speech easier to follow, and speech processing applied to music is audible as
// damage: the mid lift that pulls a voice forward also pulls a snare forward,
// and the levelling that closes the gap between a whisper and an explosion also
// closes the one between a verse and a chorus, which is the arrangement.
//
// So these do less, and what they do is chosen to leave the record's own shape
// alone. Two rules run through all four:
//
//   * The side channel is never trimmed. On dialogue, holding the sides back
//     brings the centre forward; on music it collapses the stereo image, and
//     the image is part of the recording. Mid/side is still used where the
//     centre is worth lifting on its own — but at unity side gain, so nothing
//     narrows.
//   * Slow levelling stays on. Records differ in mastering level by more than
//     anything else in this list, and that difference is exactly what a stage
//     working over minutes is for.

Preset makeRock() {
    Preset p;
    p.id = "rock";
    p.category = PresetCategory::Music;
    p.name = "Rock";
    p.description = "Lifts vocals out of distorted guitars and tidies up a saturated low end.";
    p.sliders = {30, 40, 0};

    PresetMapping& m = p.mapping;
    // Rock lives on its low end; the highpass only clears what is below the
    // kick drum's fundamental.
    m.highpassFreqAt100Hz = 60.0;
    m.lowShelfFreqHz = 250.0;
    m.lowShelfGainAt100Db = -2.5;  // the "mud" region where bass and guitar pile up
    m.speechBandsAt100 = {
        {1600.0, 0.9, 2.0},
        {3400.0, 1.1, 3.0},  // where a vocal cuts through a wall of guitars
    };
    m.highShelfFreqHz = 9000.0;
    m.highShelfGainAt100Db = 1.0;
    // Vocals sit centre and the guitars are spread, so the lift can be aimed at
    // the voice alone. The sides keep their level: narrowing a rock mix is very
    // audible and would be a worse trade than the presence gained.
    m.midSideEnabled = true;
    m.sideGainAt100Db = 0.0;
    // Off, like every music preset: the record was already compressed once on
    // purpose, and a second pass is not an improvement on the first.
    m.autoGainEnabled = false;
    m.compressorThresholdAt100Db = -20.0;
    m.compressorRatioAt100 = 3.0;
    m.compressorAttackMs = 20.0;
    m.compressorReleaseMs = 200.0;
    return p;
}

Preset makeJazz() {
    Preset p;
    p.id = "jazz";
    p.category = PresetCategory::Music;
    p.name = "Jazz";
    p.description = "Keeps every instrument of a small group distinct and rounds off only the edgy top.";
    p.sliders = {20, 30, 0};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 50.0;  // double bass goes low; leave it there
    m.lowShelfFreqHz = 180.0;
    m.lowShelfGainAt100Db = -1.5;
    m.speechBandsAt100 = {
        {2500.0, 0.8, 1.5},  // brass and reed presence, gently
    };
    // Cymbals are the loudest thing in a small-group recording and the first
    // thing to become tiring, so the top comes down rather than up.
    m.highShelfFreqHz = 10000.0;
    m.highShelfGainAt100Db = -1.5;
    // Off: in a small group nothing is privileged by being centred. The piano
    // is as likely to be hard left as the drums are to be centre, so lifting
    // the mid would pick an instrument at random.
    m.midSideEnabled = false;
    m.autoGainEnabled = false;
    m.compressorThresholdAt100Db = -22.0;
    m.compressorRatioAt100 = 2.5;
    m.compressorAttackMs = 30.0;   // let the attack of a struck note through
    m.compressorReleaseMs = 400.0;
    return p;
}

Preset makeClassical() {
    Preset p;
    p.id = "classical";
    p.category = PresetCategory::Music;
    p.name = "Classical";
    p.description = "Preserves the dynamics and the hall, and touches as little as possible.";
    p.sliders = {15, 20, 0};

    PresetMapping& m = p.mapping;
    // An orchestra's bottom octave is real content, not rumble.
    m.highpassFreqAt100Hz = 40.0;
    m.lowShelfFreqHz = 150.0;
    m.lowShelfGainAt100Db = -1.0;
    m.speechBandsAt100 = {
        {3000.0, 0.7, 1.5},  // just enough to keep strings from turning to mush
    };
    m.highShelfFreqHz = 11000.0;
    m.highShelfGainAt100Db = 0.0;
    // Off, emphatically. The spatial information in a hall recording is most of
    // what distinguishes it from a studio one, and it lives in the sides.
    m.midSideEnabled = false;
    // A pianissimo movement and the finale of the same symphony can be twenty
    // dB apart, and closing that gap is the one thing this preset must never
    // do. Dynamics *are* the performance.
    m.autoGainEnabled = false;
    m.compressorThresholdAt100Db = -24.0;
    m.compressorRatioAt100 = 3.0;
    m.compressorAttackMs = 40.0;
    m.compressorReleaseMs = 600.0;
    return p;
}

Preset makeAmbient() {
    Preset p;
    p.id = "ambient";
    p.category = PresetCategory::Music;
    p.name = "Ambient";
    p.description = "Raises fine detail at the top a little and leaves the width and the swells alone.";
    p.sliders = {20, 15, 0};

    PresetMapping& m = p.mapping;
    m.highpassFreqAt100Hz = 45.0;
    m.lowShelfFreqHz = 160.0;
    m.lowShelfGainAt100Db = -1.0;
    m.speechBandsAt100 = {
        {4000.0, 0.7, 1.5},  // keeps texture from disappearing at low volume
    };
    m.highShelfFreqHz = 12000.0;
    m.highShelfGainAt100Db = 1.0;
    // Off. Width is not incidental to this music, it is the point of it.
    m.midSideEnabled = false;
    // Ambient records are often mastered very quietly. That is a job for the
    // volume control, not for a stage that would also flatten the swells the
    // music is made of.
    m.autoGainEnabled = false;
    m.compressorThresholdAt100Db = -26.0;
    m.compressorRatioAt100 = 3.0;
    m.compressorAttackMs = 50.0;
    m.compressorReleaseMs = 800.0;
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

    // With the clarity slider at 0 the speech bands are flat and the side gain
    // is unity, so mid/side reconstructs the input exactly — correct, but a
    // waste of arithmetic. Below the threshold it is switched out instead.
    p.midSideEnabled = m.midSideEnabled && clarity > 0.02;
    p.sideGainDb = lerp(0.0, m.sideGainAt100Db, clarity);

    // Scaled by the clarity slider for the same reason it exists at all: the
    // sharpness it removes is the sharpness that slider put there.
    p.deEsser.enabled = m.deEsserEnabled && clarity > 0.02;
    p.deEsser.frequencyHz = m.deEsserFreqHz;
    p.deEsser.q = m.deEsserQ;
    p.deEsser.thresholdDb = m.deEsserThresholdDb;
    p.deEsser.amount = lerp(0.0, m.deEsserAmountAt100, clarity);
    p.deEsser.maxReductionDb = lerp(0.0, m.deEsserMaxReductionAt100Db, clarity);

    // --- 音量差 ---
    // Two stages on two time scales. The slow one takes out the difference
    // between one scene and the next; the compressor takes out the difference
    // between one syllable and the next. Neither can do the other's job.
    p.autoGain.enabled = m.autoGainEnabled && leveling > 0.02;
    p.autoGain.targetLufs = m.autoGainTargetLufs;
    p.autoGain.maxBoostDb = lerp(0.0, m.autoGainRangeAt100Db, leveling);
    p.autoGain.maxCutDb = lerp(0.0, m.autoGainRangeAt100Db, leveling);
    p.autoGain.riseDbPerSecond = m.autoGainRiseDbPerSecond;
    p.autoGain.fallDbPerSecond = m.autoGainFallDbPerSecond;

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

    // Whether anything is actually being applied, decided from the resolved
    // values rather than from which preset was chosen. A preset is not
    // inherently a passthrough; a preset with every slider at zero is, and so
    // is any other preset the user has turned all the way down.
    //
    // 0.01 dB is the same threshold the chain uses to decide a filter is worth
    // redesigning. Below it nothing is audible and the arithmetic is an
    // identity anyway: a peaking or shelving section at 0 dB has numerator and
    // denominator coefficients that are equal term for term.
    // The output gain is excluded on purpose when it only ever turns things
    // down. An attenuation cannot clip, so it needs no limiter behind it, and
    // the chain applies it on the passthrough path without adding any delay.
    // A *positive* gain would need the limiter and so counts as processing.
    constexpr double kAudible = 0.01;
    bool anyProcessing = p.highpassEnabled || p.compressorEnabled || p.autoGain.enabled ||
                         p.deEsser.enabled || std::abs(p.lowShelfGainDb) > kAudible ||
                         std::abs(p.highShelfGainDb) > kAudible ||
                         std::abs(p.inputGainDb) > kAudible || p.outputGainDb > kAudible ||
                         (p.midSideEnabled && std::abs(p.sideGainDb) > kAudible);
    for (int i = 0; i < p.speechBandCount && !anyProcessing; ++i) {
        anyProcessing = std::abs(p.speechBands[i].gainDb) > kAudible;
    }
    p.passthrough = !anyProcessing;

    return p;
}

double outputVolumeToDb(int volume) {
    const int clamped = std::clamp(volume, 0, 100);
    if (clamped >= 100) return 0.0;
    if (clamped <= 0) return -120.0;  // far below audibility; effectively mute

    // A quartic taper on amplitude. Loudness roughly follows the fourth root of
    // power, so this keeps the perceived change even across the travel instead
    // of crowding everything into the top of the slider the way a linear gain
    // does.
    const double fraction = clamped / 100.0;
    return 20.0 * std::log10(fraction * fraction);
}

double balanceToOffset(int balance) {
    return std::clamp(balance, -50, 50) / 50.0;
}

dsp::DspParameters resolveParameters(const Preset& preset) {
    return resolveParameters(preset, preset.sliders);
}

const std::vector<Preset>& builtinPresets() {
    static const std::vector<Preset> presets = {
        // Speech first, then music: the app is for making things easier to
        // hear, and the music presets are the ones that mostly stay out of the
        // way.
        makeStandard(),   makeConversation(), makeLecture(),
        makeMovie(),      makeNight(),        makeGame(),
        makeRock(),       makeJazz(),         makeClassical(),
        makeAmbient(),
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
