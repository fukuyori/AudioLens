#include "app/audio_controller.h"

#include "common/com.h"
#include "common/log.h"

namespace audiolens::app {
namespace {

/// Meters and counters are read this often. Fast enough that a level meter
/// looks continuous, slow enough to stay invisible in a CPU profile.
constexpr int kPollIntervalMs = 50;

/// The status line carries latency and glitch counts, which drift while the
/// engine runs. Republishing it only on start and stop would leave it showing
/// whatever those numbers happened to be at the instant of starting, i.e. zero.
/// Twice a second is enough for a figure a user reads rather than watches.
constexpr int kStatusEveryNPolls = 10;

/// Wait between recovery attempts. Windows takes a moment to settle after a
/// device appears or disappears, and retrying into that produces a second
/// failure rather than a faster recovery.
constexpr int kRecoveryDelayPolls = 12;  // ~600 ms

/// After this many failures the device is treated as genuinely gone and the
/// user is told, instead of the app retrying silently forever.
constexpr int kMaxRecoveryAttempts = 10;

/// How long the engine has to survive before a later fault counts as a new
/// problem rather than a continuation of the one being recovered from. Without
/// this, a device that accepts a connection and then drops it would reset the
/// retry budget on every cycle and the app would never conclude anything.
constexpr int kHealthyPolls = 100;  // ~5 s

QString toQString(const std::wstring& text) {
    return QString::fromWCharArray(text.c_str(), static_cast<int>(text.size()));
}

std::wstring toWide(const QString& text) { return text.toStdWString(); }

}  // namespace

AudioController::AudioController(QObject* parent) : QObject(parent) {
    engine_.setProcessor(&chain_);

    pollTimer_.setInterval(kPollIntervalMs);
    connect(&pollTimer_, &QTimer::timeout, this, &AudioController::poll);
    pollTimer_.start();

    // The notification arrives on an MMDevice thread. Queue it so the handler
    // runs on the GUI thread, where touching the engine is safe.
    std::string watchError;
    if (!watcher_.start([this](DeviceChange, const std::wstring&) {
            QMetaObject::invokeMethod(this, [this] { onDeviceChanged(); }, Qt::QueuedConnection);
        },
        &watchError)) {
        // Not fatal: without the watcher, recovery falls back to the retry
        // timer noticing the engine has stopped.
        qWarning("デバイス変更の監視を開始できません: %s", watchError.c_str());
    }
}

AudioController::~AudioController() {
    pollTimer_.stop();
    watcher_.stop();
    engine_.stop();
}

std::vector<DeviceChoice> AudioController::availableDevices() const {
    std::vector<DeviceChoice> result;
    for (const DeviceInfo& info : enumerateDevices(DeviceDirection::Render)) {
        DeviceChoice choice;
        choice.id = toQString(info.id);
        choice.displayName = QString::fromStdString(info.friendlyName);
        choice.isDefault = info.isDefault;
        result.push_back(std::move(choice));
    }
    return result;
}

void AudioController::setDevices(const QString& captureId, const QString& renderId) {
    if (captureId == captureId_ && renderId == renderId_) {
        return;
    }
    captureId_ = captureId;
    renderId_ = renderId;
    desiredCaptureId_ = captureId;
    desiredRenderId_ = renderId;

    // Restarting is the only way to change endpoints, so do it transparently
    // rather than making the user toggle the switch themselves.
    if (running()) {
        stop();
        start();
    }
}

QString AudioController::resolveUsableDevice(const QString& preferredId,
                                             const QString& avoidId) const {
    const std::vector<DeviceChoice> devices = availableDevices();

    // The device the user chose, if it is back.
    for (const DeviceChoice& device : devices) {
        if (device.id == preferredId) {
            return preferredId;
        }
    }

    // Otherwise the system default, which is where Windows has moved playback.
    for (const DeviceChoice& device : devices) {
        if (device.isDefault && device.id != avoidId) {
            return device.id;
        }
    }

    // Otherwise anything that is not the other end of the loop.
    for (const DeviceChoice& device : devices) {
        if (device.id != avoidId) {
            return device.id;
        }
    }
    return {};
}

bool AudioController::start() {
    if (running()) {
        return true;
    }

    if (captureId_.isEmpty() || renderId_.isEmpty()) {
        lastError_ = QStringLiteral("取り込み元と出力先を選んでください。");
        emit statusChanged(status());
        return false;
    }
    if (captureId_ == renderId_) {
        lastError_ = QStringLiteral(
            "取り込み元と出力先が同じデバイスです。音が回り込むため、別のデバイスを選んでください。");
        emit statusChanged(status());
        return false;
    }

    EngineConfig config;
    config.captureDeviceId = toWide(captureId_);
    config.renderDeviceId = toWide(renderId_);
    config.captureLoopback = true;

    std::string error;
    if (!engine_.start(config, &error)) {
        lastError_ = QString::fromStdString(error);
        emit statusChanged(status());
        return false;
    }

    lastError_.clear();
    wasRunning_ = true;
    recovering_ = false;
    recoveryAttempts_ = 0;
    pollsSinceStart_ = 0;
    if (desiredCaptureId_.isEmpty()) {
        desiredCaptureId_ = captureId_;
        desiredRenderId_ = renderId_;
    }
    emit statusChanged(status());
    return true;
}

void AudioController::stop() {
    // Clearing this first matters: a deliberate stop must not look like a
    // device failure and get undone by the recovery path.
    recovering_ = false;
    recoveryAttempts_ = 0;

    if (!running()) {
        return;
    }
    engine_.stop();
    wasRunning_ = false;
    emit statusChanged(status());
}

void AudioController::onDeviceChanged() {
    if (!recovering_) {
        return;
    }
    // Something moved; try again now rather than waiting out the timer.
    pollsUntilRetry_ = 2;
}

bool AudioController::attemptRecovery() {
    const QString capture = resolveUsableDevice(desiredCaptureId_, desiredRenderId_);
    const QString render = resolveUsableDevice(desiredRenderId_, capture);

    if (capture.isEmpty() || render.isEmpty() || capture == render) {
        return false;
    }

    captureId_ = capture;
    renderId_ = render;

    EngineConfig config;
    config.captureDeviceId = toWide(captureId_);
    config.renderDeviceId = toWide(renderId_);
    config.captureLoopback = true;

    std::string error;
    if (!engine_.start(config, &error)) {
        lastError_ = QString::fromStdString(error);
        return false;
    }

    lastError_.clear();
    wasRunning_ = true;
    recovering_ = false;
    pollsSinceStart_ = 0;
    AL_INFO("再接続しました ({} 回目): 取り込み {} / 出力 {}", recoveryAttempts_ + 1,
            captureId_.toStdString(), renderId_.toStdString());
    recoveryAttempts_ = 0;
    return true;
}

void AudioController::applyPreset(const Preset& preset, const SliderValues& sliders,
                                  int outputVolume) {
    dsp::DspParameters parameters = resolveParameters(preset, sliders);
    // Added after the passthrough decision, and safely so: the master only ever
    // attenuates, and an attenuation needs no limiter behind it. That is what
    // lets the volume work on a preset that applies nothing without costing it
    // the zero latency that makes it a passthrough.
    parameters.outputGainDb += outputVolumeToDb(outputVolume);
    chain_.setParameters(parameters);
}

void AudioController::setBypass(bool bypass) { chain_.setBypass(bypass); }

EngineStatus AudioController::status() const {
    EngineStatus s;
    s.running = engine_.running();

    const EngineStats stats = engine_.stats();
    s.latencyMs = stats.estimatedLatencyMs();
    s.underruns = stats.underruns;
    s.overruns = stats.overruns;
    s.captureSampleRate = stats.captureSampleRate;
    s.renderSampleRate = stats.renderSampleRate;
    s.recovering = recovering_;

    if (!s.running) {
        s.message = recovering_
                        ? QStringLiteral("音声デバイスが変わりました。接続し直しています...")
                        : lastError_;
    }
    return s;
}

void AudioController::poll() {
    const dsp::LevelSnapshot levels = chain_.levels();
    emit levelsChanged(levels.inputPeak, levels.outputPeak, levels.gainReductionDb);

    // An audio thread can abort on its own, most often because a device was
    // unplugged or the default output moved. Requirement N-03 says the engine
    // must follow that rather than falling silent, so this begins a recovery
    // instead of just reporting the failure.
    const std::string fault = engine_.faultReason();
    if (!fault.empty()) {
        // Logged, not just shown: this is the event a user reports as "the
        // sound suddenly stopped", and by the time they say so the status line
        // has moved on.
        AL_WARN("エンジンが停止しました: {} — 再接続を試みます", fault);
        lastError_ = QString::fromStdString(fault);
        engine_.stop();  // also consumes the fault, so this branch runs once
        wasRunning_ = false;
        recovering_ = true;
        if (pollsSinceStart_ >= kHealthyPolls) {
            recoveryAttempts_ = 0;
        }
        pollsSinceStart_ = 0;
        pollsUntilRetry_ = kRecoveryDelayPolls;
        emit statusChanged(status());
        return;
    }

    if (recovering_) {
        if (--pollsUntilRetry_ > 0) {
            return;
        }
        pollsUntilRetry_ = kRecoveryDelayPolls;

        if (attemptRecovery()) {
            emit statusChanged(status());
            return;
        }

        if (++recoveryAttempts_ >= kMaxRecoveryAttempts) {
            AL_WARN("再接続を {} 回試みましたが、使用できるデバイスが見つかりません。",
                    kMaxRecoveryAttempts);
            recovering_ = false;
            lastError_ = QStringLiteral("使用できる音声デバイスが見つかりません。"
                                        "デバイスを選び直してから開始してください。");
            emit statusChanged(status());
        }
        return;
    }

    if (engine_.running()) {
        ++pollsSinceStart_;
    }

    if (engine_.running() != wasRunning_) {
        wasRunning_ = engine_.running();
        pollsSinceStatus_ = 0;
        emit statusChanged(status());
        return;
    }

    if (engine_.running() && ++pollsSinceStatus_ >= kStatusEveryNPolls) {
        pollsSinceStatus_ = 0;
        emit statusChanged(status());
    }
}

}  // namespace audiolens::app
