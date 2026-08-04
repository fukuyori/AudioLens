#include "engine/audio_engine.h"

#include "common/com.h"
#include "common/denormals.h"
#include "common/log.h"
#include "engine/audio_format.h"

#include <avrt.h>

#include <algorithm>
#include <cstring>
#include <format>

namespace audiolens {
namespace {

/// Joins the "Pro Audio" MMCSS task for the lifetime of the object so the audio
/// threads are scheduled ahead of ordinary work.
class MmcssScope {
public:
    explicit MmcssScope(const wchar_t* taskName) {
        DWORD taskIndex = 0;
        handle_ = ::AvSetMmThreadCharacteristicsW(taskName, &taskIndex);
        if (handle_ == nullptr) {
            AL_WARN("MMCSS への参加に失敗しました(優先度は既定のまま続行)");
        }
    }
    ~MmcssScope() {
        if (handle_ != nullptr) {
            ::AvRevertMmThreadCharacteristics(handle_);
        }
    }
    MmcssScope(const MmcssScope&) = delete;
    MmcssScope& operator=(const MmcssScope&) = delete;

private:
    HANDLE handle_ = nullptr;
};

std::size_t msToFrames(std::uint32_t ms, std::uint32_t sampleRate) {
    return static_cast<std::size_t>(static_cast<std::uint64_t>(ms) * sampleRate / 1000);
}

// The drift the loop has to cancel is tiny and essentially constant: measured
// at under 18 ppm between two USB endpoints. The loop is therefore tuned to be
// slow and gentle rather than responsive. An earlier, faster tuning oscillated
// — its own corrections pushed the fill past the target, which it then
// corrected the other way, swinging the ratio over ±1500 ppm on hardware whose
// real difference was two orders of magnitude smaller.

/// One-pole coefficient applied to the ring fill each render callback, giving
/// an average about fifteen seconds long.
///
/// It was a third of that, which was long enough for two real endpoints and far
/// too short for a virtual cable. A cable is fed by whatever application is
/// playing, not by a crystal, so its delivery is uneven in a way no soundcard's
/// is: the fill wanders some twenty-five milliseconds either side of target
/// with nothing wrong at all. Averaged over four seconds that wander reaches
/// the loop nearly intact.
constexpr double kFillSmoothing = 0.0015;

/// Proportional gain from relative fill error to ratio trim.
///
/// Proportional alone, and that is enough. It settles with a standing fill
/// error in proportion to the drift it is cancelling: at the 126 ppm measured
/// through a virtual cable that is 13 % of the target, about five milliseconds
/// of a ring with twenty-two to spare on each side.
///
/// Lowered from 0.005, which turned the cable's ordinary delivery wander into
/// a trim swing of ±2800 ppm — past the ceiling below, so the loop spent its
/// time saturated. At this gain the same wander asks for ±570 ppm and the
/// ceiling stops being part of the picture. Trading five milliseconds of
/// standing fill error for that is not a close call: the ring has the room, and
/// a ratio that swings is a pitch that swings.
///
/// An integral term was added here and then removed. It was added to cancel a
/// standing error believed to be 23 %, from a clock difference read as
/// 1155 ppm; that reading came from a run in which overruns were discarding
/// captured frames and idle top-ups were adding others, so the frame counts it
/// was derived from were measuring the fault rather than the clocks. With the
/// ring sized properly the same counts give 126 ppm.
///
/// It also did real harm. Its zero landed at 0.09 rad/s against a loop
/// crossover of 0.10 rad/s, which cut the phase margin from 65° to roughly 25°
/// and set the whole thing oscillating across most of the ring.
constexpr double kDriftGain = 0.001;

/// Trim ceiling. Sixteen times the 126 ppm measured through a virtual cable,
/// so it only engages while recovering from a genuine disruption.
constexpr double kMaxTrim = 0.002;  // ±2000 ppm

}  // namespace

AudioEngine::~AudioEngine() { stop(); }

bool AudioEngine::start(const EngineConfig& config, std::string* error) {
    const auto fail = [error](std::string message) {
        if (error != nullptr) {
            *error = std::move(message);
        }
        return false;
    };

    if (running()) {
        return fail("エンジンは既に動作中です");
    }

    config_ = config;
    stopRequested_.store(false, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        fault_.clear();
    }

    if (!capture_.open(config_.captureDeviceId, config_.captureLoopback, config_.bufferMs, error)) {
        return false;
    }
    if (!render_.open(config_.renderDeviceId, config_.bufferMs, error)) {
        capture_.close();
        return false;
    }

    // The two endpoints are free to run at different rates; the resampler on
    // the render side reconciles them. Processing happens at the capture rate,
    // so the DSP chain sees the audio exactly as it arrived.
    sampleRate_ = capture_.format().sampleRate;
    renderSampleRate_ = render_.format().sampleRate;
    channels_ = std::max<std::uint32_t>(1, config_.internalChannels);

    // Four capture periods, not two.
    //
    // Two is the smallest ring that can hold a packet at all, and it leaves no
    // room for anything else: with the target at half, a single arriving packet
    // fills it to the brim and a single render period empties it. Endpoints do
    // not deliver on a metronome — a virtual cable least of all, since it is
    // driven by whatever application is feeding it — so the fill swings by a
    // whole period either way as a matter of course, and at two periods every
    // one of those swings is an overrun or a gap.
    //
    // Four gives a period of slack above and below the target, which is what
    // the ring is for. The cost is latency, and it is bounded: the target sits
    // at half the ring, so this is two capture periods of delay rather than
    // one.
    const std::size_t ringFrames = std::max<std::size_t>(msToFrames(config_.ringMs, sampleRate_),
                                                         capture_.bufferFrames() * 4);
    ring_ = std::make_unique<RingBuffer>(ringFrames, channels_);
    targetFillFrames_ = ringFrames / 2;
    capturePeriodFrames_ = capture_.bufferFrames();
    smoothedFill_ = static_cast<double>(targetFillFrames_);

    resampler_ = std::make_unique<dsp::Resampler>();
    resampler_->prepare(channels_,
                        static_cast<double>(renderSampleRate_) / static_cast<double>(sampleRate_),
                        render_.bufferFrames());

    captureScratch_.assign(capture_.bufferFrames() * channels_, 0.0f);
    renderScratch_.assign(static_cast<std::size_t>(render_.bufferFrames()) * channels_, 0.0f);
    // One render buffer's worth of output can never need more input than the
    // ring holds, but size generously: this is allocated once, off the audio
    // thread, and running short here would mean an underrun every callback.
    resamplerInput_.assign(ringFrames * channels_, 0.0f);

    // Windows commonly grants a larger buffer than requested, so the wait
    // timeouts follow the granted sizes. The capture timeout doubles as the
    // top-up cadence while a loopback endpoint is idle and delivers no events
    // at all: it has to fire faster than the render side drains the ring, or
    // every idle moment turns into a stream of underruns.
    const double capturePeriodMs = 1000.0 * capture_.bufferFrames() / sampleRate_;
    const double renderPeriodMs = 1000.0 * render_.bufferFrames() / sampleRate_;
    captureWaitMs_ = std::max<DWORD>(1, static_cast<DWORD>(capturePeriodMs / 2.0));
    renderWaitMs_ = std::max<DWORD>(20, static_cast<DWORD>(renderPeriodMs * 4.0));

    if (processor_ != nullptr) {
        processor_->prepare(sampleRate_, channels_, capture_.bufferFrames());
    }

    if (!capture_.start(error)) {
        capture_.close();
        render_.close();
        return false;
    }
    if (!render_.start(error)) {
        capture_.close();
        render_.close();
        return false;
    }

    running_.store(true, std::memory_order_release);
    captureThread_ = std::thread(&AudioEngine::captureLoop, this);
    renderThread_ = std::thread(&AudioEngine::renderLoop, this);

    AL_INFO("エンジン開始: {} Hz / 内部 {} ch / リング {} フレーム ({:.1f} ms, 目標充填 {:.1f} ms)",
            sampleRate_, channels_, ringFrames, 1000.0 * ringFrames / sampleRate_,
            1000.0 * targetFillFrames_ / sampleRate_);
    return true;
}

void AudioEngine::stop() {
    if (!running() && !captureThread_.joinable() && !renderThread_.joinable()) {
        return;
    }

    stopRequested_.store(true, std::memory_order_release);

    // Nudge both threads out of their waits so they observe the stop request
    // without having to time out first.
    if (capture_.eventHandle() != nullptr) {
        ::SetEvent(capture_.eventHandle());
    }
    if (render_.eventHandle() != nullptr) {
        ::SetEvent(render_.eventHandle());
    }

    if (captureThread_.joinable()) {
        captureThread_.join();
    }
    if (renderThread_.joinable()) {
        renderThread_.join();
    }

    running_.store(false, std::memory_order_release);
    capture_.close();
    render_.close();

    // A fault is consumed by stopping. Leaving it set made the engine look
    // permanently broken to anyone polling faultReason(): the caller would see
    // the same fault on every tick, restart its handling from scratch each
    // time, and so never get as far as reconnecting. Callers that want the
    // reason must read it before they stop.
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        fault_.clear();
    }
    ring_.reset();
    resampler_.reset();
}

