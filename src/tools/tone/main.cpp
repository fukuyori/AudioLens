// Signal generator for verifying and measuring the engine.
//
// M1 uses it to prove that real (non-silent) audio reaches the loopback tap:
// play a tone into the endpoint the passthrough tool is tapping and watch its
// capture counter move. M2 will reuse it to sweep the DSP chain.
//
//     audiolens_tone --device "スピーカー" --freq 1000 --level -20 --duration 10

#include "audiofile/wav.h"
#include "common/com.h"
#include "common/log.h"
#include "engine/audio_format.h"
#include "engine/device_manager.h"
#include "engine/wasapi_render.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <numbers>
#include <string>
#include <vector>

using namespace audiolens;

namespace {

std::atomic<bool> g_stopRequested{false};

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        g_stopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

void printUsage() {
    std::puts(
        "AudioLens 信号発生ツール\n"
        "\n"
        "使い方:\n"
        "  audiolens_tone --device <指定> [オプション]      デバイスへ出力\n"
        "  audiolens_tone --wav <path> [オプション]         WAV へ書き出し\n"
        "\n"
        "オプション:\n"
        "  --device <指定>   出力先の再生デバイス(番号 / 名前の一部 / default)\n"
        "  --wav <path>      再生せず WAV ファイルへ書き出す\n"
        "  --signal <種類>   tone(既定) または dynamic\n"
        "                    dynamic: 静かな部分と大きな部分を交互に並べた素材。\n"
        "                             「音量差」の効果を測るために使う\n"
        "  --freq <Hz>       周波数 (既定 1000)\n"
        "  --level <dBFS>    振幅。0 が最大、負の値で小さくなる (既定 -20)\n"
        "  --duration <秒>   長さ。デバイス出力時は 0 で Ctrl+C まで継続 (既定 10)\n"
        "  --rate <Hz>       WAV のサンプルレート (既定 48000)\n");
}

/// Deterministic broadband noise, so a measurement can be repeated exactly.
class NoiseSource {
public:
    explicit NoiseSource(std::uint32_t seed) : state_(seed) {}
    double next() {
        state_ = state_ * 1664525u + 1013904223u;
        return static_cast<double>(state_ >> 8) / 8388608.0 - 1.0;
    }

private:
    std::uint32_t state_;
};

/// Alternating quiet and loud passages: dialogue and then an explosion, in the
/// crudest possible terms. This is the material the「映画」and「深夜」presets
/// exist to tame, and what the loudness range figure is meant to capture.
audiolens::AudioBuffer makeDynamicSignal(double seconds, double loudDb, std::uint32_t sampleRate) {
    constexpr double kQuietOffsetDb = -18.0;
    constexpr double kSectionSeconds = 4.0;

    audiolens::AudioBuffer buffer;
    buffer.sampleRate = sampleRate;
    buffer.channels = 2;

    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    const auto sectionFrames = static_cast<std::size_t>(kSectionSeconds * sampleRate);
    buffer.samples.resize(frames * 2);

    NoiseSource noise(20260803u);
    const double loudAmplitude = std::pow(10.0, loudDb / 20.0);
    const double quietAmplitude = std::pow(10.0, (loudDb + kQuietOffsetDb) / 20.0);

    for (std::size_t f = 0; f < frames; ++f) {
        const bool loud = ((f / sectionFrames) % 2) == 1;
        const double amplitude = loud ? loudAmplitude : quietAmplitude;
        const auto v = static_cast<float>(noise.next() * amplitude);
        buffer.samples[f * 2] = v;
        buffer.samples[f * 2 + 1] = v;
    }
    return buffer;
}

audiolens::AudioBuffer makeToneSignal(double freqHz, double levelDb, double seconds,
                                      std::uint32_t sampleRate) {
    audiolens::AudioBuffer buffer;
    buffer.sampleRate = sampleRate;
    buffer.channels = 2;

    const auto frames = static_cast<std::size_t>(seconds * sampleRate);
    buffer.samples.resize(frames * 2);

    const double amplitude = std::pow(10.0, levelDb / 20.0);
    const double step = 2.0 * std::numbers::pi * freqHz / sampleRate;
    for (std::size_t f = 0; f < frames; ++f) {
        const auto v = static_cast<float>(std::sin(step * static_cast<double>(f)) * amplitude);
        buffer.samples[f * 2] = v;
        buffer.samples[f * 2 + 1] = v;
    }
    return buffer;
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCtrlHandler(consoleHandler, TRUE);

    ComApartment com;

    std::string deviceSelector;
    std::string wavPath;
    std::string signalKind = "tone";
    double freqHz = 1000.0;
    double levelDb = -20.0;
    double durationSec = 10.0;
    std::uint32_t wavRate = 48000;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s には値が必要です\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--device") {
            deviceSelector = next("--device");
        } else if (arg == "--wav") {
            wavPath = next("--wav");
        } else if (arg == "--signal") {
            signalKind = next("--signal");
        } else if (arg == "--rate") {
            wavRate = static_cast<std::uint32_t>(std::atoi(next("--rate")));
        } else if (arg == "--freq") {
            freqHz = std::atof(next("--freq"));
        } else if (arg == "--level") {
            levelDb = std::atof(next("--level"));
        } else if (arg == "--duration") {
            durationSec = std::atof(next("--duration"));
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::printf("不明な引数: %s\n\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    if (levelDb > 0.0) {
        std::puts("--level は 0 dBFS 以下で指定してください。");
        return 2;
    }
    if (signalKind != "tone" && signalKind != "dynamic") {
        std::puts("--signal は tone または dynamic を指定してください。");
        return 2;
    }

    if (!wavPath.empty()) {
        const double seconds = durationSec > 0.0 ? durationSec : 10.0;
        const audiolens::AudioBuffer buffer =
            signalKind == "dynamic" ? makeDynamicSignal(seconds, levelDb, wavRate)
                                    : makeToneSignal(freqHz, levelDb, seconds, wavRate);

        std::string writeError;
        if (!writeWav(wavPath, buffer, audiolens::WavSampleFormat::Float32, &writeError)) {
            std::printf("WAV を書き出せません: %s\n", writeError.c_str());
            return 1;
        }
        std::printf("書き出しました: %s (%s / %.1f 秒 / %u Hz)\n", wavPath.c_str(),
                    signalKind.c_str(), buffer.durationSeconds(), buffer.sampleRate);
        return 0;
    }

    if (deviceSelector.empty()) {
        printUsage();
        return 2;
    }

    std::string resolveError;
    const std::wstring deviceId =
        resolveDeviceSelector(deviceSelector, DeviceDirection::Render, &resolveError);
    if (deviceId.empty()) {
        std::printf("デバイスを解決できません: %s\n", resolveError.c_str());
        return 1;
    }

    WasapiRender render;
    std::string error;
    if (!render.open(deviceId, 10, &error)) {
        std::printf("デバイスを開けません: %s\n", error.c_str());
        return 1;
    }
    if (!render.start(&error)) {
        std::printf("再生を開始できません: %s\n", error.c_str());
        return 1;
    }

    const StreamFormat format = render.format();
    const double amplitude = std::pow(10.0, levelDb / 20.0);
    const double phaseStep = 2.0 * std::numbers::pi * freqHz / format.sampleRate;
    double phase = 0.0;

    constexpr std::uint32_t kChannels = 2;
    std::vector<float> scratch(static_cast<std::size_t>(render.bufferFrames()) * kChannels, 0.0f);

    std::printf("%.0f Hz / %.1f dBFS を %s に出力します(%.0f 秒)。Ctrl+C で停止。\n", freqHz,
                levelDb, deviceSelector.c_str(), durationSec);

    const auto start = std::chrono::steady_clock::now();
    while (!g_stopRequested.load(std::memory_order_acquire)) {
        if (durationSec > 0.0 &&
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >=
                durationSec) {
            break;
        }

        if (::WaitForSingleObject(render.eventHandle(), 200) != WAIT_OBJECT_0) {
            continue;
        }

        std::uint32_t padding = 0;
        HRESULT hr = S_OK;
        if (!render.padding(&padding, &hr)) {
            std::printf("再生バッファ残量の取得に失敗: %s\n", hresultToString(hr).c_str());
            break;
        }
        const std::uint32_t want = render.bufferFrames() - padding;
        if (want == 0) {
            continue;
        }

        for (std::uint32_t f = 0; f < want; ++f) {
            const auto sample = static_cast<float>(std::sin(phase) * amplitude);
            scratch[f * kChannels] = sample;
            scratch[f * kChannels + 1] = sample;
            phase += phaseStep;
            if (phase > 2.0 * std::numbers::pi) {
                phase -= 2.0 * std::numbers::pi;
            }
        }

        void* buffer = nullptr;
        hr = render.acquireBuffer(want, &buffer);
        if (FAILED(hr)) {
            std::printf("再生バッファの取得に失敗: %s\n", hresultToString(hr).c_str());
            break;
        }
        convertFromFloat(scratch.data(), kChannels, buffer, format, want);
        hr = render.releaseBuffer(want, 0);
        if (FAILED(hr)) {
            std::printf("再生バッファの解放に失敗: %s\n", hresultToString(hr).c_str());
            break;
        }
    }

    render.stop();
    std::puts("完了しました。");
    return 0;
}
