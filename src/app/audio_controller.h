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

    void applyPreset(const Preset& preset, const SliderValues& sliders);

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

    /// Picks a replacement for `preferredId` when that device is gone: the
    /// system default, if it is not the other end of the loop.
    QString resolveUsableDevice(const QString& preferredId, const QString& avoidId) const;

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

    /// Set when the engine stopped on its own and the user had it switched on,
    /// meaning it should come back without being asked.
    bool recovering_ = false;
    int pollsUntilRetry_ = 0;
    int recoveryAttempts_ = 0;

    /// What the user asked for, as opposed to what is currently in use. A
    /// device that comes back should be returned to.
    QString desiredCaptureId_;
    QString desiredRenderId_;
};

}  // namespace audiolens::app
