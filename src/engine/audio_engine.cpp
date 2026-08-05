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

/// Render callbacks the output may stay silent while the ring fills to target.
///
/// Filling takes one target's worth of real time plus the capture side's first
/// packet — around 90 ms, or nine callbacks. A hundred is therefore a very loose
/// bound, and it is a bound rather than a wait-forever because a source that
/// never delivers must not leave the user in silence: a thin ring still plays.
constexpr int kMaxPrimingWakes = 100;

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

    // Capacity and target fill are sized separately, and that separation is the
    // point.
    //
    // Latency is set by the *target*: audio waits there and nowhere else. Safety
    // is set by the *capacity*: an overrun is the fill reaching the top and an
    // underrun is it reaching the bottom. Tying the target to half the capacity,
    // as this did, means every millisecond of safety costs a millisecond of
    // delay — so it was sized to whichever mattered more and was short of the
    // other.
    //
    // Measured through a virtual cable over three minutes, the fill swung forty
    // milliseconds above target with nothing wrong: a cable is fed by whatever
    // application is playing, not by a crystal. Against a capacity of 88 ms that
    // left four milliseconds of headroom, which is not a margin, it is luck.
    //
    // Three periods of target is what the delay is worth; six of capacity is
    // what the delivery demands.
    //
    // Two was the figure until the idle top-up was fixed (2026-08-05), and it
    // only looked sufficient because that top-up was quietly refilling the ring
    // every period — at the price of splicing silence into the audio. With the
    // splicing gone the true behaviour showed: the fill dipped to 3.6 ms, a
    // sixth of a period from running dry, on ordinary continuous playback.
    //
    // The extra period costs 22 ms of delay here. That is the trade the whole
    // engine is meant to make: a dropout is heard as a fault, and 22 ms is not
    // heard at all.
    const std::size_t period = capture_.bufferFrames();
    const std::size_t ringFrames =
        std::max<std::size_t>(msToFrames(config_.ringMs, sampleRate_), period * 6);
    ring_ = std::make_unique<RingBuffer>(ringFrames, channels_);

    // Kept clear of both ends even if a caller asks for an unusually small ring.
    targetFillFrames_ = std::clamp<std::size_t>(period * 3, period, ringFrames - period);
    capturePeriodFrames_ = capture_.bufferFrames();

    renderPrimed_ = false;
    renderPrimingWakes_ = 0;

    // Pre-filling the ring with silence here was tried first and does not work:
    // the render side consumes it before the capture side has delivered
    // anything, so the fill lands exactly where it would have without it. The
    // hold-off in renderLoop() is what actually establishes the level; see the
    // note there.
    smoothedFill_ = static_cast<double>(targetFillFrames_);
    ringFillMin_ = ringFrames;
    ringFillMax_ = 0;
    ringReachedTarget_ = false;
    ringFillMinFrames_.store(static_cast<std::uint32_t>(ringFrames), std::memory_order_relaxed);
    ringFillMaxFrames_.store(0, std::memory_order_relaxed);
    ringCapacityFrames_.store(static_cast<std::uint32_t>(ringFrames), std::memory_order_relaxed);

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
    // A quarter of a period, not a half. This timeout is also how quickly the
    // idle top-up notices that a loopback endpoint has gone quiet, and the ring
    // drains in real time while it waits: at half a period, two wakes were up to
    // a full period of draining before anything was put back.
    captureWaitMs_ = std::max<DWORD>(1, static_cast<DWORD>(capturePeriodMs / 4.0));
    renderWaitMs_ = std::max<DWORD>(20, static_cast<DWORD>(renderPeriodMs * 4.0));

    // How long the endpoint must deliver *nothing* before its silence is taken
    // to be real. This has to be a span of time, and it has to be longer than
    // one capture period, because one capture period is the ordinary gap
    // between packets when audio is playing perfectly normally.
    //
    // It was a fixed count of two wakes, which was right when the timeout was a
    // whole period and became wrong the moment the timeout was shortened to a
    // quarter of one: two wakes then spanned half a period, so *every* ordinary
    // gap between packets looked like the source going quiet, and silence was
    // spliced into audio that had never stopped. Three periods leaves no room
    // for that to happen by accident, and still notices real silence long
    // before the ring — which holds several periods — could run dry.
    captureIdleWakes_ = std::max<int>(
        2, static_cast<int>(std::ceil(3.0 * capturePeriodMs / captureWaitMs_)));

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

    // Silence must never land in the middle of real audio, so the threshold is
    // computed from the wait timeout in open() rather than fixed here: see the
    // note there for what a fixed count cost.
    const int idleWakesBeforeSilence = captureIdleWakes_;
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
        } else if (++idleWakes >= idleWakesBeforeSilence) {
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

        // --- hold off until the ring has filled to target ---
        //
        // The render side is ready to pull the instant it starts; the capture
        // side cannot deliver anything until its first packet completes, one
        // whole capture period later. Pulling through that window drains the
        // ring by a capture period plus the render buffer's initial fill — 44 ms
        // of the 66 ms target, measured. What is left is then handed to the
        // drift loop to make up, and that loop has a seven-minute crossover, so
        // the ring spent the first minutes of every run well below target with
        // the margin to match.
        //
        // Emitting silence until the ring is ready costs one target's worth of
        // silence at startup, which nobody hears, and hands the loop a ring that
        // already sits where it belongs.
        if (!renderPrimed_) {
            if (ring_->availableToRead() < targetFillFrames_ &&
                renderPrimingWakes_ < kMaxPrimingWakes) {
                ++renderPrimingWakes_;
                void* silence = nullptr;
                if (FAILED(render_.acquireBuffer(want, &silence))) {
                    reportFault("再生バッファの取得に失敗しました");
                    break;
                }
                // Not counted as an underrun: the stream has not started yet, so
                // this is not the ring failing to keep up with anything.
                if (FAILED(render_.releaseBuffer(want, AUDCLNT_BUFFERFLAGS_SILENT))) {
                    reportFault("再生バッファの解放に失敗しました");
                    break;
                }
                continue;
            }
            // The bound exists so that a source which never delivers cannot
            // hold the output silent indefinitely; better a thin ring than no
            // sound at all.
            renderPrimed_ = true;
            AL_DEBUG("再生開始: リング {} / 目標 {} フレーム ({} 回待機)",
                     ring_->availableToRead(), targetFillFrames_, renderPrimingWakes_);
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
        const std::size_t fillFrames = ring_->availableToRead();
        const auto rawFill = static_cast<double>(fillFrames);
        smoothedFill_ += kFillSmoothing * (rawFill - smoothedFill_);

        // Recorded every callback, not every time somebody asks: an overrun is
        // decided in the one callback where the fill peaked, and a reader
        // sampling twice a second will never see it.
        //
        // The low mark only starts counting once the ring has something real in
        // it. Before that it is empty by construction, and recording zero there
        // would report no margin at all on every run regardless of how the run
        // then went.
        //
        // Armed at one period, not at the target. Arming at the target assumed
        // the fill always gets there, and when it does not — which is exactly
        // the case worth knowing about — the low mark was never recorded at all
        // and the run reported a full ring of margin. A diagnostic that goes
        // quiet precisely when the thing it measures goes wrong is worse than
        // none, because it reads as good news.
        if (!ringReachedTarget_ && fillFrames >= capturePeriodFrames_) {
            ringReachedTarget_ = true;
        }
        if (ringReachedTarget_ && fillFrames < ringFillMin_) {
            ringFillMin_ = fillFrames;
            ringFillMinFrames_.store(static_cast<std::uint32_t>(fillFrames),
                                     std::memory_order_relaxed);
        }
        if (fillFrames > ringFillMax_) {
            ringFillMax_ = fillFrames;
            ringFillMaxFrames_.store(static_cast<std::uint32_t>(fillFrames),
                                     std::memory_order_relaxed);
        }

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
        s.ringFillMinMs =
            1000.0 * ringFillMinFrames_.load(std::memory_order_relaxed) / rate;
        s.ringFillMaxMs =
            1000.0 * ringFillMaxFrames_.load(std::memory_order_relaxed) / rate;
        s.ringCapacityMs =
            1000.0 * ringCapacityFrames_.load(std::memory_order_relaxed) / rate;
        s.renderPaddingMs =
            1000.0 * static_cast<double>(lastRenderPadding_.load(std::memory_order_relaxed)) / rate;
    }
    s.captureLatencyMs = capture_.streamLatencyMs();
    s.renderLatencyMs = render_.streamLatencyMs();
    return s;
}

}  // namespace audiolens
