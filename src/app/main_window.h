#pragma once

#include "app/audio_controller.h"
#include "app/settings_store.h"
#include "core/preset.h"

#include <QIcon>
#include <QMainWindow>
#include <QVector>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QMenu;
class QPushButton;
class QSlider;
class QSystemTrayIcon;

namespace audiolens::app {

class GainReductionMeter;
class LevelMeter;

/// The whole user interface: preset list, the three amounts, device routing,
/// meters and the A/B button, all on one screen (requirement F-30).
class MainWindow : public QMainWindow {
    Q_OBJECT

public:
    MainWindow();
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent* event) override;

    /// Catches the two gestures that put the balance back to centre. They are
    /// mouse events on widgets that do not otherwise report them, so a filter
    /// is the way to reach them without subclassing QSlider and QLabel.
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    // --- construction ---
    QWidget* buildPresetSection();
    QWidget* buildSliderSection();
    QWidget* buildMeterSection();
    QWidget* buildDeviceSection();
    QWidget* buildFooter();
    void buildTrayIcon();

    // --- state flow ---
    void reloadPresets();
    void refreshDeviceLists();
    void selectPresetById(const QString& id);
    void onPresetSelectionChanged();
    void onSliderChanged();
    void onOutputVolumeChanged();
    void onBalanceChanged();
    void onDeviceChanged();
    void onPowerToggled(bool on);
    void onSaveUserPreset();
    void onDeleteUserPreset();
    void onStartWithWindowsToggled(bool enabled);
    void onTakeOverToggled(bool enabled);
    void onRememberDeviceToggled(bool enabled);
    void onLanguageChanged();

    // --- device-linked presets (requirement F-13) ---
    /// Applies whatever was remembered for `renderDeviceId`, if anything.
    void applyDeviceProfile(const QString& renderDeviceId);
    /// Writes the current preset, amounts and volume into the profile for the
    /// current output device, when that device is being remembered.
    void storeDeviceProfile();

    // --- default device (requirement N-04) ---
    /// Makes the capture device the system default, recording what it displaced
    /// before touching anything so a kill cannot lose it.
    bool acquireDefaultDevice();
    void releaseDefaultDevice();

    /// Called once at startup. A device left recorded in the settings means the
    /// previous run never gave it back, so the routing is repaired here.
    void repairStrandedDefaultDevice();

    void applyCurrentSettings();
    void updateStatusLabel(const EngineStatus& status);

    /// Puts the switch, the tray entry and the icons into the on or off state
    /// without acting on it. Used both when the user throws the switch and when
    /// the engine stops or restarts on its own.
    void syncPowerUi(bool on);
    void updateSliderLabels();
    void updatePresetActions();
    void persistSettings();

    /// The preset currently selected, with the sliders as the user has left
    /// them rather than as the preset defines them.
    const Preset* currentPreset() const;
    SliderValues currentSliders() const;

    static QIcon makeApplicationIcon(bool active);

    SettingsStore store_;
    AppSettings settings_;
    AudioController controller_;

    /// Built-ins followed by the user's own, which is the order they appear in.
    QVector<Preset> presets_;
    int userPresetStartIndex_ = 0;

    bool loading_ = false;  ///< Suppresses handlers while widgets are populated.
    bool quitting_ = false;

    QListWidget* presetList_ = nullptr;
    QLabel* presetDescription_ = nullptr;
    QPushButton* savePresetButton_ = nullptr;
    QPushButton* deletePresetButton_ = nullptr;

    QSlider* bassSlider_ = nullptr;
    QSlider* claritySlider_ = nullptr;
    QSlider* levelingSlider_ = nullptr;
    QLabel* bassValue_ = nullptr;
    QLabel* clarityValue_ = nullptr;
    QLabel* levelingValue_ = nullptr;

    QSlider* volumeSlider_ = nullptr;
    QLabel* volumeValue_ = nullptr;

    QSlider* balanceSlider_ = nullptr;
    /// Doubles as the readout and the way back to centre, so that the reset is
    /// one click on something already on screen rather than a control of its
    /// own competing for width.
    QPushButton* balanceValue_ = nullptr;

    LevelMeter* inputMeter_ = nullptr;
    LevelMeter* outputMeter_ = nullptr;
    GainReductionMeter* reductionMeter_ = nullptr;
    QLabel* reductionValue_ = nullptr;

    QComboBox* captureCombo_ = nullptr;
    QComboBox* renderCombo_ = nullptr;
    QComboBox* languageCombo_ = nullptr;
    QCheckBox* startWithWindowsCheck_ = nullptr;
    QCheckBox* takeOverCheck_ = nullptr;
    QCheckBox* rememberDeviceCheck_ = nullptr;

    /// The output device whose profile has already been applied. Without it,
    /// every settings change would look like a device change and re-apply the
    /// profile over whatever the user had just adjusted.
    QString appliedProfileDeviceId_;

    QPushButton* powerButton_ = nullptr;
    QPushButton* compareButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* trayPowerAction_ = nullptr;
    QMenu* trayPresetMenu_ = nullptr;
};

}  // namespace audiolens::app
