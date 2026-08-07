#pragma once

#include "core/preset.h"
#include "dsp/dsp_chain.h"
#include "engine/audio_engine.h"
#include "engine/device_manager.h"
#include "engine/device_watcher.h"

#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>
#include <vector>

namespace audiolens::app {

struct DeviceChoice {
    QString id;           ///< Empty means "whatever the system default is".
    QString displayName;
    bool isDefault = false;
};

struct EngineStatus {
    bool running = false;
    QString message;         ///< Why it is not running, when it is not.
    double latencyMs = 0.0;
    quint64 underruns = 0;
    quint64 overruns = 0;

    /// Frames of silence the engine pushed into the ring because the tapped
    /// endpoint had gone quiet. Surfaced because a bug that made this fire in
    /// the *middle* of audio — audible as a click every period — was invisible
    /// for as long as the counter was: the run showed zero underruns and zero
    /// overruns while splicing 76 ms of silence into every 30 seconds of sound.
    quint64 silenceFillFrames = 0;

    /// True while the controller is trying to get back on its feet after a
    /// device disappeared. The UI shows this rather than an error, because
    /// nothing is wrong that the user has to act on.
    bool recovering = false;
    quint32 captureSampleRate = 0;
    quint32 renderSampleRate = 0;
};

/// Owns the audio engine and the DSP chain, and presents them to the UI as
/// something with signals rather than something to poll.
///
/// Everything here runs on the GUI thread. The engine spawns its own audio
/// threads; this class only ever starts, stops and configures them, and reads
/// their atomically published counters on a timer.
class AudioController : public QObject {
    Q_OBJECT

public:
    explicit AudioController(QObject* parent = nullptr);
    ~AudioController() override;

    /// Render endpoints, which is what both lists offer: the capture side taps
    /// a render device via loopback.
    std::vector<DeviceChoice> availableDevices() const;

    void setDevices(const QString& captureId, const QString& renderId);
    QString captureDeviceId() const { return captureId_; }
    QString renderDeviceId() const { return renderId_; }

    bool start();
    void stop();
    bool running() const { return engine_.running(); }

    /// `outputVolume` is the master level, 0-100 (requirement F-22). It is
    /// folded in here rather than baked into the preset: the preset says how the
    /// sound should be shaped, the master says how loud the result should be,
    /// and changing preset should not change the volume.
    /// `balance` is the left/right trim, -50 to +50 (requirement F-24), and is
    /// outside the preset for the same reason the volume is.
    void applyPreset(const Preset& preset, const SliderValues& sliders, int outputVolume,
                     int balance);


    /// Writes one line to the log when the dropout counters have moved and then
    /// settled. Separating underruns from overruns is the point: the status
    /// line adds them together, and the two call for opposite fixes.
    void reportDropouts();

    /// Writes a periodic line saying the engine is still well, with the figures
    /// that drift. Without it a healthy long run leaves no record at all, and
    /// "eight hours without trouble" is indistinguishable from "stopped early".
    void reportHealth();

    /// Bypasses the processing without changing the signal path or its latency,
    /// so a listener comparing the two is hearing only the processing.
    void setBypass(bool bypass);
    bool bypassed() const { return chain_.bypassed(); }

    EngineStatus status() const;
    dsp::LevelSnapshot levels() const { return chain_.levels(); }
    double dspLatencyMs() const { return chain_.latencyMs(); }

signals:
    void statusChanged(const EngineStatus& status);
    void levelsChanged(float inputPeak, float outputPeak, float gainReductionDb);

private:
    void poll();

    /// Called from an MMDevice notification thread; hands the event to the GUI
    /// thread rather than acting on it directly.
    void onDeviceChanged();

    /// Tries to restart after a device went away. Returns true once running.
    bool attemptRecovery();

    /// Returns `preferredId` if that device is currently present, and nothing
    /// otherwise. Deliberately does not substitute: see the note on the
    /// definition for what substituting did on a real unplug.
    QString resolveUsableDevice(const QString& preferredId) const;

    dsp::DspChain chain_;
    AudioEngine engine_;
    DeviceWatcher watcher_;
    QTimer pollTimer_;

    QString captureId_;
    QString renderId_;
    QString lastError_;
    bool wasRunning_ = false;
    int pollsSinceStatus_ = 0;

    /// How long the engine has been up since it last started. A fault that
    /// arrives seconds after a successful start is the same problem still
    /// happening, not a fresh one, so the retry budget must not be handed back.
    int pollsSinceStart_ = 0;

    /// Whether the user has the switch on, as opposed to whether audio is
    /// currently flowing. Distinct from `recovering_`, which is only true while
    /// an attempt is in progress: this stays true across a failed recovery, and
    /// is what says a device appearing later is worth acting on.
    bool userWantsRunning_ = false;

    /// Set when the engine stopped on its own and the user had it switched on,
    /// meaning it should come back without being asked.
    bool recovering_ = false;
    int pollsUntilRetry_ = 0;
    int recoveryAttempts_ = 0;

    /// What the user asked for, as opposed to what is currently in use. A
    /// device that comes back should be returned to.
    QString desiredCaptureId_;
    QString desiredRenderId_;

    /// Dropout counts as of the last line written to the log, so that only the
    /// change is reported and a quiet run stays quiet.
    quint64 loggedUnderruns_ = 0;
    quint64 loggedOverruns_ = 0;
    quint64 loggedResyncs_ = 0;
    int dropoutQuietPolls_ = 0;
    int pollsSinceHealth_ = 0;
};

}  // namespace audiolens::app
