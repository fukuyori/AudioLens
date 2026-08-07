#include "app/main_window.h"

#include "app/default_device_guard.h"
#include "app/i18n.h"
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
#include <QTimer>
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
/// A bare signed number would make the user work out which end is which. Words
/// say it outright, but in Japanese those are full-width characters, nearly
/// twice the width of a Latin one, and this readout shares a grid column with
/// the plain numbers above it — so the widest thing it can ever show sets how
/// much room every slider in the group has left. L and R are what the labelling
/// on every piece of audio hardware uses, in a third of the space, and they
/// need no translation.
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
    startMinimizedCheck_->setChecked(settings_.startMinimized);
    takeOverCheck_->setChecked(settings_.takeOverDefaultDevice);
    languageCombo_->setCurrentIndex(
        std::max(0, languageCombo_->findData(languageToString(settings_.language))));
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
    auto* group = new QGroupBox(tr("Presets"));
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
    savePresetButton_ = new QPushButton(tr("Save these settings..."));
    connect(savePresetButton_, &QPushButton::clicked, this, &MainWindow::onSaveUserPreset);
    deletePresetButton_ = new QPushButton(tr("Delete"));
    connect(deletePresetButton_, &QPushButton::clicked, this, &MainWindow::onDeleteUserPreset);
    buttons->addWidget(savePresetButton_);
    buttons->addWidget(deletePresetButton_);
    buttons->addStretch(1);
    layout->addLayout(buttons);

    return group;
}

