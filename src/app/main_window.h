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
    void onDeviceChanged();
    void onPowerToggled(bool on);
    void onSaveUserPreset();
    void onDeleteUserPreset();
    void onStartWithWindowsToggled(bool enabled);
    void onTakeOverToggled(bool enabled);

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

    LevelMeter* inputMeter_ = nullptr;
    LevelMeter* outputMeter_ = nullptr;
    GainReductionMeter* reductionMeter_ = nullptr;
    QLabel* reductionValue_ = nullptr;

    QComboBox* captureCombo_ = nullptr;
    QComboBox* renderCombo_ = nullptr;
    QCheckBox* startWithWindowsCheck_ = nullptr;
    QCheckBox* takeOverCheck_ = nullptr;

    QPushButton* powerButton_ = nullptr;
    QPushButton* compareButton_ = nullptr;
    QLabel* statusLabel_ = nullptr;

    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* trayPowerAction_ = nullptr;
    QMenu* trayPresetMenu_ = nullptr;
};

}  // namespace audiolens::app
