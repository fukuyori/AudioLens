#include "app/settings_store.h"

#include "app/preset_json.h"

#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace audiolens::app {
namespace {

QJsonObject readJsonObject(const QString& path) {
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    return document.isObject() ? document.object() : QJsonObject{};
}

/// Writes through QSaveFile so an interrupted write cannot leave a truncated
/// settings file behind: the original stays intact until the new one is whole.
bool writeJsonObject(const QString& path, const QJsonObject& json, QString* error) {
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 を書き込めません: %2").arg(path, file.errorString());
        }
        return false;
    }
    file.write(QJsonDocument(json).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 を保存できません: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

}  // namespace

SettingsStore::SettingsStore() {
    // Resolves to %APPDATA%\AudioLens. AppDataLocation is <roaming>/<org>/<app>
    // and Qt drops empty components, so main() deliberately leaves the
    // organisation name unset rather than repeating "AudioLens" twice in the
    // path. (GenericDataLocation would not do: on Windows it is %LOCALAPPDATA%.)
    directory_ = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(directory_);
    QDir().mkpath(userPresetDirectory());
}

QString SettingsStore::settingsPath() const {
    return QDir(directory_).filePath(QStringLiteral("settings.json"));
}

QString SettingsStore::userPresetDirectory() const {
    return QDir(directory_).filePath(QStringLiteral("presets"));
}

AppSettings SettingsStore::loadSettings() const {
    AppSettings settings;
    const QJsonObject json = readJsonObject(settingsPath());
    if (json.isEmpty()) {
        return settings;
    }
    settings.restored = true;

    settings.captureDeviceId = json[QStringLiteral("captureDeviceId")].toString();
    settings.renderDeviceId = json[QStringLiteral("renderDeviceId")].toString();
    settings.activePresetId =
        json[QStringLiteral("activePresetId")].toString(settings.activePresetId);
    settings.outputVolume =
        std::clamp(json[QStringLiteral("outputVolume")].toInt(settings.outputVolume), 0, 100);
    settings.startWithWindows =
        json[QStringLiteral("startWithWindows")].toBool(settings.startWithWindows);
    settings.startMinimized =
        json[QStringLiteral("startMinimized")].toBool(settings.startMinimized);
    settings.takeOverDefaultDevice =
        json[QStringLiteral("takeOverDefaultDevice")].toBool(settings.takeOverDefaultDevice);
    settings.processingEnabled =
        json[QStringLiteral("processingEnabled")].toBool(settings.processingEnabled);
    settings.previousDefaultDeviceId = json[QStringLiteral("previousDefaultDeviceId")].toString();

    const QJsonObject sliders = json[QStringLiteral("sliders")].toObject();
    settings.sliders.bass = sliders[QStringLiteral("bass")].toInt(settings.sliders.bass);
    settings.sliders.clarity = sliders[QStringLiteral("clarity")].toInt(settings.sliders.clarity);
    settings.sliders.leveling = sliders[QStringLiteral("leveling")].toInt(settings.sliders.leveling);
    settings.sliders = settings.sliders.clamped();

    const QJsonObject profiles = json[QStringLiteral("deviceProfiles")].toObject();
    for (auto it = profiles.begin(); it != profiles.end(); ++it) {
        const QJsonObject entry = it.value().toObject();
        DeviceProfile profile;
        profile.presetId = entry[QStringLiteral("presetId")].toString();
        // An entry naming no preset would silently do nothing on being applied,
        // which is worse than not being there at all.
        if (profile.presetId.isEmpty()) {
            continue;
        }
        const QJsonObject profileSliders = entry[QStringLiteral("sliders")].toObject();
        profile.sliders.bass = profileSliders[QStringLiteral("bass")].toInt(50);
        profile.sliders.clarity = profileSliders[QStringLiteral("clarity")].toInt(50);
        profile.sliders.leveling = profileSliders[QStringLiteral("leveling")].toInt(50);
        profile.sliders = profile.sliders.clamped();
        profile.outputVolume =
            std::clamp(entry[QStringLiteral("outputVolume")].toInt(100), 0, 100);
        settings.deviceProfiles.insert(it.key(), profile);
    }

    return settings;
}

