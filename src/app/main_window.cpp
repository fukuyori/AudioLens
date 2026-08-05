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
#include <QEvent>
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
#include <initializer_list>

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

/// The balance slider's readout: L30 / 0 / R30.
///
/// A bare signed number would make the user work out which end is which. "左"
/// and "右" say it outright, but a full-width character is nearly twice the
/// width of a Latin one, and this readout shares a grid column with the plain
/// numbers above it — so the widest thing it can ever show sets how much room
/// every slider in the group has left. L and R are what the labelling on every
/// piece of audio hardware uses, in a third of the space.
QString balanceText(int balance) {
    if (balance == 0) {
        return QStringLiteral("0");
    }
    return balance < 0 ? QStringLiteral("L%1").arg(-balance)
                       : QStringLiteral("R%1").arg(balance);
}

/// One column of the main window. The trailing stretch is what keeps the three
/// columns independent: without it the shortest column's group box would be
/// pulled tall to match the tallest one, and a half-empty box of stretched
/// controls reads as a layout mistake.
QVBoxLayout* makeColumn(std::initializer_list<QWidget*> sections) {
    auto* column = new QVBoxLayout;
    column->setSpacing(12);
    for (QWidget* section : sections) {
        column->addWidget(section);
    }
    column->addStretch(1);
    return column;
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

    // Three columns rather than one tall stack: what you pick (preset), what you
    // adjust and what it is doing (amounts + meters), and where the sound goes
    // (routing). Stacked vertically these came to ~1000 px and the lower half
    // was off screen on a laptop; side by side each column is short enough to
    // read at a glance, and the meters sit next to the sliders that move them.
    //
    // The group boxes already draw the boundaries, so the separator that used to
    // divide the routing from the rest is gone — a column edge says the same
    // thing.
    auto* columns = new QHBoxLayout;
    columns->setSpacing(14);

    columns->addLayout(makeColumn({buildPresetSection()}), 1);
    columns->addLayout(makeColumn({buildSliderSection(), buildMeterSection()}), 1);
    columns->addLayout(makeColumn({buildDeviceSection()}), 1);

    layout->addLayout(columns, 1);
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
    volumeSlider_->setValue(settings_.outputVolume);
    balanceSlider_->setValue(settings_.balance);
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
    onOutputVolumeChanged();
    onBalanceChanged();
    applyCurrentSettings();
    onDeviceChanged();
    updateStatusLabel(controller_.status());

    // Restore what the app was doing, not merely what it looked like. Starting
    // with Windows is meant to give the user corrected sound from the moment
    // they log in; a tray icon that processes nothing is not that.
    if (settings_.processingEnabled) {
        powerButton_->setChecked(true);  // runs onPowerToggled, which starts it
    }

    // Wide and short is the shape three columns want. The height is the tallest
    // column (the preset list, which is sized to whole rows) plus the footer;
    // anything more is empty space under all three at once.
    resize(1020, 425);
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

    // No explanatory line under each slider. The names carry it, and three
    // greyed sentences repeated on every screenshot are read once and then
    // become noise that the eye has to step over on the way to the control.
    // The tooltips keep the wording for anyone who wants it.
    struct Row {
        const char* label;
        const char* tip;
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
        name->setToolTip(QString::fromUtf8(r.tip));
        *r.slider = new QSlider(Qt::Horizontal);
        (*r.slider)->setRange(0, 100);
        (*r.slider)->setSingleStep(1);
        (*r.slider)->setPageStep(10);
        (*r.slider)->setToolTip(QString::fromUtf8(r.tip));
        connect(*r.slider, &QSlider::valueChanged, this, &MainWindow::onSliderChanged);

        *r.value = new QLabel(QStringLiteral("0"));
        (*r.value)->setMinimumWidth(32);
        (*r.value)->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

        grid->addWidget(name, row, 0);
        grid->addWidget(*r.slider, row, 1);
        grid->addWidget(*r.value, row, 2);
        ++row;
    }

    // Volume sits with the other sliders rather than in a box of its own. It is
    // not an "amount of effect", but it is the same gesture reached for in the
    // same place, and a group of its own cost enough height to squeeze the rest
    // of the window.
    grid->addWidget(makeSeparator(), row, 0, 1, 3);
    ++row;

    auto* volumeName = new QLabel(QStringLiteral("出力音量"));
    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setSingleStep(1);
    volumeSlider_->setPageStep(5);
    connect(volumeSlider_, &QSlider::valueChanged, this, &MainWindow::onOutputVolumeChanged);

    volumeValue_ = new QLabel(QStringLiteral("100"));
    volumeValue_->setMinimumWidth(32);
    volumeValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // No hint under this one. The three above need explaining because "低音"
    // and "音量差" describe an amount of processing rather than a thing; a
    // volume slider explains itself.
    grid->addWidget(volumeName, row, 0);
    grid->addWidget(volumeSlider_, row, 1);
    grid->addWidget(volumeValue_, row, 2);
    ++row;

    auto* balanceName = new QLabel(QStringLiteral("左右"));
    balanceSlider_ = new QSlider(Qt::Horizontal);
    balanceSlider_->setRange(-50, 50);
    balanceSlider_->setSingleStep(1);
    balanceSlider_->setPageStep(5);
    balanceSlider_->setToolTip(QStringLiteral(
        "左右の聞こえ方が違うときに、大きく聞こえる側を下げて釣り合わせます。\n"
        "ダブルクリックで中央に戻ります。"));
    balanceSlider_->installEventFilter(this);
    connect(balanceSlider_, &QSlider::valueChanged, this, &MainWindow::onBalanceChanged);

    // The readout is also the reset. A control that can be moved off a
    // meaningful default needs one click back to it, and the value being shown
    // is the obvious thing to click: it is already where the eye goes to find
    // out where the balance is.
    //
    // Flat, so it reads as the same kind of readout as the numbers above it
    // until the pointer is over it — at which point the hover highlight says it
    // does something, which those numbers do not.
    balanceValue_ = new QPushButton(balanceText(0));
    balanceValue_->setFlat(true);
    // Drawn as text, not as a box. A native button reserves padding meant for
    // one carrying a job title, and four characters in that much chrome made
    // the value column wide enough to take 27 px off every slider in the group
    // — and a fixed width narrow enough to fix that clipped the text instead.
    //
    // Losing the frame settles both: the widget is now exactly as wide as the
    // numbers above it and reads as one of them, and the underline on hover is
    // what says it can be clicked. It stays a QPushButton so that it is still a
    // button to the keyboard and to accessibility tools, which a QLabel with a
    // mouse handler bolted on would not be.
    balanceValue_->setStyleSheet(QStringLiteral(
        "QPushButton { border: none; background: transparent; padding: 0px;"
        "              text-align: right; }"
        "QPushButton:hover { text-decoration: underline; }"));
    // The same floor as the plain readouts above, so this row no longer widens
    // the shared column and the sliders keep the width instead.
    balanceValue_->setMinimumWidth(32);
    balanceValue_->setCursor(Qt::PointingHandCursor);
    balanceValue_->setFocusPolicy(Qt::NoFocus);
    balanceValue_->setToolTip(QStringLiteral("L = 左、R = 右。クリックで中央(0)に戻します"));
    connect(balanceValue_, &QPushButton::clicked, this,
            [this] { balanceSlider_->setValue(0); });

    grid->addWidget(balanceName, row, 0);
    grid->addWidget(balanceSlider_, row, 1);
    grid->addWidget(balanceValue_, row, 2);

    return group;
}

