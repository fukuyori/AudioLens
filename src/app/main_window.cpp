#include "app/main_window.h"

#include "app/default_device_guard.h"
#include "app/level_meter.h"
#include "common/log.h"
#include "engine/default_device.h"

#include <QAction>
#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QListWidgetItem>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSystemTrayIcon>
#include <QVBoxLayout>

#include <algorithm>

namespace audiolens::app {
namespace {

constexpr const char* kRunKey = "HKEY_CURRENT_USER\\Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr const char* kRunValue = "AudioLens";

QString qs(const std::string& text) { return QString::fromStdString(text); }

QLabel* makeHint(const QString& text) {
    auto* label = new QLabel(text);
    label->setWordWrap(true);
    QFont font = label->font();
    font.setPointSizeF(font.pointSizeF() * 0.9);
    label->setFont(font);
    label->setEnabled(false);  // Renders greyed, which is what a hint should look like.
    return label;
}

QFrame* makeSeparator() {
    auto* line = new QFrame;
    line->setFrameShape(QFrame::HLine);
    line->setFrameShadow(QFrame::Sunken);
    return line;
}

}  // namespace

MainWindow::MainWindow() {
    setWindowTitle(QStringLiteral("AudioLens"));
    setWindowIcon(makeApplicationIcon(false));

    settings_ = store_.loadSettings();

    // Before the window is even built: if the last run was killed while it held
    // the default output, the machine has been silent ever since and fixing
    // that is more urgent than anything on screen.
    repairStrandedDefaultDevice();

    auto* central = new QWidget;
    auto* layout = new QVBoxLayout(central);
    layout->setContentsMargins(16, 16, 16, 12);
    layout->setSpacing(12);

    layout->addWidget(buildPresetSection());
    layout->addWidget(buildSliderSection());
    layout->addWidget(buildMeterSection());
    layout->addWidget(makeSeparator());
    layout->addWidget(buildDeviceSection());
    layout->addStretch(1);
    layout->addWidget(buildFooter());

    setCentralWidget(central);
    buildTrayIcon();

    connect(&controller_, &AudioController::levelsChanged, this,
            [this](float input, float output, float reduction) {
                inputMeter_->setLevel(input);
                outputMeter_->setLevel(output);
                reductionMeter_->setReduction(reduction);
                reductionValue_->setText(reduction < -0.1f
                                             ? QStringLiteral("-%1 dB").arg(-reduction, 0, 'f', 1)
                                             : QStringLiteral("--"));
            });
    connect(&controller_, &AudioController::statusChanged, this, &MainWindow::updateStatusLabel);

    loading_ = true;
    reloadPresets();
    refreshDeviceLists();
    startWithWindowsCheck_->setChecked(settings_.startWithWindows);
    takeOverCheck_->setChecked(settings_.takeOverDefaultDevice);
    selectPresetById(settings_.activePresetId);

    // On a first run there are no stored slider positions, so the preset's own
    // values are what the user should see: they are what the preset name means.
    const SliderValues initial =
        settings_.restored ? settings_.sliders
                           : (currentPreset() != nullptr ? currentPreset()->sliders : SliderValues{});
    bassSlider_->setValue(initial.bass);
    claritySlider_->setValue(initial.clarity);
    levelingSlider_->setValue(initial.leveling);
    loading_ = false;

    updateSliderLabels();
    applyCurrentSettings();
    onDeviceChanged();
    updateStatusLabel(controller_.status());

    // Restore what the app was doing, not merely what it looked like. Starting
    // with Windows is meant to give the user corrected sound from the moment
    // they log in; a tray icon that processes nothing is not that.
    if (settings_.processingEnabled) {
        powerButton_->setChecked(true);  // runs onPowerToggled, which starts it
    }

    resize(480, 700);
}

MainWindow::~MainWindow() {
    // Quitting from the tray menu never goes through closeEvent, so this is the
    // path that catches it. release() is a no-op when the device is not held.
    releaseDefaultDevice();
}

// ---------------------------------------------------------------- layout ---

QWidget* MainWindow::buildPresetSection() {
    auto* group = new QGroupBox(QStringLiteral("プリセット"));
    auto* layout = new QVBoxLayout(group);

    presetList_ = new QListWidget;
    presetList_->setSelectionMode(QAbstractItemView::SingleSelection);
    // Height is set from the row count in reloadPresets(); without it the
    // surrounding layout squeezes the list down to a couple of rows and the
    // preset choice stops looking like a choice.
    connect(presetList_, &QListWidget::currentRowChanged, this,
            &MainWindow::onPresetSelectionChanged);
    // Mouse and keyboard move the current row and the selection together, but
    // accessibility clients (and screen readers) can change only the selection.
    // Fold that back into the current row so both paths pick a preset.
    connect(presetList_, &QListWidget::itemSelectionChanged, this, [this] {
        const QList<QListWidgetItem*> selected = presetList_->selectedItems();
        if (!selected.isEmpty()) {
            const int row = presetList_->row(selected.first());
            if (row != presetList_->currentRow()) {
                presetList_->setCurrentRow(row);
            }
        }
    });
    layout->addWidget(presetList_);

    presetDescription_ = makeHint(QString());
    presetDescription_->setMinimumHeight(32);
    layout->addWidget(presetDescription_);

    auto* buttons = new QHBoxLayout;
    savePresetButton_ = new QPushButton(QStringLiteral("いまの設定を保存..."));
    connect(savePresetButton_, &QPushButton::clicked, this, &MainWindow::onSaveUserPreset);
    deletePresetButton_ = new QPushButton(QStringLiteral("削除"));
    connect(deletePresetButton_, &QPushButton::clicked, this, &MainWindow::onDeleteUserPreset);
    buttons->addWidget(savePresetButton_);
    buttons->addWidget(deletePresetButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    return group;
}

QWidget* MainWindow::buildSliderSection() {
    auto* group = new QGroupBox(QStringLiteral("効果の強さ"));
    auto* grid = new QGridLayout(group);
    grid->setColumnStretch(1, 1);

    struct Row {
        const char* label;
        const char* hint;
        QSlider** slider;
        QLabel** value;
    };
    const Row rows[] = {
        {"低音", "こもりや響きすぎを抑えます", &bassSlider_, &bassValue_},
        {"声の明瞭さ", "人の声を聞き取りやすくします", &claritySlider_, &clarityValue_},
        {"音量差", "大きい音と小さい音の差を揃えます", &levelingSlider_, &levelingValue_},
    };

    int row = 0;
    for (const Row& r : rows) {
        auto* name = new QLabel(QString::fromUtf8(r.label));
        *r.slider = new QSlider(Qt::Horizontal);
        (*r.slider)->setRange(0, 100);
        (*r.slider)->setSingleStep(1);
        (*r.slider)->setPageStep(10);
        connect(*r.slider, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);

        *r.value = new QLabel(QStringLiteral("0"));
        (*r.value)->setMinimumWidth(32);
        (*r.value)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        grid->addWidget(name, row, 0);
        grid->addWidget(*r.slider, row, 1);
        grid->addWidget(*r.value, row, 2);
        grid->addWidget(makeHint(QString::fromUtf8(r.hint)), row + 1, 1, 1, 2);
        row += 2;
    }

    return group;
}

QWidget* MainWindow::buildMeterSection() {
    auto* group = new QGroupBox(QStringLiteral("動作状況"));
    auto* grid = new QGridLayout(group);
    grid->setColumnStretch(1, 1);

    inputMeter_ = new LevelMeter;
    outputMeter_ = new LevelMeter;
    reductionMeter_ = new GainReductionMeter;
    reductionValue_ = new QLabel(QStringLiteral("--"));
    reductionValue_->setMinimumWidth(56);
    reductionValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    grid->addWidget(new QLabel(QStringLiteral("入力")), 0, 0);
    grid->addWidget(inputMeter_, 0, 1, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("出力")), 1, 0);
    grid->addWidget(outputMeter_, 1, 1, 1, 2);
    grid->addWidget(new QLabel(QStringLiteral("音量調整")), 2, 0);
    grid->addWidget(reductionMeter_, 2, 1);
    grid->addWidget(reductionValue_, 2, 2);

    return group;
}

QWidget* MainWindow::buildDeviceSection() {
    auto* group = new QGroupBox(QStringLiteral("音の経路"));
    auto* layout = new QVBoxLayout(group);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    captureCombo_ = new QComboBox;
    renderCombo_ = new QComboBox;
    connect(captureCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceChanged);
    connect(renderCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceChanged);

    grid->addWidget(new QLabel(QStringLiteral("取り込み元")), 0, 0);
    grid->addWidget(captureCombo_, 0, 1);
    grid->addWidget(new QLabel(QStringLiteral("出力先")), 1, 0);
    grid->addWidget(renderCombo_, 1, 1);
    layout->addLayout(grid);

    layout->addWidget(makeHint(QStringLiteral(
        "Windows の既定の出力デバイスを「取り込み元」に指定し、実際に聞くデバイスを"
        "「出力先」に指定します。両方を同じにすると音が回り込むため設定できません。")));

    takeOverCheck_ = new QCheckBox(
        QStringLiteral("動作中は「取り込み元」を Windows の既定の出力にする"));
    takeOverCheck_->setToolTip(QStringLiteral(
        "開始したときに既定の出力を取り込み元へ切り替え、停止したときに元へ戻します。\n"
        "オフにすると、切り替えは手動になります。"));
    connect(takeOverCheck_, &QCheckBox::toggled, this, &MainWindow::onTakeOverToggled);
    layout->addWidget(takeOverCheck_);

    startWithWindowsCheck_ = new QCheckBox(QStringLiteral("Windows の起動時に開始する"));
    connect(startWithWindowsCheck_, &QCheckBox::toggled, this,
            &MainWindow::onStartWithWindowsToggled);
    layout->addWidget(startWithWindowsCheck_);

    return group;
}

QWidget* MainWindow::buildFooter() {
    auto* container = new QWidget;
    auto* layout = new QVBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* buttons = new QHBoxLayout;

    powerButton_ = new QPushButton(QStringLiteral("開始"));
    powerButton_->setCheckable(true);
    powerButton_->setMinimumHeight(38);
    connect(powerButton_, &QPushButton::toggled, this, &MainWindow::onPowerToggled);

    // Press and hold is the natural gesture for an A/B: you hear the difference
    // while the button is down and the processing returns when you let go.
    compareButton_ = new QPushButton(QStringLiteral("押している間だけ処理前の音"));
    compareButton_->setMinimumHeight(38);
    connect(compareButton_, &QPushButton::pressed, this,
            [this] { controller_.setBypass(true); });
    connect(compareButton_, &QPushButton::released, this,
            [this] { controller_.setBypass(false); });

    buttons->addWidget(powerButton_, 1);
    buttons->addWidget(compareButton_, 2);
    layout->addLayout(buttons);

    // The status text and the version share a row: the status is what changes
    // and gets the room, the version sits quietly at the end of it. Putting it
    // on a line of its own would give a number that never changes the same
    // weight as the one thing on this screen that does.
    auto* statusRow = new QHBoxLayout;

    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(true);
    statusRow->addWidget(statusLabel_, 1);

    auto* versionLabel = makeHint(QStringLiteral(AUDIOLENS_VERSION));
    versionLabel->setWordWrap(false);
    versionLabel->setAlignment(Qt::AlignRight | Qt::AlignBottom);
    // Selectable so a version can be copied into a bug report rather than
    // transcribed from the screen.
    versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    versionLabel->setToolTip(QStringLiteral("AudioLens %1").arg(AUDIOLENS_VERSION));
    statusRow->addWidget(versionLabel, 0);

    layout->addLayout(statusRow);

    return container;
}

void MainWindow::buildTrayIcon() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    trayMenu_ = new QMenu(this);

