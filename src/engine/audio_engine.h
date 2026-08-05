#pragma once

#include "dsp/resampler.h"
#include "engine/ring_buffer.h"
#include "engine/wasapi_capture.h"
#include "engine/wasapi_render.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace audiolens {

/// Hook where the DSP chain is installed. M1 runs without one (passthrough);
/// M2 supplies the real implementation.
///
/// `process` is called on the capture thread. It must not allocate, lock, or
/// call into COM.
class IAudioProcessor {
public:
    virtual ~IAudioProcessor() = default;

    /// Called once before the audio threads start, from the caller's thread.
    virtual void prepare(std::uint32_t sampleRate, std::uint32_t channels,
                         std::uint32_t maxFramesPerBlock) = 0;

    /// Processes interleaved float32 audio in place.
    virtual void process(float* audio, std::size_t frames, std::uint32_t channels) noexcept = 0;
};

struct EngineConfig {
    std::wstring captureDeviceId;
    std::wstring renderDeviceId;

    /// True when the capture endpoint is a render device being tapped via
    /// loopback, which is how system audio is captured.
    bool captureLoopback = true;

    /// Requested WASAPI buffer for each endpoint.
    std::uint32_t bufferMs = 10;

    /// Capacity of the ring between the capture and render threads. It absorbs
    /// scheduling jitter, so it also sets a floor on added latency (half the
    /// capacity is the steady-state fill).
    std::uint32_t ringMs = 40;

    std::uint32_t internalChannels = 2;
};

struct EngineStats {
    std::uint32_t sampleRate = 0;
    std::uint64_t capturedFrames = 0;
    std::uint64_t renderedFrames = 0;
    std::uint64_t underruns = 0;        ///< Render callbacks that found the ring dry.
    std::uint64_t overruns = 0;         ///< Capture packets that found the ring full.
    std::uint64_t discontinuities = 0;  ///< WASAPI reported a capture gap.
    std::uint64_t silenceFills = 0;     ///< Idle-loopback silence pushed into the ring.

    /// The two endpoints' rates. They no longer have to match.
    std::uint32_t captureSampleRate = 0;
    std::uint32_t renderSampleRate = 0;

    /// How far the resampler's ratio is trimmed from nominal, in parts per
    /// million. Once settled this *is* the measured difference between the two
    /// endpoints' clocks.
    double driftPpm = 0.0;

    double ringFillMs = 0.0;

    /// How close the ring has come to each end since the engine started.
    ///
    /// The instantaneous fill is sampled by whoever asks, which misses exactly
    /// the moments that matter: the ring is only ever a problem at its
    /// extremes, and those are brief. These are the high-water marks, and the
    /// distance from them to 0 and to the ring's capacity is the margin the
    /// configuration is actually running on.
    double ringFillMinMs = 0.0;
    double ringFillMaxMs = 0.0;
    double ringCapacityMs = 0.0;
    double captureLatencyMs = 0.0;
    double renderLatencyMs = 0.0;
    double renderPaddingMs = 0.0;

    /// Best available estimate of end-to-end added latency.
    double estimatedLatencyMs() const {
        return captureLatencyMs + ringFillMs + renderPaddingMs + renderLatencyMs;
    }
};

/// Capture -> (DSP) -> render pipeline across two WASAPI endpoints.
///
/// The two endpoints may run at different sample rates, and always run on
/// independent clocks, so the ring between them would slowly drain or overflow.
/// A resampler on the render side handles both: it converts between the rates,
/// and the render thread trims its ratio by a few parts per million to hold the
/// ring at its target fill. That replaces an earlier scheme of dropping or
/// duplicating whole frames at the ring, which capped drift but put a
/// discontinuity into the signal each time it acted.
class AudioEngine {
public:
    AudioEngine() = default;
    ~AudioEngine();

    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    /// Must be called before start().
    void setProcessor(IAudioProcessor* processor) { processor_ = processor; }

    bool start(const EngineConfig& config, std::string* error);
    void stop();

    bool running() const noexcept { return running_.load(std::memory_order_acquire); }

    /// Set when an audio thread aborted, e.g. because the device was removed.
    /// Empty while the engine is healthy.
    ///
    /// Cleared by both start() and stop(), so a fault is reported until it is
    /// acted on and not after. Read it *before* stopping if the reason is
    /// wanted.
    std::string faultReason() const;

    EngineStats stats() const;

private:
    void captureLoop();
    void renderLoop();
    void reportFault(std::string reason);

    EngineConfig config_;
    WasapiCapture capture_;
    WasapiRender render_;
    std::unique_ptr<RingBuffer> ring_;
    IAudioProcessor* processor_ = nullptr;

    /// Converts the capture rate to the render rate, and absorbs the drift
    /// between the two endpoints' clocks by trimming its ratio. Always present,
    /// even when the rates match, so drift handling has one implementation
    /// rather than two.
    std::unique_ptr<dsp::Resampler> resampler_;

    /// Internal rate: the capture endpoint's. Filters are designed against it,
    /// so no resampling happens before the DSP chain.
    std::uint32_t sampleRate_ = 0;
    std::uint32_t renderSampleRate_ = 0;
    std::uint32_t channels_ = 2;
    std::size_t targetFillFrames_ = 0;
    std::size_t capturePeriodFrames_ = 0;

    // Derived from the buffer sizes WASAPI actually granted, which routinely
    // differ from EngineConfig::bufferMs.
    DWORD captureWaitMs_ = 20;
    DWORD renderWaitMs_ = 20;

    /// Consecutive empty capture wakes that mean the tapped endpoint really has
    /// gone quiet. Derived from `captureWaitMs_` so that it stays a fixed amount
    /// of *time* however the timeout is tuned.
    int captureIdleWakes_ = 2;

    std::vector<float> captureScratch_;
    std::vector<float> renderScratch_;
    /// Holds capture-rate frames on their way from the ring into the resampler.
    std::vector<float> resamplerInput_;

    /// Low-passed ring fill, in frames, driving the drift correction. Smoothing
    /// it keeps ordinary jitter from modulating the ratio.
    double smoothedFill_ = 0.0;

    /// High-water marks, owned by the render thread and published for reading.
    std::size_t ringFillMin_ = 0;
    std::size_t ringFillMax_ = 0;
    bool ringReachedTarget_ = false;

    std::thread captureThread_;
    std::thread renderThread_;
    std::atomic<bool> stopRequested_{false};
    std::atomic<bool> running_{false};

    mutable std::mutex faultMutex_;
    std::string fault_;

    // Written by the audio threads, read by the control thread.
    std::atomic<std::uint64_t> capturedFrames_{0};
    std::atomic<std::uint64_t> renderedFrames_{0};
    std::atomic<std::uint64_t> underruns_{0};
    std::atomic<std::uint64_t> overruns_{0};
    std::atomic<std::uint64_t> discontinuities_{0};
    std::atomic<std::uint64_t> silenceFills_{0};
    std::atomic<double> driftPpm_{0.0};

    std::atomic<std::uint32_t> lastRenderPadding_{0};
    std::atomic<std::uint32_t> ringFillMinFrames_{0};
    std::atomic<std::uint32_t> ringFillMaxFrames_{0};
    std::atomic<std::uint32_t> ringCapacityFrames_{0};
};

}  // namespace audiolens