QWidget* MainWindow::buildMeterSection() {
    auto* group = new QGroupBox(QStringLiteral("ボリューム"));
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
    // The same paragraph that used to sit here in grey, moved to where it is
    // asked for rather than always on screen.
    const QString routingTip = QStringLiteral(
        "Windows の既定の出力デバイスを「取り込み元」に指定し、実際に聞くデバイスを\n"
        "「出力先」に指定します。両方を同じにすると音が回り込むため設定できません。");
    captureCombo_->setToolTip(routingTip);
    renderCombo_->setToolTip(routingTip);

    layout->addLayout(grid);

    rememberDeviceCheck_ = new QCheckBox(
        QStringLiteral("この出力先の設定として覚える"));
    rememberDeviceCheck_->setToolTip(QStringLiteral(
        "いまのプリセット・効果の強さ・出力音量を、この出力先に結び付けて覚えます。\n"
        "次にこの出力先へ切り替えたとき、自動で戻ります。"));
    connect(rememberDeviceCheck_, &QCheckBox::toggled, this,
            &MainWindow::onRememberDeviceToggled);
    layout->addWidget(rememberDeviceCheck_);

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

void MainWindow::onRememberDeviceToggled(bool enabled) {
    if (loading_) {
        return;
    }
    const QString renderId = renderCombo_->currentData().toString();
    if (renderId.isEmpty()) {
        return;
    }
    if (enabled) {
        storeDeviceProfile();
    } else {
        settings_.deviceProfiles.remove(renderId);
    }
    persistSettings();
}

void MainWindow::storeDeviceProfile() {
    if (rememberDeviceCheck_ == nullptr || !rememberDeviceCheck_->isChecked()) {
        return;
    }
    const QString renderId = renderCombo_->currentData().toString();
    if (renderId.isEmpty()) {
        return;
    }
    DeviceProfile profile;
    if (const Preset* preset = currentPreset()) {
        profile.presetId = qs(preset->id);
    }
    if (profile.presetId.isEmpty()) {
        return;
    }
    profile.sliders = currentSliders();
    profile.outputVolume = volumeSlider_->value();
    profile.balance = balanceSlider_->value();
    settings_.deviceProfiles.insert(renderId, profile);
}

void MainWindow::applyDeviceProfile(const QString& renderDeviceId) {
    const auto it = settings_.deviceProfiles.constFind(renderDeviceId);

    // The checkbox reflects whether this device is remembered, so it has to be
    // updated whether or not there was anything to apply.
    const bool wasLoading = loading_;
    loading_ = true;
    rememberDeviceCheck_->setChecked(it != settings_.deviceProfiles.constEnd());

    if (it != settings_.deviceProfiles.constEnd()) {
        selectPresetById(it->presetId);
        bassSlider_->setValue(it->sliders.bass);
        claritySlider_->setValue(it->sliders.clarity);
        levelingSlider_->setValue(it->sliders.leveling);
        volumeSlider_->setValue(it->outputVolume);
        balanceSlider_->setValue(it->balance);
    }
    loading_ = wasLoading;

    if (it != settings_.deviceProfiles.constEnd()) {
        settings_.outputVolume = volumeSlider_->value();
        settings_.balance = balanceSlider_->value();
        updateSliderLabels();
        volumeValue_->setText(QString::number(volumeSlider_->value()));
        balanceValue_->setText(balanceText(balanceSlider_->value()));
        applyCurrentSettings();
    }
}

void MainWindow::onOutputVolumeChanged() {
    volumeValue_->setText(QString::number(volumeSlider_->value()));
    if (loading_) {
        return;
    }
    settings_.outputVolume = volumeSlider_->value();
    applyCurrentSettings();
    persistSettings();
}

bool MainWindow::eventFilter(QObject* watched, QEvent* event) {
    // Double click on the slider is the gesture anyone who has used a mixer
    // reaches for first. It is invisible, which is why it is the second way in
    // and not the only one — the readout button is the discoverable one.
    if (watched == balanceSlider_ && event->type() == QEvent::MouseButtonDblClick) {
        balanceSlider_->setValue(0);
        return true;
    }
    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::onBalanceChanged() {
    // A detent at the centre. Landing exactly on 0 with a mouse is otherwise
    // fiddly, and the values being swallowed are worth 0.2 dB at most, which is
    // well under what anyone can hear. Calling setValue re-enters this function
    // with a value of 0, which then takes the ordinary path.
    constexpr int kCentreDetent = 2;
    const int raw = balanceSlider_->value();
    if (raw != 0 && std::abs(raw) <= kCentreDetent) {
        balanceSlider_->setValue(0);
        return;
    }

    balanceValue_->setText(balanceText(raw));
    if (loading_) {
        return;
    }
    settings_.balance = raw;
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

    const QString renderId = renderCombo_->currentData().toString();
    // Only on an actual change of output device. Every settings change also
    // lands here, and re-applying the profile then would undo the adjustment
    // the user had just made.
    if (renderId != appliedProfileDeviceId_) {
        appliedProfileDeviceId_ = renderId;
        applyDeviceProfile(renderId);
    }

    controller_.setDevices(captureCombo_->currentData().toString(), renderId);

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
    if (captureId.isEmpty()) {
        return true;  // nothing to route
    }
    const QString previous = DefaultDeviceGuard::currentDefault();

    // Written *before* the switch. If the process dies between these two lines
    // the settings name a device that is still the default, and the repair at
    // next startup is a no-op — which is the harmless direction to fail in.
    settings_.previousDefaultDeviceId = previous;
    store_.saveSettings(settings_);

    QString error;
    if (!DefaultDeviceGuard::acquire(captureId, controller_.renderDeviceId(), previous, &error)) {
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

    // Same order as the clean exit: the output device first, the displaced one
    // only if that fails. Both are already in the settings file, so this needs
    // nothing extra recorded — and the reason for the order is stronger here
    // than anywhere else, because a run that was killed is exactly the run
    // whose displaced device might have been the cable itself.
    std::string error;
    for (const QString& candidate : {settings_.renderDeviceId, stranded}) {
        if (candidate.isEmpty()) {
            continue;
        }
        if (setDefaultRenderDevice(candidate.toStdWString(), &error)) {
            AL_INFO("前回の終了時に戻せなかった既定の出力デバイスを復元しました。");
            return;
        }
    }
    AL_WARN("既定の出力デバイスを復元できません: {}", error);
}

void MainWindow::applyCurrentSettings() {
    const Preset* preset = currentPreset();
    if (preset != nullptr) {
        controller_.applyPreset(*preset, currentSliders(), settings_.outputVolume,
                                settings_.balance);
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
    settings_.outputVolume = volumeSlider_->value();
    settings_.balance = balanceSlider_->value();
    // A remembered device follows whatever the user does next, so the profile is
    // refreshed alongside the ordinary settings rather than only when the
    // checkbox is ticked.
    storeDeviceProfile();
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