    // The version belongs somewhere a user can find it without a terminal, and
    // the tray menu is the one part of the app that is always reachable.
    QAction* versionAction =
        trayMenu_->addAction(QStringLiteral("AudioLens %1").arg(AUDIOLENS_VERSION));
    versionAction->setEnabled(false);
    trayMenu_->addSeparator();

    trayPowerAction_ = trayMenu_->addAction(QStringLiteral("開始"));
    trayPowerAction_->setCheckable(true);
    connect(trayPowerAction_, &QAction::toggled, this,
            [this](bool on) { powerButton_->setChecked(on); });

    trayPresetMenu_ = trayMenu_->addMenu(QStringLiteral("プリセット"));

    trayMenu_->addSeparator();
    QAction* showAction = trayMenu_->addAction(QStringLiteral("ウィンドウを表示"));
    connect(showAction, &QAction::triggered, this, [this] {
        showNormal();
        raise();
        activateWindow();
    });

    QAction* quitAction = trayMenu_->addAction(QStringLiteral("終了"));
    connect(quitAction, &QAction::triggered, this, [this] {
        quitting_ = true;
        QApplication::quit();
    });

    tray_ = new QSystemTrayIcon(makeApplicationIcon(false), this);
    tray_->setContextMenu(trayMenu_);
    tray_->setToolTip(QStringLiteral("AudioLens"));
    connect(tray_, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (reason == QSystemTrayIcon::Trigger || reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    raise();
                    activateWindow();
                }
            });
    tray_->show();
}

