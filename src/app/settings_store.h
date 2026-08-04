#pragma once

#include "core/preset.h"

#include <QString>
#include <QVector>

namespace audiolens::app {

/// Everything the app remembers between runs, other than the presets.
struct AppSettings {
    /// False on a first run, where there is nothing stored yet. The caller
    /// needs to know, because "no saved slider positions" and "saved positions
    /// that happen to equal the defaults" call for different behaviour.
    bool restored = false;

    QString captureDeviceId;
    QString renderDeviceId;

    QString activePresetId = QStringLiteral("standard");
    SliderValues sliders;

    bool startWithWindows = false;
    bool startMinimized = false;

    /// Whether processing was on when the app last exited, and therefore
    /// whether to switch it on again at startup. Without this, "start with
    /// Windows" only produces a tray icon that does nothing: the app would be
    /// running but not processing, which is not what the user asked for.
    bool processingEnabled = false;

    /// Whether AudioLens makes its capture device the system default while it
    /// runs. On by default: without it the user has to change the output device
    /// by hand every time they start and stop the app.
    bool takeOverDefaultDevice = true;

    /// The system default output as it was before AudioLens redirected it,
    /// written *before* the switch and cleared *after* the restore. A non-empty
    /// value at startup therefore means the last run was killed without giving
    /// the device back, and the routing has to be repaired (requirement N-04).
    QString previousDefaultDeviceId;
};

/// Reads and writes `%APPDATA%\AudioLens`.
///
/// Everything is stored as indented JSON: a user who wants to know what the app
/// remembers about them, or to hand-edit a preset, can just open the file
/// (requirement N-09).
class SettingsStore {
public:
    SettingsStore();

    /// Directory the settings and presets live in. Created on demand.
    QString directory() const { return directory_; }

    AppSettings loadSettings() const;
    bool saveSettings(const AppSettings& settings, QString* error = nullptr) const;

    /// User presets only. The built-ins come from `builtinPresets()` and are
    /// never written to disk, so an updated build can improve them.
    QVector<Preset> loadUserPresets() const;

    /// Saves under `preset.id`, replacing any existing preset with that id.
    bool saveUserPreset(const Preset& preset, QString* error = nullptr) const;
    bool deleteUserPreset(const QString& id, QString* error = nullptr) const;

    /// Turns a display name into an id that is safe as a file name and does not
    /// collide with a built-in.
    static QString makeUserPresetId(const QString& displayName);

private:
    QString settingsPath() const;
    QString userPresetDirectory() const;

    QString directory_;
};

}  // namespace audiolens::app
