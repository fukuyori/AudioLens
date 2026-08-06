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

/// Attempts made at the quick cadence above before backing off.
constexpr int kFastRecoveryAttempts = 10;  // ~6 s

/// The slower cadence used after that.
///
/// Six seconds of retrying covers a device being unplugged and plugged back in,
/// and nothing longer. It does not cover a machine resuming from sleep: USB
/// audio can take the better part of a minute to re-enumerate, and against a
/// six-second budget the app gave up while the hardware was still coming back —
/// then sat stopped for the rest of the day.
constexpr int kSlowRecoveryDelayPolls = 100;  // ~5 s

/// After this many failures the device is treated as genuinely gone and the
/// user is told, instead of the app retrying silently forever.
///
/// Ten fast attempts then twenty-four slow ones is about two minutes, which is
/// longer than any resume observed and still short enough that a device which
/// really is gone does not leave the status line lying for an afternoon.
constexpr int kMaxRecoveryAttempts = 34;

/// How long the engine has to survive before a later fault counts as a new
/// problem rather than a continuation of the one being recovered from. Without
/// this, a device that accepts a connection and then drops it would reset the
/// retry budget on every cycle and the app would never conclude anything.
constexpr int kHealthyPolls = 100;  // ~5 s

/// Polls of quiet before a burst of dropouts is written to the log. A resume
/// produces them by the dozen over a second or so, and one line per poll would
/// bury the one fact worth having under fifty copies of itself.
constexpr int kDropoutSettlePolls = 20;  // ~1 s

/// How often the engine writes a line saying it is still well.
///
/// Everything else in the log is an event: something went wrong and here is the
/// note. That leaves a long run with nothing written at all, and no way to tell
/// "ran for eight hours without trouble" from "died after ten minutes and
/// nobody noticed". A heartbeat carrying the figures that drift — latency and
/// the high-water marks — turns an empty log into evidence.
///
/// Ten minutes gives 48 lines across an eight-hour run: enough to see a trend,
/// few enough to read.
constexpr int kHealthEveryNPolls = 12000;  // ~10 min

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
        qWarning("Could not start watching for device changes: %s", watchError.c_str());
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

QString AudioController::resolveUsableDevice(const QString& preferredId) const {
    // The device the user chose, and nothing else.
    //
    // This used to fall back: first to the system default, then to whatever
    // else was in the list. Measured on a real unplug, that put the output on
    // an HDMI monitor — an endpoint the listener cannot hear, chosen silently,
    // with the app reporting a successful reconnection. Substituting the
    // capture side is worse still: it taps a virtual cable that everything else
    // is playing into, and any other cable has nothing playing into it at all.
    //
    // The retry budget and the re-arm on device change are what make waiting
    // the better answer. A device that comes back is reconnected to within a
    // second; a device that does not is reported, and the user picks another
    // one deliberately. Neither outcome is a guess the user has to discover by
    // hearing nothing.
    for (const DeviceChoice& device : availableDevices()) {
        if (device.id == preferredId) {
            return preferredId;
        }
    }
    return {};
}