// ------------------------------------------------------------- behaviour ---

void MainWindow::reloadPresets() {
    const bool wasLoading = loading_;
    loading_ = true;

    presets_.clear();
    presetList_->clear();
    if (trayPresetMenu_ != nullptr) {
        trayPresetMenu_->clear();
    }

    for (const Preset& preset : builtinPresets()) {
        presets_.push_back(preset);
    }
    userPresetStartIndex_ = presets_.size();
    for (const Preset& preset : store_.loadUserPresets()) {
        presets_.push_back(preset);
    }

    // Rows are set tighter than the style's default so that every preset fits
    // on screen at once. Scrolling would be the ordinary answer to a list that
    // has outgrown its box, but not for this list: choosing a preset is the
    // main thing this window is for, and a choice you have to scroll to find is
    // one you do not know you have. Four of the ten only appeared on a scroll
    // before this, and they were the four most recently added.
    //
    // The floor keeps the rows a comfortable size to hit with a mouse; padding
    // is what gets taken away, not legibility.
    const int rowHeight = std::max(22, presetList_->fontMetrics().height() + 6);

    for (int i = 0; i < presets_.size(); ++i) {
        QString label = qs(presets_[i].name);
        if (i >= userPresetStartIndex_) {
            label += QStringLiteral("  (自分の設定)");
        }
        auto* item = new QListWidgetItem(label, presetList_);
        item->setSizeHint(QSize(0, rowHeight));

        if (trayPresetMenu_ != nullptr) {
            QAction* action = trayPresetMenu_->addAction(qs(presets_[i].name));
            connect(action, &QAction::triggered, this,
                    [this, i] { presetList_->setCurrentRow(i); });
        }
    }

    // Size to a whole number of rows. A list ending in a half-visible entry
    // reads as a rendering glitch rather than as "scroll for more".
    //
    // The cap only exists so that a user with a great many saved presets does
    // not get a window taller than their screen; the built-in ten always fit.
    if (presetList_->count() > 0) {
        constexpr int kMaxVisibleRows = 14;
        const int visibleRows = std::min(presetList_->count(), kMaxVisibleRows);
        const int frame = 2 * presetList_->frameWidth();
        presetList_->setFixedHeight(rowHeight * visibleRows + frame);
    }

    loading_ = wasLoading;
}

