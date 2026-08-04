#pragma once

#include "core/preset.h"

#include <QMap>
#include <QString>
#include <QVector>

namespace audiolens::app {

/// What was in use on one particular output device (requirement F-13).
///
/// Keyed by the *render* device rather than the capture one: the capture side is
/// the virtual cable and never changes, while the output is headphones one hour
/// and speakers the next — and those want different corrections. Headphones
/// need less bass control than a desk speaker sitting on a resonant surface,
/// and the volume that suits one is painful on the other.
struct DeviceProfile {
    QString presetId;
    SliderValues sliders;
    int outputVolume = 100;
};

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

    /// Master output level, 0-100, where 100 is unity gain (requirement F-22).
    ///
    /// Deliberately attenuation only. A master that could also boost would need
    /// the limiter behind it to stay safe, and the limiter is exactly what the
    /// passthrough preset exists to avoid; making things louder is what the
    /// presets are for. This is the control for "that is too loud", which is
    /// what a volume control means to everyone who has ever used one.
    int outputVolume = 100;

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

    /// Remembered settings per output device, keyed by device id. A device with
    /// no entry here is simply not being remembered, which is the default: the
    /// feature only starts applying once the user asks it to for a given device.
    QMap<QString, DeviceProfile> deviceProfiles;
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