std::string AudioEngine::faultReason() const {
    std::lock_guard<std::mutex> lock(faultMutex_);
    return fault_;
}

void AudioEngine::reportFault(std::string reason) {
    {
        std::lock_guard<std::mutex> lock(faultMutex_);
        if (fault_.empty()) {
            fault_ = std::move(reason);
        }
    }
    stopRequested_.store(true, std::memory_order_release);
}

void AudioEngine::captureLoop() {
    ComApartment com;
    MmcssScope mmcss(L"Pro Audio");
    // The DSP chain runs on this thread, and its filter and envelope state
    // decays into the denormal range during silence.
    ScopedNoDenormals noDenormals;

    // Two consecutive empty wakes mean the tapped endpoint has genuinely gone
    // quiet, rather than a single wake that merely raced a packet. Only then is
    // it safe to push silence, which must never land in the middle of real audio.
    constexpr int kIdleWakesBeforeSilence = 2;
    int idleWakes = 0;

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForSingleObject(capture_.eventHandle(), captureWaitMs_);
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            reportFault("キャプチャイベントの待機に失敗しました");
            break;
        }

        bool gotAudio = false;

        // Drain every queued packet. Loopback endpoints deliver nothing at all
        // while the device is idle, which is why the wait above has a timeout.
        for (;;) {
            std::uint32_t packetFrames = 0;
            HRESULT hr = S_OK;
            if (!capture_.nextPacketSize(&packetFrames, &hr)) {
                reportFault(std::format("キャプチャの取得に失敗: {}", hresultToString(hr)));
                break;
            }
            if (packetFrames == 0) {
                break;
            }

            const void* data = nullptr;
            std::uint32_t frames = 0;
            DWORD flags = 0;
            hr = capture_.acquirePacket(&data, &frames, &flags);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                // A success code, not a failure: no packet was handed over. The
                // buffer still has to be released, with a zero frame count, or
                // the capture client stays locked.
                capture_.releasePacket(0);
                break;
            }
            if (FAILED(hr)) {
                reportFault(std::format("キャプチャバッファの取得に失敗: {}", hresultToString(hr)));
                break;
            }

            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0) {
                discontinuities_.fetch_add(1, std::memory_order_relaxed);
            }

            const std::size_t usable = std::min<std::size_t>(frames, capture_.bufferFrames());
            if (usable > 0) {
                if (data != nullptr) {
                    convertToFloat(data, capture_.format(), captureScratch_.data(), channels_, usable);
                } else {
                    // Silent packet: WASAPI leaves the buffer contents undefined.
                    std::memset(captureScratch_.data(), 0, usable * channels_ * sizeof(float));
                }

                if (processor_ != nullptr) {
                    processor_->process(captureScratch_.data(), usable, channels_);
                }

                const std::size_t written = ring_->write(captureScratch_.data(), usable);
                if (written < usable) {
                    overruns_.fetch_add(1, std::memory_order_relaxed);
                }
                capturedFrames_.fetch_add(written, std::memory_order_relaxed);
                gotAudio = true;
            }

            hr = capture_.releasePacket(frames);
            if (FAILED(hr)) {
                reportFault(std::format("キャプチャバッファの解放に失敗: {}", hresultToString(hr)));
                break;
            }
        }

        // A loopback endpoint delivers no packets whatsoever while nothing is
        // playing into it. Top the ring back up to its target so the render side
        // has silence to play instead of starving.
        if (gotAudio) {
            idleWakes = 0;
        } else if (++idleWakes >= kIdleWakesBeforeSilence) {
            const std::size_t fill = ring_->availableToRead();
            if (fill < targetFillFrames_) {
                const std::size_t added = ring_->writeSilence(targetFillFrames_ - fill);
                if (added > 0) {
                    silenceFills_.fetch_add(added, std::memory_order_relaxed);
                }
            }
        }
    }

    AL_DEBUG("キャプチャスレッド終了");
}