void MainWindow::refreshDeviceLists() {
    const bool wasLoading = loading_;
    loading_ = true;

    captureCombo_->clear();
    renderCombo_->clear();

    for (const DeviceChoice& device : controller_.availableDevices()) {
        const QString label =
            device.isDefault ? device.displayName + QStringLiteral("  [既定]") : device.displayName;
        captureCombo_->addItem(label, device.id);
        renderCombo_->addItem(label, device.id);
    }

    const auto selectOrDefault = [](QComboBox* combo, const QString& id, bool preferDefault) {
        const int index = combo->findData(id);
        if (index >= 0) {
            combo->setCurrentIndex(index);
            return;
        }
        // Falling back to the system default is the useful guess for the
        // capture side; for the output it only avoids an empty box.
        for (int i = 0; i < combo->count(); ++i) {
            if (combo->itemText(i).contains(QStringLiteral("[既定]")) == preferDefault) {
                combo->setCurrentIndex(i);
                return;
            }
        }
    };
    selectOrDefault(captureCombo_, settings_.captureDeviceId, true);
    selectOrDefault(renderCombo_, settings_.renderDeviceId, false);

    loading_ = wasLoading;
}

void MainWindow::selectPresetById(const QString& id) {
    for (int i = 0; i < presets_.size(); ++i) {
        if (qs(presets_[i].id) == id) {
            presetList_->setCurrentRow(i);
            return;
        }
    }
    if (!presets_.isEmpty()) {
        presetList_->setCurrentRow(0);
    }
}