bool SettingsStore::saveSettings(const AppSettings& settings, QString* error) const {
    QJsonObject sliders;
    sliders[QStringLiteral("bass")] = settings.sliders.bass;
    sliders[QStringLiteral("clarity")] = settings.sliders.clarity;
    sliders[QStringLiteral("leveling")] = settings.sliders.leveling;

    QJsonObject json;
    json[QStringLiteral("schemaVersion")] = kPresetSchemaVersion;
    json[QStringLiteral("captureDeviceId")] = settings.captureDeviceId;
    json[QStringLiteral("renderDeviceId")] = settings.renderDeviceId;
    json[QStringLiteral("activePresetId")] = settings.activePresetId;
    json[QStringLiteral("sliders")] = sliders;
    json[QStringLiteral("outputVolume")] = settings.outputVolume;
    json[QStringLiteral("startWithWindows")] = settings.startWithWindows;
    json[QStringLiteral("startMinimized")] = settings.startMinimized;
    json[QStringLiteral("takeOverDefaultDevice")] = settings.takeOverDefaultDevice;
    json[QStringLiteral("processingEnabled")] = settings.processingEnabled;
    json[QStringLiteral("previousDefaultDeviceId")] = settings.previousDefaultDeviceId;

    QJsonObject profiles;
    for (auto it = settings.deviceProfiles.begin(); it != settings.deviceProfiles.end(); ++it) {
        QJsonObject profileSliders;
        profileSliders[QStringLiteral("bass")] = it.value().sliders.bass;
        profileSliders[QStringLiteral("clarity")] = it.value().sliders.clarity;
        profileSliders[QStringLiteral("leveling")] = it.value().sliders.leveling;

        QJsonObject entry;
        entry[QStringLiteral("presetId")] = it.value().presetId;
        entry[QStringLiteral("sliders")] = profileSliders;
        entry[QStringLiteral("outputVolume")] = it.value().outputVolume;
        profiles[it.key()] = entry;
    }
    json[QStringLiteral("deviceProfiles")] = profiles;

    return writeJsonObject(settingsPath(), json, error);
}

QVector<Preset> SettingsStore::loadUserPresets() const {
    QVector<Preset> presets;

    QDir dir(userPresetDirectory());
    const QStringList files = dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& fileName : files) {
        const QJsonObject json = readJsonObject(dir.filePath(fileName));
        if (json.isEmpty()) {
            continue;
        }
        Preset preset;
        QString error;
        // A single corrupt or too-new preset must not stop the others loading.
        if (presetFromJson(json, &preset, &error)) {
            presets.push_back(std::move(preset));
        }
    }
    return presets;
}

bool SettingsStore::saveUserPreset(const Preset& preset, QString* error) const {
    QDir().mkpath(userPresetDirectory());
    const QString path =
        QDir(userPresetDirectory()).filePath(QString::fromStdString(preset.id) + QStringLiteral(".json"));
    return writeJsonObject(path, presetToJson(preset), error);
}

bool SettingsStore::deleteUserPreset(const QString& id, QString* error) const {
    const QString path = QDir(userPresetDirectory()).filePath(id + QStringLiteral(".json"));
    QFile file(path);
    if (!file.exists()) {
        return true;
    }
    if (!file.remove()) {
        if (error != nullptr) {
            *error = QStringLiteral("%1 を削除できません: %2").arg(path, file.errorString());
        }
        return false;
    }
    return true;
}

QString SettingsStore::makeUserPresetId(const QString& displayName) {
    // The id becomes a file name, so anything outside a conservative set is
    // replaced rather than trusted. Japanese names are common here and would
    // otherwise be lost entirely, so a hash keeps them distinguishable.
    static const QRegularExpression unsafe(QStringLiteral("[^a-zA-Z0-9_-]+"));

    QString slug = displayName.trimmed().toLower();
    slug.replace(unsafe, QStringLiteral("_"));
    slug = slug.mid(0, 32);
    while (slug.startsWith(QLatin1Char('_'))) {
        slug.remove(0, 1);
    }
    while (slug.endsWith(QLatin1Char('_'))) {
        slug.chop(1);
    }

    const auto hash = static_cast<quint32>(qHash(displayName.trimmed()));
    const QString suffix = QStringLiteral("%1").arg(hash, 8, 16, QLatin1Char('0'));

    return slug.isEmpty() ? QStringLiteral("user_%1").arg(suffix)
                          : QStringLiteral("user_%1_%2").arg(slug, suffix);
}

}  // namespace audiolens::app