void AudioEngine::renderLoop() {
    ComApartment com;
    MmcssScope mmcss(L"Pro Audio");

    const std::uint32_t renderBufferFrames = render_.bufferFrames();

    while (!stopRequested_.load(std::memory_order_acquire)) {
        const DWORD wait = ::WaitForSingleObject(render_.eventHandle(), renderWaitMs_);
        if (stopRequested_.load(std::memory_order_acquire)) {
            break;
        }
        if (wait != WAIT_OBJECT_0 && wait != WAIT_TIMEOUT) {
            reportFault("再生イベントの待機に失敗しました");
            break;
        }

        std::uint32_t padding = 0;
        HRESULT hr = S_OK;
        if (!render_.padding(&padding, &hr)) {
            reportFault(std::format("再生バッファ残量の取得に失敗: {}", hresultToString(hr)));
            break;
        }
        lastRenderPadding_.store(padding, std::memory_order_relaxed);

        const std::uint32_t want = renderBufferFrames - padding;
        if (want == 0) {
            continue;
        }

        // --- clock drift correction ---
        // Trim the resampler's ratio to hold the ring at its target fill. A
        // proportional term is enough: it settles with a standing fill error
        // proportional to the actual drift, and for the ~100 ppm real hardware
        // shows that error is a fraction of a percent of the target.
        //
        // The fill is smoothed first. Reacting to the raw value would modulate
        // the ratio with every scheduling hiccup, which is audible as warble on
        // sustained tones — the very thing this scheme exists to avoid.
        const auto rawFill = static_cast<double>(ring_->availableToRead());
        smoothedFill_ += kFillSmoothing * (rawFill - smoothedFill_);

        const auto target = static_cast<double>(targetFillFrames_);
        const double fillError = (smoothedFill_ - target) / target;

        // A ring filling up means capture is outrunning render, so more input
        // must be consumed per output frame: a lower ratio, i.e. trim below 1.
        const double trim = std::clamp(1.0 - kDriftGain * fillError, 1.0 - kMaxTrim, 1.0 + kMaxTrim);
        resampler_->setRatioTrim(trim);
        driftPpm_.store((trim - 1.0) * 1e6, std::memory_order_relaxed);

        // --- feed the resampler and pull the block ---
        const std::size_t wantedInput = resampler_->inputFramesWanted(want);
        if (wantedInput > 0) {
            const std::size_t capacity = resamplerInput_.size() / channels_;
            const std::size_t toRead = std::min(wantedInput, capacity);
            const std::size_t got = ring_->read(resamplerInput_.data(), toRead);
            if (got > 0) {
                resampler_->pushInput(resamplerInput_.data(), got);
            }
            if (got < toRead) {
                // The ring ran dry. Silence keeps the resampler's phase moving
                // at the right rate, so the stream resumes in time rather than
                // permanently offset.
                resampler_->pushSilence(toRead - got);
            }
        }

        const std::size_t produced = resampler_->pullOutput(renderScratch_.data(), want);
        if (produced < want) {
            std::memset(&renderScratch_[produced * channels_], 0,
                        (want - produced) * channels_ * sizeof(float));
            underruns_.fetch_add(1, std::memory_order_relaxed);
        }

        void* buffer = nullptr;
        hr = render_.acquireBuffer(want, &buffer);
        if (FAILED(hr)) {
            reportFault(std::format("再生バッファの取得に失敗: {}", hresultToString(hr)));
            break;
        }

        convertFromFloat(renderScratch_.data(), channels_, buffer, render_.format(), want);

        hr = render_.releaseBuffer(want, 0);
        if (FAILED(hr)) {
            reportFault(std::format("再生バッファの解放に失敗: {}", hresultToString(hr)));
            break;
        }
        renderedFrames_.fetch_add(want, std::memory_order_relaxed);
    }

    AL_DEBUG("再生スレッド終了");
}