const Preset* MainWindow::currentPreset() const {
    const int row = presetList_->currentRow();
    if (row < 0 || row >= presets_.size()) {
        return nullptr;
    }
    return &presets_[row];
}

SliderValues MainWindow::currentSliders() const {
    return SliderValues{bassSlider_->value(), claritySlider_->value(), levelingSlider_->value()}
        .clamped();
}

void MainWindow::onPresetSelectionChanged() {
    const Preset* preset = currentPreset();
    if (preset == nullptr) {
        return;
    }

    presetDescription_->setText(qs(preset->description));
    updatePresetActions();

    if (!loading_) {
        // Choosing a preset means wanting its sliders; the user can move them
        // afterwards and those positions stick until they choose another.
        loading_ = true;
        bassSlider_->setValue(preset->sliders.bass);
        claritySlider_->setValue(preset->sliders.clarity);
        levelingSlider_->setValue(preset->sliders.leveling);
        loading_ = false;

        updateSliderLabels();
        applyCurrentSettings();
        persistSettings();
    }
}

void MainWindow::onSliderChanged() {
    updateSliderLabels();
    if (loading_) {
        return;
    }
    applyCurrentSettings();
    persistSettings();
}

void MainWindow::onDeviceChanged() {
    if (loading_) {
        return;
    }
    // Changing the capture device while running moves where the audio has to
    // come from, so the default has to move with it. Without this the system
    // would keep feeding the device we no longer listen to, and the user would
    // hear nothing with no indication of why.
    const bool wasHeld = DefaultDeviceGuard::held();
    if (wasHeld) {
        releaseDefaultDevice();
    }

    controller_.setDevices(captureCombo_->currentData().toString(),
                           renderCombo_->currentData().toString());

    if (wasHeld && controller_.running()) {
        acquireDefaultDevice();
    }
    persistSettings();
    updateStatusLabel(controller_.status());
}

void MainWindow::onPowerToggled(bool on) {
    if (on) {
        // Take the device over first: between the switch and the engine
        // starting, audio has nowhere to go, so that gap should be as short as
        // possible. A failure here is not fatal — the user can still route the
        // sound by hand — so it does not stop the engine from starting.
        acquireDefaultDevice();
        if (!controller_.start()) {
            // start() has already published why; drop the button back so the UI
            // is not claiming to run something that is not running.
            releaseDefaultDevice();
            powerButton_->setChecked(false);
            return;
        }
    } else {
        controller_.stop();
        releaseDefaultDevice();
    }

    powerButton_->setText(on ? QStringLiteral("停止") : QStringLiteral("開始"));
    if (trayPowerAction_ != nullptr) {
        QSignalBlocker blocker(trayPowerAction_);
        trayPowerAction_->setChecked(on);
        trayPowerAction_->setText(on ? QStringLiteral("停止") : QStringLiteral("開始"));
    }
    if (tray_ != nullptr) {
        tray_->setIcon(makeApplicationIcon(on));
    }
    setWindowIcon(makeApplicationIcon(on));

    settings_.processingEnabled = on;
    persistSettings();
}

void MainWindow::onSaveUserPreset() {
    const Preset* base = currentPreset();
    if (base == nullptr) {
        return;
    }

    bool accepted = false;
    const QString name = QInputDialog::getText(
        this, QStringLiteral("設定を保存"), QStringLiteral("名前を付けてください:"),
        QLineEdit::Normal, qs(base->name) + QStringLiteral(" (自分用)"), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }

    Preset preset = *base;
    preset.id = SettingsStore::makeUserPresetId(name).toStdString();
    preset.name = name.trimmed().toStdString();
    preset.description = QStringLiteral("%1 をもとに調整した設定です。").arg(qs(base->name)).toStdString();
    preset.sliders = currentSliders();

    QString error;
    if (!store_.saveUserPreset(preset, &error)) {
        QMessageBox::warning(this, QStringLiteral("保存できません"), error);
        return;
    }

    reloadPresets();
    selectPresetById(qs(preset.id));
    persistSettings();
}