bool AudioController::start() {
    if (running()) {
        return true;
    }

    if (captureId_.isEmpty() || renderId_.isEmpty()) {
        lastError_ = tr("Choose a capture source and an output.");
        emit statusChanged(status());
        return false;
    }
    if (captureId_ == renderId_) {
        lastError_ = tr("The capture source and the output are the same device. "
                        "The sound would loop back, so choose a different one.");
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
    userWantsRunning_ = true;
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
    // Clearing these first matters: a deliberate stop must not look like a
    // device failure and get undone by the recovery path, nor leave the
    // watcher armed to restart something the user has just switched off.
    userWantsRunning_ = false;
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
    if (recovering_) {
        // Something moved; try again now rather than waiting out the timer.
        pollsUntilRetry_ = 2;
        return;
    }

    // Recovery had been given up on — but a device has just appeared or changed
    // state, and that is new information the decision to give up was made
    // without. Arm again.
    //
    // Without this, giving up was permanent: the retry budget ran out, the
    // watcher's callback returned at the guard above, and the app stayed
    // stopped no matter what was plugged in afterwards. The budget exists to
    // stop the app retrying *silently and forever*, not to make a verdict that
    // outlives the evidence for it.
    //
    // This cannot spin: it is driven by device notifications from the system,
    // not by a timer, and a device that connects and immediately drops is
    // caught by the kHealthyPolls rule below instead.
    if (userWantsRunning_ && !running()) {
        AL_INFO("A device changed; arming reconnection again.");
        recovering_ = true;
        recoveryAttempts_ = 0;
        pollsUntilRetry_ = 2;
        emit statusChanged(status());
    }
}

bool AudioController::attemptRecovery() {
    const QString capture = resolveUsableDevice(desiredCaptureId_);
    const QString render = resolveUsableDevice(desiredRenderId_);

    // Empty means the device the user chose is still missing, so there is
    // nothing to reconnect to yet. The equality check is kept as a belt: the
    // two ids can only match if the user managed to choose the same device for
    // both, which start() rejects, but a silent audio loop is bad enough to be
    // worth two comparisons.
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
    AL_INFO("Reconnected (attempt {}): capture {} / render {}", recoveryAttempts_ + 1,
            captureId_.toStdString(), renderId_.toStdString());
    recoveryAttempts_ = 0;
    return true;
}

void AudioController::applyPreset(const Preset& preset, const SliderValues& sliders,
                                  int outputVolume, int balance) {
    dsp::DspParameters parameters = resolveParameters(preset, sliders);
    // Both are added after the passthrough decision, and safely so: each only
    // ever attenuates, and an attenuation needs no limiter behind it. That is
    // what lets the volume and the balance work on a preset that applies
    // nothing without costing it the zero latency that makes it a passthrough.
    parameters.outputGainDb += outputVolumeToDb(outputVolume);
    parameters.balance = balanceToOffset(balance);
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
    s.silenceFillFrames = stats.silenceFills;
    s.captureSampleRate = stats.captureSampleRate;
    s.renderSampleRate = stats.renderSampleRate;
    s.recovering = recovering_;

    if (!s.running) {
        s.message = recovering_ ? tr("An audio device changed. Reconnecting...") : lastError_;
    }
    return s;
}

void AudioController::reportDropouts() {
    const EngineStats stats = engine_.stats();
    const quint64 under = stats.underruns;
    const quint64 over = stats.overruns;

    // A counter that went *backwards* means the engine restarted and began
    // again from zero — a recovery, not a dropout. Rebasing rather than
    // subtracting matters: these are unsigned, so the difference would wrap to
    // something astronomical and get logged as a catastrophe that never
    // happened.
    const quint64 resyncs = stats.resyncs;

    if (under < loggedUnderruns_ || over < loggedOverruns_ || resyncs < loggedResyncs_) {
        loggedUnderruns_ = under;
        loggedOverruns_ = over;
        loggedResyncs_ = resyncs;
        dropoutQuietPolls_ = 0;
        return;
    }

    if (under == loggedUnderruns_ && over == loggedOverruns_ && resyncs == loggedResyncs_) {
        dropoutQuietPolls_ = 0;
        return;
    }

    // Held back until the burst has stopped, then logged once. A resume
    // produces dropouts by the dozen over a second or so; a line per poll would
    // bury the one fact worth having under fifty copies of itself.
    if (++dropoutQuietPolls_ < kDropoutSettlePolls) {
        return;
    }

    // Which way it broke is the whole point of recording this. The status line
    // adds the two together, so a burst of dropouts says only that something
    // went wrong — not whether the ring ran dry or overflowed, which call for
    // opposite fixes. The fill at the time says how far from target it ended.
    AL_WARN(
        "dropouts: underrun {} (+{}) / overrun {} (+{}, {} gap(s)) / "
        "resync {} (+{}, dropped {:.0f} ms) / "
        "discontinuities {} / ring {:.1f} ms (min {:.1f} / max {:.1f} / capacity {:.1f})",
        static_cast<unsigned long long>(under),
        static_cast<unsigned long long>(under - loggedUnderruns_),
        static_cast<unsigned long long>(over),
        static_cast<unsigned long long>(over - loggedOverruns_),
        static_cast<unsigned long long>(stats.overrunBursts),
        static_cast<unsigned long long>(resyncs),
        static_cast<unsigned long long>(resyncs - loggedResyncs_),
        stats.sampleRate > 0 ? 1000.0 * static_cast<double>(stats.resyncDroppedFrames) /
                                   stats.sampleRate
                             : 0.0,
        static_cast<unsigned long long>(stats.discontinuities), stats.ringFillMs,
        stats.ringFillMinMs, stats.ringFillMaxMs, stats.ringCapacityMs);

    loggedUnderruns_ = under;
    loggedOverruns_ = over;
    loggedResyncs_ = resyncs;
    dropoutQuietPolls_ = 0;
}

void AudioController::reportHealth() {
    const EngineStats stats = engine_.stats();
    // The total, the same figure the status line shows, because the acceptance
    // criterion is stated on end-to-end latency and 100 ms is not far above
    // what has been measured. A run that stays under it needs to be able to
    // prove so afterwards.
    const double total = stats.estimatedLatencyMs() + chain_.latencyMs();

    // The extremes of this interval, not of the whole run. The run-long marks
    // are in the final report; here they would be a constant, because the
    // largest excursion of an eight-hour night happened in its first half hour
    // and every line after it repeated that one number.
    double windowMin = stats.ringFillMs;
    double windowMax = stats.ringFillMs;
    engine_.takeRingFillWindow(&windowMin, &windowMax);

    AL_INFO("health: latency {:.1f} ms (buffer {:.1f} / processing {:.1f}) / "
            "ring {:.1f} ms (min {:.1f} / max {:.1f} / capacity {:.1f}) / "
            "underrun {} / overrun {} ({} gaps) / resync {} / silence {} / drift {:+.0f} ppm",
            total, stats.estimatedLatencyMs(), chain_.latencyMs(), stats.ringFillMs,
            stats.ringFillMinMs, stats.ringFillMaxMs, stats.ringCapacityMs,
            static_cast<unsigned long long>(stats.underruns),
            static_cast<unsigned long long>(stats.overruns),
            static_cast<unsigned long long>(stats.overrunBursts),
            static_cast<unsigned long long>(stats.resyncs),
            static_cast<unsigned long long>(stats.silenceFills), stats.driftPpm);
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
        AL_WARN("The engine stopped: {} - attempting to reconnect", fault);
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
        pollsUntilRetry_ = recoveryAttempts_ < kFastRecoveryAttempts ? kRecoveryDelayPolls
                                                                    : kSlowRecoveryDelayPolls;

        if (attemptRecovery()) {
            emit statusChanged(status());
            return;
        }

        if (++recoveryAttempts_ >= kMaxRecoveryAttempts) {
            AL_WARN("Gave up after {} reconnection attempts; no usable device.",
                    kMaxRecoveryAttempts);
            recovering_ = false;
            lastError_ = tr("No usable audio device was found. "
                            "Choose the devices again and start.");
            emit statusChanged(status());
        }
        return;
    }

    if (engine_.running()) {
        ++pollsSinceStart_;
        reportDropouts();
        if (++pollsSinceHealth_ >= kHealthEveryNPolls) {
            pollsSinceHealth_ = 0;
            reportHealth();
        }
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
