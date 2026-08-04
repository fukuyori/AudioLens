#include "app/preset_json.h"

#include <QJsonArray>

namespace audiolens::app {
namespace {

QJsonObject speechBandToJson(const dsp::SpeechBand& band) {
    QJsonObject json;
    json[QStringLiteral("freqHz")] = band.freqHz;
    json[QStringLiteral("q")] = band.q;
    json[QStringLiteral("gainDb")] = band.gainDb;
    return json;
}

dsp::SpeechBand speechBandFromJson(const QJsonObject& json) {
    dsp::SpeechBand band;
    band.freqHz = json[QStringLiteral("freqHz")].toDouble(band.freqHz);
    band.q = json[QStringLiteral("q")].toDouble(band.q);
    band.gainDb = json[QStringLiteral("gainDb")].toDouble(band.gainDb);
    return band;
}

}  // namespace

QJsonObject presetToJson(const Preset& preset) {
    const PresetMapping& m = preset.mapping;

    QJsonObject sliders;
    sliders[QStringLiteral("bass")] = preset.sliders.bass;
    sliders[QStringLiteral("clarity")] = preset.sliders.clarity;
    sliders[QStringLiteral("leveling")] = preset.sliders.leveling;

    QJsonObject bass;
    bass[QStringLiteral("highpassFreqAt100Hz")] = m.highpassFreqAt100Hz;
    bass[QStringLiteral("highpassQ")] = m.highpassQ;
    bass[QStringLiteral("lowShelfFreqHz")] = m.lowShelfFreqHz;
    bass[QStringLiteral("lowShelfGainAt0Db")] = m.lowShelfGainAt0Db;
    bass[QStringLiteral("lowShelfGainAt100Db")] = m.lowShelfGainAt100Db;
    bass[QStringLiteral("lowShelfQ")] = m.lowShelfQ;

    QJsonArray bands;
    for (const dsp::SpeechBand& band : m.speechBandsAt100) {
        bands.append(speechBandToJson(band));
    }

    QJsonObject clarity;
    clarity[QStringLiteral("speechBandsAt100")] = bands;
    clarity[QStringLiteral("highShelfFreqHz")] = m.highShelfFreqHz;
    clarity[QStringLiteral("highShelfGainAt0Db")] = m.highShelfGainAt0Db;
    clarity[QStringLiteral("highShelfGainAt100Db")] = m.highShelfGainAt100Db;
    clarity[QStringLiteral("highShelfQ")] = m.highShelfQ;

    QJsonObject leveling;
    leveling[QStringLiteral("thresholdAt0Db")] = m.compressorThresholdAt0Db;
    leveling[QStringLiteral("thresholdAt100Db")] = m.compressorThresholdAt100Db;
    leveling[QStringLiteral("ratioAt0")] = m.compressorRatioAt0;
    leveling[QStringLiteral("ratioAt100")] = m.compressorRatioAt100;
    leveling[QStringLiteral("kneeDb")] = m.compressorKneeDb;
    leveling[QStringLiteral("attackMs")] = m.compressorAttackMs;
    leveling[QStringLiteral("releaseMs")] = m.compressorReleaseMs;
    leveling[QStringLiteral("makeupReferenceDb")] = m.compressorMakeupReferenceDb;

    QJsonObject output;
    output[QStringLiteral("outputGainDb")] = m.outputGainDb;
    output[QStringLiteral("limiterCeilingDb")] = m.limiterCeilingDb;
    output[QStringLiteral("limiterLookaheadMs")] = m.limiterLookaheadMs;
    output[QStringLiteral("limiterReleaseMs")] = m.limiterReleaseMs;

    QJsonObject json;
    json[QStringLiteral("schemaVersion")] = kPresetSchemaVersion;
    json[QStringLiteral("id")] = QString::fromStdString(preset.id);
    json[QStringLiteral("name")] = QString::fromStdString(preset.name);
    json[QStringLiteral("description")] = QString::fromStdString(preset.description);
    json[QStringLiteral("sliders")] = sliders;
    json[QStringLiteral("bass")] = bass;
    json[QStringLiteral("clarity")] = clarity;
    json[QStringLiteral("leveling")] = leveling;
    json[QStringLiteral("output")] = output;
    return json;
}

bool presetFromJson(const QJsonObject& json, Preset* out, QString* error) {
    const auto fail = [error](const QString& message) {
        if (error != nullptr) {
            *error = message;
        }
        return false;
    };

    const int version = json[QStringLiteral("schemaVersion")].toInt(0);
    if (version <= 0) {
        return fail(QStringLiteral("schemaVersion がありません。"));
    }
    if (version > kPresetSchemaVersion) {
        return fail(QStringLiteral("このプリセットは新しい形式 (v%1) です。AudioLens を更新してください。")
                        .arg(version));
    }

    const QString id = json[QStringLiteral("id")].toString();
    const QString name = json[QStringLiteral("name")].toString();
    if (id.isEmpty() || name.isEmpty()) {
        return fail(QStringLiteral("id または name が空です。"));
    }

    // Anything absent falls back to the default, so a hand-edited file that
    // only overrides one value still loads.
    Preset preset;
    preset.id = id.toStdString();
    preset.name = name.toStdString();
    preset.description = json[QStringLiteral("description")].toString().toStdString();

    const QJsonObject sliders = json[QStringLiteral("sliders")].toObject();
    preset.sliders.bass = sliders[QStringLiteral("bass")].toInt(preset.sliders.bass);
    preset.sliders.clarity = sliders[QStringLiteral("clarity")].toInt(preset.sliders.clarity);
    preset.sliders.leveling = sliders[QStringLiteral("leveling")].toInt(preset.sliders.leveling);
    preset.sliders = preset.sliders.clamped();

    PresetMapping& m = preset.mapping;

    const QJsonObject bass = json[QStringLiteral("bass")].toObject();
    m.highpassFreqAt100Hz =
        bass[QStringLiteral("highpassFreqAt100Hz")].toDouble(m.highpassFreqAt100Hz);
    m.highpassQ = bass[QStringLiteral("highpassQ")].toDouble(m.highpassQ);
    m.lowShelfFreqHz = bass[QStringLiteral("lowShelfFreqHz")].toDouble(m.lowShelfFreqHz);
    m.lowShelfGainAt0Db = bass[QStringLiteral("lowShelfGainAt0Db")].toDouble(m.lowShelfGainAt0Db);
    m.lowShelfGainAt100Db =
        bass[QStringLiteral("lowShelfGainAt100Db")].toDouble(m.lowShelfGainAt100Db);
    m.lowShelfQ = bass[QStringLiteral("lowShelfQ")].toDouble(m.lowShelfQ);

    const QJsonObject clarity = json[QStringLiteral("clarity")].toObject();
    m.speechBandsAt100.clear();
    for (const QJsonValue& value : clarity[QStringLiteral("speechBandsAt100")].toArray()) {
        m.speechBandsAt100.push_back(speechBandFromJson(value.toObject()));
    }
    if (m.speechBandsAt100.empty()) {
        return fail(QStringLiteral("speechBandsAt100 が空です。"));
    }
    m.highShelfFreqHz = clarity[QStringLiteral("highShelfFreqHz")].toDouble(m.highShelfFreqHz);
    m.highShelfGainAt0Db =
        clarity[QStringLiteral("highShelfGainAt0Db")].toDouble(m.highShelfGainAt0Db);
    m.highShelfGainAt100Db =
        clarity[QStringLiteral("highShelfGainAt100Db")].toDouble(m.highShelfGainAt100Db);
    m.highShelfQ = clarity[QStringLiteral("highShelfQ")].toDouble(m.highShelfQ);

    const QJsonObject leveling = json[QStringLiteral("leveling")].toObject();
    m.compressorThresholdAt0Db =
        leveling[QStringLiteral("thresholdAt0Db")].toDouble(m.compressorThresholdAt0Db);
    m.compressorThresholdAt100Db =
        leveling[QStringLiteral("thresholdAt100Db")].toDouble(m.compressorThresholdAt100Db);
    m.compressorRatioAt0 = leveling[QStringLiteral("ratioAt0")].toDouble(m.compressorRatioAt0);
    m.compressorRatioAt100 = leveling[QStringLiteral("ratioAt100")].toDouble(m.compressorRatioAt100);
    m.compressorKneeDb = leveling[QStringLiteral("kneeDb")].toDouble(m.compressorKneeDb);
    m.compressorAttackMs = leveling[QStringLiteral("attackMs")].toDouble(m.compressorAttackMs);
    m.compressorReleaseMs = leveling[QStringLiteral("releaseMs")].toDouble(m.compressorReleaseMs);
    m.compressorMakeupReferenceDb =
        leveling[QStringLiteral("makeupReferenceDb")].toDouble(m.compressorMakeupReferenceDb);

    const QJsonObject output = json[QStringLiteral("output")].toObject();
    m.outputGainDb = output[QStringLiteral("outputGainDb")].toDouble(m.outputGainDb);
    m.limiterCeilingDb = output[QStringLiteral("limiterCeilingDb")].toDouble(m.limiterCeilingDb);
    m.limiterLookaheadMs =
        output[QStringLiteral("limiterLookaheadMs")].toDouble(m.limiterLookaheadMs);
    m.limiterReleaseMs = output[QStringLiteral("limiterReleaseMs")].toDouble(m.limiterReleaseMs);

    *out = std::move(preset);
    return true;
}

}  // namespace audiolens::app