void MainWindow::onDeleteUserPreset() {
    const int row = presetList_->currentRow();
    if (row < userPresetStartIndex_ || row >= presets_.size()) {
        return;
    }

    const QString name = qs(presets_[row].name);
    if (QMessageBox::question(this, QStringLiteral("削除の確認"),
                              QStringLiteral("「%1」を削除します。よろしいですか?").arg(name)) !=
        QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!store_.deleteUserPreset(qs(presets_[row].id), &error)) {
        QMessageBox::warning(this, QStringLiteral("削除できません"), error);
        return;
    }

    reloadPresets();
    presetList_->setCurrentRow(0);
}

void MainWindow::onStartWithWindowsToggled(bool enabled) {
    if (loading_) {
        return;
    }

    QSettings run(QString::fromUtf8(kRunKey), QSettings::NativeFormat);
    if (enabled) {
        const QString command =
            QStringLiteral("\"%1\" --minimized")
                .arg(QDir::toNativeSeparators(QApplication::applicationFilePath()));
        run.setValue(QString::fromUtf8(kRunValue), command);
    } else {
        run.remove(QString::fromUtf8(kRunValue));
    }

    settings_.startWithWindows = enabled;
    persistSettings();
}

void MainWindow::onTakeOverToggled(bool enabled) {
    if (loading_) {
        return;
    }
    settings_.takeOverDefaultDevice = enabled;

    // Turning it off mid-run has to give the device back straight away.
    // Leaving it held would mean the setting says one thing and the system
    // does another, and the user has no way to tell.
    if (!enabled) {
        releaseDefaultDevice();
    } else if (controller_.running()) {
        acquireDefaultDevice();
    }
    persistSettings();
}

bool MainWindow::acquireDefaultDevice() {
    if (!settings_.takeOverDefaultDevice || DefaultDeviceGuard::held()) {
        return true;
    }
    const QString captureId = controller_.captureDeviceId();
    const QString previous = DefaultDeviceGuard::currentDefault();
    if (captureId.isEmpty() || captureId == previous) {
        return true;  // nothing to displace
    }

    // Written *before* the switch. If the process dies between these two lines
    // the settings name a device that is still the default, and the repair at
    // next startup is a no-op — which is the harmless direction to fail in.
    settings_.previousDefaultDeviceId = previous;
    store_.saveSettings(settings_);

    QString error;
    if (!DefaultDeviceGuard::acquire(captureId, previous, &error)) {
        settings_.previousDefaultDeviceId.clear();
        store_.saveSettings(settings_);
        QMessageBox::warning(
            this, QStringLiteral("AudioLens"),
            QStringLiteral("既定の出力デバイスを切り替えられませんでした。\n%1\n\n"
                           "Windows のサウンド設定から「%2」を既定の出力にしてください。")
                .arg(error, captureCombo_->currentText()));
        return false;
    }
    return true;
}

void MainWindow::releaseDefaultDevice() {
    if (!DefaultDeviceGuard::held()) {
        return;
    }
    DefaultDeviceGuard::release();
    settings_.previousDefaultDeviceId.clear();
    store_.saveSettings(settings_);
}

void MainWindow::repairStrandedDefaultDevice() {
    const QString stranded = settings_.previousDefaultDeviceId;
    if (stranded.isEmpty()) {
        return;
    }
    // Getting here means the last run was killed while holding the default.
    // Everything on the machine has been going into a cable nobody is reading
    // ever since, so put it back before anything else happens.
    settings_.previousDefaultDeviceId.clear();
    store_.saveSettings(settings_);

    std::string error;
    if (setDefaultRenderDevice(stranded.toStdWString(), &error)) {
        AL_INFO("前回の終了時に戻せなかった既定の出力デバイスを復元しました。");
    } else {
        AL_WARN("既定の出力デバイスを復元できません: {}", error);
    }
}

void MainWindow::applyCurrentSettings() {
    const Preset* preset = currentPreset();
    if (preset != nullptr) {
        controller_.applyPreset(*preset, currentSliders());
    }
}

void MainWindow::updateSliderLabels() {
    bassValue_->setText(QString::number(bassSlider_->value()));
    clarityValue_->setText(QString::number(claritySlider_->value()));
    levelingValue_->setText(QString::number(levelingSlider_->value()));
}