EngineStats AudioEngine::stats() const {
    EngineStats s;
    s.sampleRate = sampleRate_;
    s.capturedFrames = capturedFrames_.load(std::memory_order_relaxed);
    s.renderedFrames = renderedFrames_.load(std::memory_order_relaxed);
    s.underruns = underruns_.load(std::memory_order_relaxed);
    s.overruns = overruns_.load(std::memory_order_relaxed);
    s.discontinuities = discontinuities_.load(std::memory_order_relaxed);
    s.silenceFills = silenceFills_.load(std::memory_order_relaxed);
    s.captureSampleRate = sampleRate_;
    s.renderSampleRate = renderSampleRate_;
    s.driftPpm = driftPpm_.load(std::memory_order_relaxed);

    if (sampleRate_ > 0) {
        const double rate = static_cast<double>(sampleRate_);
        if (ring_) {
            s.ringFillMs = 1000.0 * static_cast<double>(ring_->availableToRead()) / rate;
        }
        s.renderPaddingMs =
            1000.0 * static_cast<double>(lastRenderPadding_.load(std::memory_order_relaxed)) / rate;
    }
    s.captureLatencyMs = capture_.streamLatencyMs();
    s.renderLatencyMs = render_.streamLatencyMs();
    return s;
}

}  // namespace audiolens