QWidget* MainWindow::buildSliderSection() {
    auto* group = new QGroupBox(tr("Amount of effect"));
    auto* grid = new QGridLayout(group);
    grid->setColumnStretch(1, 1);

    // No explanatory line under each slider. The names carry it, and three
    // greyed sentences repeated on every screenshot are read once and then
    // become noise that the eye has to step over on the way to the control.
    // The tooltips keep the wording for anyone who wants it.
    //
    // The strings are built here rather than held in the table below: tr()
    // called on a table entry would translate at the point of use, which is
    // correct, but lupdate reads source text and would find nothing to extract.
    struct Row {
        QString label;
        QString tip;
        QSlider** slider;
        QLabel** value;
    };
    const Row rows[] = {
        {tr("Bass"), tr("Reduces boom and excessive resonance"), &bassSlider_, &bassValue_},
        {tr("Speech clarity"), tr("Makes voices easier to follow"), &claritySlider_, &clarityValue_},
        {tr("Loudness range"), tr("Evens out the gap between loud and quiet"), &levelingSlider_,
         &levelingValue_},
    };

    int row = 0;
    for (const Row& r : rows) {
        auto* name = new QLabel(r.label);
        name->setToolTip(r.tip);
        *r.slider = new QSlider(Qt::Horizontal);
        (*r.slider)->setRange(0, 100);
        (*r.slider)->setSingleStep(1);
        (*r.slider)->setPageStep(10);
        (*r.slider)->setToolTip(r.tip);
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

    auto* volumeName = new QLabel(tr("Output volume"));
    volumeSlider_ = new QSlider(Qt::Horizontal);
    volumeSlider_->setRange(0, 100);
    volumeSlider_->setSingleStep(1);
    volumeSlider_->setPageStep(5);
    connect(volumeSlider_, &QSlider::valueChanged, this, &MainWindow::onOutputVolumeChanged);

    volumeValue_ = new QLabel(QStringLiteral("100"));
    volumeValue_->setMinimumWidth(32);
    volumeValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    // No hint under this one. The three above need explaining because "bass"
    // and "loudness range" name an amount of processing rather than a thing; a
    // volume slider explains itself.
    grid->addWidget(volumeName, row, 0);
    grid->addWidget(volumeSlider_, row, 1);
    grid->addWidget(volumeValue_, row, 2);
    ++row;

    auto* balanceName = new QLabel(tr("Balance"));
    balanceSlider_ = new QSlider(Qt::Horizontal);
    balanceSlider_->setRange(-50, 50);
    balanceSlider_->setSingleStep(1);
    balanceSlider_->setPageStep(5);
    balanceSlider_->setToolTip(
        tr("When one ear hears more than the other, turns the louder side down\n"
           "to even them up. Double-click to return to centre."));
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
    balanceValue_->setToolTip(tr("L = left, R = right. Click to return to centre (0)"));
    connect(balanceValue_, &QPushButton::clicked, this,
            [this] { balanceSlider_->setValue(0); });

    grid->addWidget(balanceName, row, 0);
    grid->addWidget(balanceSlider_, row, 1);
    grid->addWidget(balanceValue_, row, 2);

    return group;
}

QWidget* MainWindow::buildMeterSection() {
    auto* group = new QGroupBox(tr("Levels"));
    auto* grid = new QGridLayout(group);
    grid->setColumnStretch(1, 1);

    inputMeter_ = new LevelMeter;
    outputMeter_ = new LevelMeter;
    reductionMeter_ = new GainReductionMeter;
    reductionValue_ = new QLabel(QStringLiteral("--"));
    reductionValue_->setMinimumWidth(56);
    reductionValue_->setAlignment(Qt::AlignRight | Qt::AlignVCenter);

    grid->addWidget(new QLabel(tr("Input")), 0, 0);
    grid->addWidget(inputMeter_, 0, 1, 1, 2);
    grid->addWidget(new QLabel(tr("Output")), 1, 0);
    grid->addWidget(outputMeter_, 1, 1, 1, 2);
    grid->addWidget(new QLabel(tr("Levelling")), 2, 0);
    grid->addWidget(reductionMeter_, 2, 1);
    grid->addWidget(reductionValue_, 2, 2);

    return group;
}

QWidget* MainWindow::buildDeviceSection() {
    auto* group = new QGroupBox(tr("Signal path"));
    auto* layout = new QVBoxLayout(group);

    auto* grid = new QGridLayout;
    grid->setColumnStretch(1, 1);

    captureCombo_ = new QComboBox;
    renderCombo_ = new QComboBox;
    connect(captureCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceChanged);
    connect(renderCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onDeviceChanged);

    grid->addWidget(new QLabel(tr("Capture from")), 0, 0);
    grid->addWidget(captureCombo_, 0, 1);
    grid->addWidget(new QLabel(tr("Play to")), 1, 0);
    grid->addWidget(renderCombo_, 1, 1);
    // The same paragraph that used to sit here in grey, moved to where it is
    // asked for rather than always on screen.
    const QString routingTip =
        tr("Set Windows' default output device as the capture source, and the\n"
           "device you actually listen on as the output. The two cannot be the\n"
           "same, because the sound would loop back on itself.");
    captureCombo_->setToolTip(routingTip);
    renderCombo_->setToolTip(routingTip);

    layout->addLayout(grid);

    rememberDeviceCheck_ = new QCheckBox(tr("Remember these settings for this output"));
    rememberDeviceCheck_->setToolTip(
        tr("Ties the current preset, amounts, volume and balance to this output\n"
           "device. Switching back to it restores them."));
    connect(rememberDeviceCheck_, &QCheckBox::toggled, this,
            &MainWindow::onRememberDeviceToggled);
    layout->addWidget(rememberDeviceCheck_);

    takeOverCheck_ = new QCheckBox(tr("Make the capture source the Windows default while running"));
    takeOverCheck_->setToolTip(
        tr("Switches the default output to the capture source on start and puts\n"
           "it back on stop. Off, the switching is yours to do."));
    connect(takeOverCheck_, &QCheckBox::toggled, this, &MainWindow::onTakeOverToggled);
    layout->addWidget(takeOverCheck_);

    startWithWindowsCheck_ = new QCheckBox(tr("Start with Windows"));
    connect(startWithWindowsCheck_, &QCheckBox::toggled, this,
            &MainWindow::onStartWithWindowsToggled);
    layout->addWidget(startWithWindowsCheck_);

    // Only about launching it yourself. Starting with Windows already passes
    // --minimized, because a window appearing over whatever you were doing at
    // login is not what "start with Windows" is asking for.
    startMinimizedCheck_ = new QCheckBox(tr("Start in the tray, without the window"));
    startMinimizedCheck_->setToolTip(
        tr("Applies when you start AudioLens yourself. Starting with Windows\n"
           "never shows the window. Open it again from the tray icon."));
    connect(startMinimizedCheck_, &QCheckBox::toggled, this,
            &MainWindow::onStartMinimizedToggled);
    layout->addWidget(startMinimizedCheck_);

    // Language sits here rather than in a settings dialog of its own. There is
    // no such dialog, and one control does not justify inventing one; this
    // column already holds the settings that are chosen once and left alone.
    auto* languageRow = new QHBoxLayout;
    languageRow->addWidget(new QLabel(tr("Language")));
    languageCombo_ = new QComboBox;
    // The two named languages are written in themselves, not translated. A
    // reader who has landed in the wrong language needs to recognise their own,
    // and "Japanese" is no help to somebody who cannot read the interface it is
    // written in.
    languageCombo_->addItem(tr("Same as Windows"), languageToString(Language::System));
    languageCombo_->addItem(QStringLiteral("English"), languageToString(Language::English));
    languageCombo_->addItem(QString::fromUtf8("\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E"),
                            languageToString(Language::Japanese));
    connect(languageCombo_, &QComboBox::currentIndexChanged, this, &MainWindow::onLanguageChanged);
    languageRow->addWidget(languageCombo_, 1);
    layout->addLayout(languageRow);

    return group;
}

QWidget* MainWindow::buildFooter() {
    // One row, and the buttons at their natural size.
    //
    // They were 38 px tall and stretched across the whole window, with the A/B
    // button given twice the width of the switch. That is a lot of chrome for
    // two controls that are pressed once a day at most: the app starts with
    // Windows and restores its own state, so the switch is rarely touched, and
    // the A/B comparison is for tuning a preset rather than for listening.
    //
    // Sized to their text and left-aligned, they read as what they are, and the
    // status — the one thing here that changes while you watch — gets the rest
    // of the row instead of a line of its own.
    auto* container = new QWidget;
    auto* layout = new QHBoxLayout(container);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(8);

    powerButton_ = new QPushButton(tr("Start"));
    powerButton_->setCheckable(true);
    connect(powerButton_, &QPushButton::toggled, this, &MainWindow::onPowerToggled);

    // Press and hold is the natural gesture for an A/B: you hear the difference
    // while the button is down and the processing returns when you let go.
    compareButton_ = new QPushButton(tr("Sound before processing"));
    compareButton_->setToolTip(
        tr("Removes the correction while held. The path and the latency do not\n"
           "change, so what you are comparing is the processing alone."));
    connect(compareButton_, &QPushButton::pressed, this,
            [this] { controller_.setBypass(true); });
    connect(compareButton_, &QPushButton::released, this,
            [this] { controller_.setBypass(false); });

    layout->addWidget(powerButton_);
    layout->addWidget(compareButton_);
    layout->addSpacing(8);

    // Word wrap off, unlike before: on a shared row a wrapping label would
    // change the footer's height as the text changed, which makes the whole
    // window twitch every time the latency figure gains a digit.
    statusLabel_ = new QLabel;
    statusLabel_->setWordWrap(false);
    layout->addWidget(statusLabel_, 1);

    auto* versionLabel = makeHint(QStringLiteral(AUDIOLENS_VERSION));
    versionLabel->setWordWrap(false);
    versionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    // Selectable so a version can be copied into a bug report rather than
    // transcribed from the screen.
    versionLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    versionLabel->setToolTip(QStringLiteral("AudioLens %1").arg(AUDIOLENS_VERSION));
    layout->addWidget(versionLabel, 0);

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

    // A command, not a state, and so deliberately not checkable.
    //
    // A checkable entry has to answer two questions in one row: what happens if
    // you click it, and what is true now. Those disagree when the label is a
    // verb. While processing ran, the menu showed a ticked 「停止」 -- which
    // reads as "stopped" to anyone who does not already know the tick means the
    // opposite of the word beside it.
    //
    // The verb is the half worth keeping. Whether processing is running is
    // already in the tray icon's colour, and the window says it in words.
    trayPowerAction_ = trayMenu_->addAction(tr("Start"));
    connect(trayPowerAction_, &QAction::triggered, this,
            [this] { powerButton_->setChecked(!powerButton_->isChecked()); });

    trayPresetMenu_ = trayMenu_->addMenu(tr("Presets"));

    trayMenu_->addSeparator();
    QAction* showAction = trayMenu_->addAction(tr("Show window"));
    connect(showAction, &QAction::triggered, this, &MainWindow::showAndRaise);

    QAction* quitAction = trayMenu_->addAction(tr("Quit"));
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
                    showAndRaise();
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
        QString label = presetName(presets_[i]);
        if (i >= userPresetStartIndex_) {
            label += tr("  (your own)");
        }
        auto* item = new QListWidgetItem(label, presetList_);
        item->setSizeHint(QSize(0, rowHeight));

        if (trayPresetMenu_ != nullptr) {
            QAction* action = trayPresetMenu_->addAction(presetName(presets_[i]));
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

    // Carried alongside the list rather than recovered from the labels. The
    // marker used to be found by searching the item text for "[default]", which
    // worked only for as long as that text never changed — and translating the
    // interface changes it. A flag that the enumeration already provides cannot
    // be broken by rewording anything.
    QString defaultId;
    for (const DeviceChoice& device : controller_.availableDevices()) {
        const QString label =
            device.isDefault ? device.displayName + tr("  [default]") : device.displayName;
        captureCombo_->addItem(label, device.id);
        renderCombo_->addItem(label, device.id);
        if (device.isDefault) {
            defaultId = device.id;
        }
    }

    const auto selectOrDefault = [&defaultId](QComboBox* combo, const QString& id,
                                             bool preferDefault) {
        const int index = combo->findData(id);
        if (index >= 0) {
            combo->setCurrentIndex(index);
            return;
        }
        // Falling back to the system default is the useful guess for the
        // capture side; for the output it only avoids an empty box.
        for (int i = 0; i < combo->count(); ++i) {
            const bool isDefault = !defaultId.isEmpty() && combo->itemData(i) == defaultId;
            if (isDefault == preferDefault) {
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

    presetDescription_->setText(presetDescription(*preset));
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

void MainWindow::onLanguageChanged() {
    if (loading_) {
        return;
    }
    const Language chosen = languageFromString(languageCombo_->currentData().toString());
    if (chosen == settings_.language) {
        return;
    }
    settings_.language = chosen;
    persistSettings();

    // Deliberately not retranslated in place.
    //
    // Qt can do that — a changeEvent handler re-running every setText — but it
    // means every string in the window has to be reachable from one function
    // and kept in step with where it was set. That is a second copy of the
    // whole interface, and the copy is the one that rots: a control added later
    // gets its text in the builder and nobody remembers the other place.
    //
    // Changing the language is something a user does once. Asking them to
    // restart is a fair price for not carrying that duplicate.
    QMessageBox::information(this, QStringLiteral("AudioLens"),
                             tr("The language will change when AudioLens is restarted."));
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

    syncPowerUi(on);

    settings_.processingEnabled = on;
    persistSettings();
}

void MainWindow::onSaveUserPreset() {
    const Preset* base = currentPreset();
    if (base == nullptr) {
        return;
    }

    bool accepted = false;
    // The suggested name and the description are built from the *translated*
    // base name, and are then stored as plain text. A preset the user saved is
    // theirs, and the words in it should stay as they were when they saved it
    // rather than shifting the next time the interface language changes.
    const QString baseName = presetName(*base);
    const QString name =
        QInputDialog::getText(this, tr("Save settings"), tr("Give it a name:"), QLineEdit::Normal,
                              tr("%1 (mine)").arg(baseName), &accepted);
    if (!accepted || name.trimmed().isEmpty()) {
        return;
    }

    Preset preset = *base;
    preset.id = SettingsStore::makeUserPresetId(name).toStdString();
    preset.name = name.trimmed().toStdString();
    preset.description = tr("Adjusted from %1.").arg(baseName).toStdString();
    preset.sliders = currentSliders();

    QString error;
    if (!store_.saveUserPreset(preset, &error)) {
        QMessageBox::warning(this, tr("Cannot save"), error);
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

    const QString name = presetName(presets_[row]);
    if (QMessageBox::question(this, tr("Confirm deletion"),
                              tr("Delete \"%1\"?").arg(name)) != QMessageBox::Yes) {
        return;
    }

    QString error;
    if (!store_.deleteUserPreset(qs(presets_[row].id), &error)) {
        QMessageBox::warning(this, tr("Cannot delete"), error);
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

void MainWindow::onStartMinimizedToggled(bool enabled) {
    if (loading_) {
        return;
    }
    settings_.startMinimized = enabled;
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
        QMessageBox::warning(this, QStringLiteral("AudioLens"),
                             tr("Could not change the default output device.\n%1\n\n"
                                "Set \"%2\" as the default output in Windows sound settings.")
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
            AL_INFO("Restored the default output device the previous run left behind.");
            return;
        }
    }
    AL_WARN("Could not restore the default output device: {}", error);
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
        QString text = tr("Running  latency %1 ms").arg(status.latencyMs + processing, 0, 'f', 1);
        if (processing > 0.0) {
            text += tr("(buffer %1 / processing %2)")
                        .arg(status.latencyMs, 0, 'f', 1)
                        .arg(processing, 0, 'f', 1);
        } else {
            text += tr("(no correction)");
        }
        if (status.captureSampleRate != status.renderSampleRate && status.captureSampleRate > 0) {
            text += tr("  %1 -> %2 Hz conversion")
                        .arg(status.captureSampleRate)
                        .arg(status.renderSampleRate);
        }
        if (status.underruns > 0 || status.overruns > 0) {
            text += tr("  %1 dropout(s)").arg(status.underruns + status.overruns);
        }
        // Shown separately from the dropout count because it is a different
        // fault with a different sound. A dropout is the ring running dry; this
        // is silence deliberately inserted, and when it is inserted wrongly it
        // is heard as a click rather than a gap. Reported in milliseconds
        // because a frame count means nothing to the person hearing it.
        if (status.silenceFillFrames > 0 && status.captureSampleRate > 0) {
            text += tr("  %1 ms silence inserted")
                        .arg(1000.0 * static_cast<double>(status.silenceFillFrames) /
                                 status.captureSampleRate,
                             0, 'f', 0);
        }
        statusLabel_->setText(text);
    } else if (status.recovering) {
        statusLabel_->setText(status.message);
    } else if (!status.message.isEmpty()) {
        statusLabel_->setText(tr("Stopped - ") + status.message);
    } else {
        statusLabel_->setText(tr("Stopped"));
    }

    // While recovering, leave the switch on: the user asked for it to be
    // running, and it is coming back on its own. Flicking it off here would
    // both misrepresent the state and cancel the recovery.
    if (status.recovering) {
        return;
    }

    // Both directions, not just the stopping one. The engine can stop for good
    // — a device unplugged with nothing usable to replace it — and it can also
    // come back on its own long afterwards, once whatever was missing is
    // plugged in again. A switch that only ever moved to "Start" would leave
    // the second case showing the app as stopped while it was in fact running.
    if (status.running != powerButton_->isChecked()) {
        syncPowerUi(status.running);
    }
}

void MainWindow::syncPowerUi(bool on) {
    // Signals blocked throughout: this reflects a decision already taken, and
    // letting it run onPowerToggled would start or stop the engine a second
    // time on the strength of the display catching up.
    QSignalBlocker buttonBlocker(powerButton_);
    powerButton_->setChecked(on);
    powerButton_->setText(on ? tr("Stop") : tr("Start"));

    if (trayPowerAction_ != nullptr) {
        // No signal blocker: setText emits nothing, which is the other half of
        // why the entry is a plain command now.
        trayPowerAction_->setText(on ? tr("Stop") : tr("Start"));
    }
    if (tray_ != nullptr) {
        tray_->setIcon(makeApplicationIcon(on));
    }
    setWindowIcon(makeApplicationIcon(on));
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

// ------------------------------------------------------ command line (F-36) ---

void MainWindow::showAndRaise() {
    showNormal();
    raise();
    activateWindow();
}

int MainWindow::findPresetRow(const QString& name) const {
    for (int i = 0; i < presets_.size(); ++i) {
        if (qs(presets_[i].id).compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    // Then the display name, which is what someone reads off the screen before
    // going to the shell. Second rather than first because ids are unique and
    // names need not be.
    //
    // Matched as translated, not as written in the source. The window shows
    // 「映画」and the source string is "Film"; a user who types what they can
    // see would otherwise be told there is no such preset while looking
    // straight at it. The untranslated form is accepted too, for a script
    // written on one machine and run on another with a different language.
    for (int i = 0; i < presets_.size(); ++i) {
        if (presetName(presets_[i]).compare(name, Qt::CaseInsensitive) == 0 ||
            qs(presets_[i].name).compare(name, Qt::CaseInsensitive) == 0) {
            return i;
        }
    }
    return -1;
}

QString MainWindow::presetListing() const {
    QStringList lines;
    for (int i = 0; i < presets_.size(); ++i) {
        lines << QStringLiteral("%1%2  %3")
                     .arg(qs(presets_[i].id), -16)
                     .arg(i >= userPresetStartIndex_ ? QStringLiteral("*") : QStringLiteral(" "),
                          presetName(presets_[i]));
    }
    lines << QStringLiteral("(* = your own preset)");
    return lines.join(QLatin1Char('\n'));
}

int MainWindow::findDeviceRow(QComboBox* combo, const QString& name, QString* problem) const {
    const std::vector<DeviceChoice> devices = controller_.availableDevices();

    // Exact first. Without it, a device whose whole name is contained in a
    // longer one -- "Speakers" beside "Speakers (USB2.0 Device)" -- could never
    // be selected, because naming it exactly would still match both.
    for (const DeviceChoice& device : devices) {
        if (device.displayName.compare(name, Qt::CaseInsensitive) == 0) {
            return combo->findData(device.id);
        }
    }

    QStringList matched;
    QString onlyId;
    for (const DeviceChoice& device : devices) {
        if (device.displayName.contains(name, Qt::CaseInsensitive)) {
            matched << device.displayName;
            onlyId = device.id;
        }
    }
    if (matched.size() == 1) {
        return combo->findData(onlyId);
    }

    // Refused rather than guessed at. Picking the wrong output device is a
    // mistake nobody sees: it is heard, later, as nothing at all.
    if (matched.isEmpty()) {
        *problem = QStringLiteral("No device matches '%1'. Try --list-outputs.").arg(name);
    } else {
        *problem = QStringLiteral("'%1' matches more than one device:\n  %2\n"
                                  "Give enough of the name to pick just one.")
                       .arg(name, matched.join(QStringLiteral("\n  ")));
    }
    return -1;
}

QString MainWindow::deviceListing() const {
    const QString outputId = renderCombo_->currentData().toString();
    const QString inputId = captureCombo_->currentData().toString();

    QStringList lines;
    for (const DeviceChoice& device : controller_.availableDevices()) {
        QStringList marks;
        if (device.id == outputId) {
            marks << QStringLiteral("output");
        }
        if (device.id == inputId) {
            marks << QStringLiteral("input");
        }
        if (device.isDefault) {
            marks << QStringLiteral("system default");
        }
        lines << (marks.isEmpty()
                      ? device.displayName
                      : QStringLiteral("%1  [%2]").arg(device.displayName,
                                                       marks.join(QStringLiteral(", "))));
    }
    lines << QStringLiteral("(one list serves --output and --input: the capture side taps a "
                            "playback device through loopback)");
    return lines.join(QLatin1Char('\n'));
}

QString MainWindow::statusReport() const {
    const EngineStatus status = controller_.status();
    QStringList lines;

    lines << QStringLiteral("state     %1")
                 .arg(status.running        ? QStringLiteral("running")
                      : status.recovering   ? QStringLiteral("recovering")
                                            : QStringLiteral("stopped"));
    if (!status.running && !status.message.isEmpty()) {
        lines << QStringLiteral("reason    %1").arg(status.message);
    }
    if (const Preset* preset = currentPreset()) {
        lines << QStringLiteral("preset    %1  (%2)").arg(qs(preset->id), presetName(*preset));
    }
    const SliderValues sliders = currentSliders();
    lines << QStringLiteral("amounts   bass %1  clarity %2  leveling %3")
                 .arg(sliders.bass)
                 .arg(sliders.clarity)
                 .arg(sliders.leveling);
    lines << QStringLiteral("volume    %1").arg(volumeSlider_->value());
    lines << QStringLiteral("balance   %1").arg(balanceSlider_->value());
    lines << QStringLiteral("bypass    %1")
                 .arg(controller_.bypassed() ? QStringLiteral("on") : QStringLiteral("off"));
    lines << QStringLiteral("output    %1").arg(renderCombo_->currentText());
    lines << QStringLiteral("input     %1").arg(captureCombo_->currentText());

    if (status.running) {
        // The same two figures the status line shows, added rather than nested:
        // the engine's estimate knows nothing about the chain's look-ahead.
        const double processing = controller_.dspLatencyMs();
        lines << QStringLiteral("latency   %1 ms  (buffer %2 / processing %3)")
                     .arg(status.latencyMs + processing, 0, 'f', 1)
                     .arg(status.latencyMs, 0, 'f', 1)
                     .arg(processing, 0, 'f', 1);
        if (status.captureSampleRate > 0) {
            lines << QStringLiteral("rate      %1 -> %2 Hz")
                         .arg(status.captureSampleRate)
                         .arg(status.renderSampleRate);
        }
        lines << QStringLiteral("dropouts  %1 underrun / %2 overrun")
                     .arg(status.underruns)
                     .arg(status.overruns);
    }
    return lines.join(QLatin1Char('\n'));
}

bool MainWindow::handleControlMessage(const QStringList& arguments, QString* reply) {
    const ControlRequest request = parseControlRequest(arguments);
    if (!request.error.isEmpty()) {
        *reply = request.error + QLatin1Char('\n') + controlUsage();
        return false;
    }
    if (request.help) {
        *reply = controlUsage();
        return true;
    }
    if (request.version) {
        *reply = QStringLiteral("AudioLens " AUDIOLENS_VERSION "\n");
        return true;
    }

    // A second launch with nothing to say is someone opening the app that is
    // already open -- a shortcut, the Start menu, the installer's tick box.
    // Every other tray application answers that by showing its window, and
    // doing nothing at all is indistinguishable from a broken shortcut.
    if (!request.actsOnRunningInstance()) {
        if (!request.minimized) {
            showAndRaise();
        }
        return true;
    }
    return applyControlRequest(request, reply);
}

bool MainWindow::applyControlRequest(const ControlRequest& request, QString* reply) {
    QStringList lines;
    bool ok = true;

    // A fixed order, not the order the flags were typed in.
    //
    // Choosing a preset resets the three amounts to the ones that preset means,
    // so `--bass 20 --preset movie` would throw the 20 away if the flags were
    // honoured left to right. Applying the preset first makes both orderings of
    // the same command line mean the same thing, which is what anyone writing
    // the second one expects.
    //
    // Routing comes before even that, for the same reason one step further out:
    // changing the output device applies whatever was remembered for it -- its
    // preset, amounts, volume and balance (F-13). Anything else on the same
    // command line has to land after that, or the profile would overwrite it.
    const auto route = [&](QComboBox* combo, const std::optional<QString>& name) {
        if (!name) {
            return;
        }
        QString problem;
        const int row = findDeviceRow(combo, *name, &problem);
        if (row < 0) {
            lines << problem;
            ok = false;
            return;
        }
        // Through the widget, so the same handler runs that would have run had
        // the user picked it from the list: the default-output takeover moves
        // with the capture device, and the profile is applied and stored.
        combo->setCurrentIndex(row);
    };
    route(renderCombo_, request.output);
    route(captureCombo_, request.input);

    if (request.preset) {
        const int row = findPresetRow(*request.preset);
        if (row < 0) {
            lines << QStringLiteral("There is no preset called '%1'. Try --list-presets.")
                         .arg(*request.preset);
            ok = false;
        } else {
            // Through the widget rather than the model, here and below. The
            // handler behind it is what redraws the screen, writes the settings
            // file and refreshes the per-device profile (F-13); reaching past it
            // would leave the window showing one thing and the engine doing
            // another, and would silently stop remembering the change.
            presetList_->setCurrentRow(row);
        }
    }

    if (request.bass) {
        bassSlider_->setValue(*request.bass);
    }
    if (request.clarity) {
        claritySlider_->setValue(*request.clarity);
    }
    if (request.leveling) {
        levelingSlider_->setValue(*request.leveling);
    }
    if (request.volume) {
        volumeSlider_->setValue(*request.volume);
    }
    if (request.volumeStep) {
        // QSlider clamps, so a step off either end lands on the end rather than
        // failing. That is what a volume key should do when it is held down.
        volumeSlider_->setValue(volumeSlider_->value() + *request.volumeStep);
    }
    if (request.balance) {
        balanceSlider_->setValue(*request.balance);
    }

    if (request.powerToggle) {
        powerButton_->setChecked(!powerButton_->isChecked());
    }
    if (request.power) {
        powerButton_->setChecked(*request.power);
    }
    if (request.power.value_or(false) && !controller_.running()) {
        // The button drops itself back when the engine refuses to start, so the
        // window already tells the truth. The command line has to be told too,
        // or a script would go on as though the sound were being processed.
        const QString why = controller_.status().message;
        lines << (why.isEmpty() ? QStringLiteral("Could not start processing.")
                                : QStringLiteral("Could not start processing: %1").arg(why));
        ok = false;
    }

    if (request.bypass) {
        // Not written to the settings, and gone at the next launch. The A/B
        // button on screen is press-and-hold for the same reason: bypass is
        // something you do while listening, not a mode to leave switched on by
        // accident and then report as the app not working.
        controller_.setBypass(*request.bypass);
    }

    if (request.show) {
        showAndRaise();
    }
    if (request.hide) {
        hide();
    }

    if (request.listPresets) {
        lines << presetListing();
    }
    if (request.listOutputs) {
        lines << deviceListing();
    }
    if (request.status) {
        lines << statusReport();
    }

    if (request.quit) {
        lines << QStringLiteral("Quitting.");
        quitting_ = true;
        // Queued, so the reply is written and flushed before the event loop
        // unwinds. Quitting inside the handler would drop the answer, and would
        // race the restore of the default output device -- which is the one
        // thing that must not be skipped on the way out (requirement N-04).
        QTimer::singleShot(0, qApp, &QApplication::quit);
    }

    if (reply != nullptr) {
        *reply = lines.join(QLatin1Char('\n'));
        if (!reply->isEmpty() && !reply->endsWith(QLatin1Char('\n'))) {
            reply->append(QLatin1Char('\n'));
        }
    }
    return ok;
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