void MainWindow::updatePresetActions() {
    const int row = presetList_->currentRow();
    deletePresetButton_->setEnabled(row >= userPresetStartIndex_ && row < presets_.size());
}

void MainWindow::updateStatusLabel(const EngineStatus& status) {
    if (status.running) {
        // The two are added, not nested. EngineStats::estimatedLatencyMs()
        // covers the buffering between the two endpoints and knows nothing
        // about the chain's look-ahead, so the old wording — "of which
        // processing is N ms" — understated the total by exactly that much.
        const double processing = controller_.dspLatencyMs();
        QString text = QStringLiteral("動作中  遅延 %1 ms")
                           .arg(status.latencyMs + processing, 0, 'f', 1);
        if (processing > 0.0) {
            text += QStringLiteral("(バッファ %1 / 処理 %2)")
                        .arg(status.latencyMs, 0, 'f', 1)
                        .arg(processing, 0, 'f', 1);
        } else {
            text += QStringLiteral("(補正なし)");
        }
        if (status.captureSampleRate != status.renderSampleRate && status.captureSampleRate > 0) {
            text += QStringLiteral("  %1→%2 Hz 変換")
                        .arg(status.captureSampleRate)
                        .arg(status.renderSampleRate);
        }
        if (status.underruns > 0 || status.overruns > 0) {
            text += QStringLiteral("  途切れ %1 回").arg(status.underruns + status.overruns);
        }
        statusLabel_->setText(text);
    } else if (status.recovering) {
        statusLabel_->setText(status.message);
    } else if (!status.message.isEmpty()) {
        statusLabel_->setText(QStringLiteral("停止中 — ") + status.message);
    } else {
        statusLabel_->setText(QStringLiteral("停止中"));
    }

    // While recovering, leave the switch on: the user asked for it to be
    // running, and it is coming back on its own. Flicking it off here would
    // both misrepresent the state and cancel the recovery.
    if (status.recovering) {
        return;
    }

    // The engine can stop for good, for instance when a device is unplugged
    // and nothing usable replaces it.
    if (!status.running && powerButton_->isChecked()) {
        QSignalBlocker blocker(powerButton_);
        powerButton_->setChecked(false);
        powerButton_->setText(QStringLiteral("開始"));
        if (trayPowerAction_ != nullptr) {
            QSignalBlocker trayBlocker(trayPowerAction_);
            trayPowerAction_->setChecked(false);
            trayPowerAction_->setText(QStringLiteral("開始"));
        }
        if (tray_ != nullptr) {
            tray_->setIcon(makeApplicationIcon(false));
        }
    }
}

void MainWindow::persistSettings() {
    if (loading_) {
        return;
    }
    settings_.captureDeviceId = captureCombo_->currentData().toString();
    settings_.renderDeviceId = renderCombo_->currentData().toString();
    settings_.sliders = currentSliders();
    if (const Preset* preset = currentPreset()) {
        settings_.activePresetId = qs(preset->id);
    }
    store_.saveSettings(settings_);
}

void MainWindow::closeEvent(QCloseEvent* event) {
    // Closing the window leaves the app running in the tray, because the whole
    // point is to keep processing while you use something else.
    if (!quitting_ && tray_ != nullptr && tray_->isVisible()) {
        hide();
        event->ignore();
        return;
    }
    controller_.stop();
    releaseDefaultDevice();
    event->accept();
}

QIcon MainWindow::makeApplicationIcon(bool active) {
    // Drawn rather than shipped as a file: one less binary asset to keep in
    // sync, and it can reflect whether processing is running.
    QPixmap pixmap(64, 64);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);

    const QColor accent = active ? QColor(0x3f, 0xa7, 0x5f) : QColor(0x88, 0x8f, 0x96);
    painter.setPen(QPen(accent, 6));
    painter.setBrush(Qt::NoBrush);
    painter.drawEllipse(QRectF(8, 8, 40, 40));

    painter.setPen(QPen(accent, 7, Qt::SolidLine, Qt::RoundCap));
    painter.drawLine(QPointF(44, 44), QPointF(57, 57));

    return QIcon(pixmap);
}

}  // namespace audiolens::app
