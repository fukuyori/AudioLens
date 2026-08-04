// M1 verification tool: taps an endpoint, runs the (currently empty) processing
// chain, and plays the result on another endpoint while reporting latency and
// glitch counters.
//
// Typical use with a virtual cable installed and set as the system default:
//     audiolens_passthrough --list
//     audiolens_passthrough --capture "CABLE Input" --render "Headphones"

#include "common/com.h"
#include "common/log.h"
#include "core/preset.h"
#include "dsp/dsp_chain.h"
#include "engine/audio_engine.h"
#include "engine/default_device.h"
#include "engine/device_manager.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <string>
#include <thread>
#include <vector>

using namespace audiolens;

namespace {

std::atomic<bool> g_stopRequested{false};

/// Set by --takeover: the endpoint that was default before we displaced it.
/// A global because the console handler has to be able to put it back, and it
/// runs on its own thread with no access to main's locals.
std::wstring g_restoreDeviceId;
std::atomic<bool> g_restoreDone{false};

/// Puts the system default back. Safe to call more than once and from any
/// thread; only the first call does anything.
void restoreDefaultDevice() {
    if (g_restoreDeviceId.empty()) return;
    if (g_restoreDone.exchange(true, std::memory_order_acq_rel)) return;

    // The console handler runs on a thread of its own, which has no apartment.
    const ComApartment com;
    std::string error;
    if (setDefaultRenderDevice(g_restoreDeviceId, &error)) {
        std::puts("既定の再生デバイスを元に戻しました。");
    } else {
        std::printf("既定の再生デバイスを戻せませんでした: %s\n", error.c_str());
        std::puts("サウンド設定から手動で戻してください。");
    }
    std::fflush(stdout);
}

BOOL WINAPI consoleHandler(DWORD signal) {
    if (signal == CTRL_C_EVENT || signal == CTRL_BREAK_EVENT || signal == CTRL_CLOSE_EVENT) {
        // CTRL_CLOSE_EVENT gives the process only a few seconds before it is
        // killed, so the restore cannot wait for the main loop to notice.
        // Leaving the machine routed to a cable nobody is listening to is the
        // exact failure N-04 exists to prevent.
        restoreDefaultDevice();
        g_stopRequested.store(true, std::memory_order_release);
        return TRUE;
    }
    return FALSE;
}

void printUsage() {
    std::puts(
        "AudioLens パススルー検証ツール " AUDIOLENS_VERSION "\n"
        "\n"
        "使い方:\n"
        "  audiolens_passthrough --list\n"
        "  audiolens_passthrough --capture <指定> --render <指定> [オプション]\n"
        "\n"
        "デバイス指定: --list の番号 / デバイス名の一部 / 完全な ID / default\n"
        "\n"
        "オプション:\n"
        "  --list              デバイス一覧を表示して終了\n"
        "  --set-default <指定> 既定の再生デバイスを変更して終了。音が出なくなったときの\n"
        "                      復旧用(仮想ケーブルが既定のまま取り残された場合など)\n"
        "  --invalidate <指定>  デバイスのサンプルレートを一時的に変えて元に戻す。\n"
        "                      要件 N-03 の「サンプルレート変更への追従」を試すため\n"
        "  --capture <指定>    取り込み元。既定では再生デバイスをループバックで取り込む\n"
        "  --render <指定>     出力先の再生デバイス\n"
        "  --no-loopback       取り込み元を録音デバイス(マイク等)として扱う\n"
        "  --buffer <ms>       各エンドポイントのバッファ長 (既定 10)\n"
        "  --ring <ms>         内部リングバッファ長 (既定 40)\n"
        "  --duration <秒>     実行時間。0 で Ctrl+C まで継続 (既定 0)\n"
        "  --takeover          取り込み元を実行中だけシステム既定の出力にし、終了時に戻す。\n"
        "                      仮想ケーブル経由で全アプリの音を通したいときに使う\n"
        "  --verbose           詳細ログを出力\n"
        "\n"
        "音声補正:\n"
        "  --preset <id>       プリセット。省略すると補正なしの素通し\n"
        "  --bass <0-100>      「低音」。プリセットの値を上書き\n"
        "  --clarity <0-100>   「声の明瞭さ」\n"
        "  --leveling <0-100>  「音量差」\n"
        "  --ab <秒>           指定秒ごとに補正のオン/オフを切り替えて比較する\n");
}

bool parseSlider(const char* text, int* out) {
    char* end = nullptr;
    const long value = std::strtol(text, &end, 10);
    if (end == text || *end != '\0' || value < 0 || value > 100) {
        return false;
    }
    *out = static_cast<int>(value);
    return true;
}

void printDeviceList() {
    for (const DeviceDirection direction : {DeviceDirection::Render, DeviceDirection::Capture}) {
        const char* label = direction == DeviceDirection::Render ? "再生デバイス" : "録音デバイス";
        std::printf("\n== %s ==\n", label);

        const std::vector<DeviceInfo> devices = enumerateDevices(direction);
        if (devices.empty()) {
            std::puts("  (なし)");
            continue;
        }
        for (std::size_t i = 0; i < devices.size(); ++i) {
            std::printf("  %2zu. %s%s\n", i + 1, devices[i].friendlyName.c_str(),
                        devices[i].isDefault ? "  [既定]" : "");
        }
    }
    std::puts(
        "\nヒント: システム音声を取り込むには、取り込み元に「再生デバイス」を指定します"
        "(ループバック)。\n"
        "        出力先には別の再生デバイスを指定してください。同じデバイスを指定すると"
        "音が回り込みます。");
}

/// Returns nullptr-equivalent (nullopt semantics via `ok`) rather than throwing,
/// so a bad command line produces a usage message instead of a crash.
bool parseUInt(const char* text, std::uint32_t* out) {
    char* end = nullptr;
    const unsigned long value = std::strtoul(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCP(CP_UTF8);
    ::SetConsoleCtrlHandler(consoleHandler, TRUE);

    ComApartment com;

    bool listOnly = false;
    bool loopback = true;
    bool takeover = false;
    std::string setDefaultSelector;
    std::string invalidateSelector;
    std::string captureSelector;
    std::string renderSelector;
    std::uint32_t bufferMs = 10;
    std::uint32_t ringMs = 40;
    std::uint32_t durationSec = 0;
    std::string presetId;
    int bass = -1;
    int clarity = -1;
    int leveling = -1;
    std::uint32_t abSeconds = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s には値が必要です\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--list") {
            listOnly = true;
        } else if (arg == "--capture") {
            captureSelector = next("--capture");
        } else if (arg == "--render") {
            renderSelector = next("--render");
        } else if (arg == "--no-loopback") {
            loopback = false;
        } else if (arg == "--buffer") {
            if (!parseUInt(next("--buffer"), &bufferMs)) {
                std::puts("--buffer の値が不正です");
                return 2;
            }
        } else if (arg == "--ring") {
            if (!parseUInt(next("--ring"), &ringMs)) {
                std::puts("--ring の値が不正です");
                return 2;
            }
        } else if (arg == "--duration") {
            if (!parseUInt(next("--duration"), &durationSec)) {
                std::puts("--duration の値が不正です");
                return 2;
            }
        } else if (arg == "--preset") {
            presetId = next("--preset");
        } else if (arg == "--bass") {
            if (!parseSlider(next("--bass"), &bass)) {
                std::puts("--bass は 0〜100 で指定してください");
                return 2;
            }
        } else if (arg == "--clarity") {
            if (!parseSlider(next("--clarity"), &clarity)) {
                std::puts("--clarity は 0〜100 で指定してください");
                return 2;
            }
        } else if (arg == "--leveling") {
            if (!parseSlider(next("--leveling"), &leveling)) {
                std::puts("--leveling は 0〜100 で指定してください");
                return 2;
            }
        } else if (arg == "--ab") {
            if (!parseUInt(next("--ab"), &abSeconds)) {
                std::puts("--ab の値が不正です");
                return 2;
            }
        } else if (arg == "--takeover") {
            takeover = true;
        } else if (arg == "--set-default") {
            setDefaultSelector = next("--set-default");
        } else if (arg == "--invalidate") {
            invalidateSelector = next("--invalidate");
        } else if (arg == "--verbose") {
            setLogLevel(LogLevel::Debug);
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::printf("不明な引数: %s\n\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    if (listOnly) {
        printDeviceList();
        return 0;
    }

    // Provokes the device change requirement N-03 has to survive, without
    // administrator rights and without unplugging anything: Windows invalidates
    // every stream on an endpoint whose shared-mode format changes.
    if (!invalidateSelector.empty()) {
        std::string selectorError;
        const std::wstring id =
            resolveDeviceSelector(invalidateSelector, DeviceDirection::Render, &selectorError);
        if (id.empty()) {
            std::printf("デバイスを解決できません: %s\n", selectorError.c_str());
            printDeviceList();
            return 1;
        }

        std::uint32_t original = 0;
        std::string formatError;
        if (!setRenderDeviceSampleRate(id, 0, &original, &formatError)) {
            std::printf("現在の形式を取得できません: %s\n", formatError.c_str());
            return 1;
        }
        const std::uint32_t other = original == 48000 ? 44100 : 48000;

        std::printf("サンプルレートを %u → %u Hz に変更します\n", original, other);
        if (!setRenderDeviceSampleRate(id, other, nullptr, &formatError)) {
            std::printf("変更できません: %s\n", formatError.c_str());
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::seconds(2));

        std::printf("サンプルレートを %u Hz に戻します\n", original);
        if (!setRenderDeviceSampleRate(id, original, nullptr, &formatError)) {
            std::printf("戻せません: %s\n手動でサウンド設定から戻してください。\n",
                        formatError.c_str());
            return 1;
        }
        std::puts("完了しました。");
        return 0;
    }

    if (!setDefaultSelector.empty()) {
        std::string selectorError;
        const std::wstring id =
            resolveDeviceSelector(setDefaultSelector, DeviceDirection::Render, &selectorError);
        if (id.empty()) {
            std::printf("デバイスを解決できません: %s\n", selectorError.c_str());
            printDeviceList();
            return 1;
        }
        std::string setError;
        if (!setDefaultRenderDevice(id, &setError)) {
            std::printf("既定デバイスを変更できません: %s\n", setError.c_str());
            return 1;
        }
        std::puts("既定の再生デバイスを変更しました。");
        printDeviceList();
        return 0;
    }

    if (captureSelector.empty() || renderSelector.empty()) {
        printUsage();
        std::puts("\n--capture と --render の両方を指定してください。");
        printDeviceList();
        return 2;
    }

    // A loopback tap reads from a *render* endpoint, so both selectors resolve
    // against the render list in the default configuration.
    const DeviceDirection captureDirection =
        loopback ? DeviceDirection::Render : DeviceDirection::Capture;

    std::string resolveError;
    const std::wstring captureId = resolveDeviceSelector(captureSelector, captureDirection, &resolveError);
    if (captureId.empty()) {
        std::printf("取り込み元を解決できません: %s\n", resolveError.c_str());
        return 1;
    }
    const std::wstring renderId = resolveDeviceSelector(renderSelector, DeviceDirection::Render, &resolveError);
    if (renderId.empty()) {
        std::printf("出力先を解決できません: %s\n", resolveError.c_str());
        return 1;
    }
    if (loopback && captureId == renderId) {
        std::puts(
            "取り込み元と出力先が同じデバイスです。音が回り込むため、別のデバイスを指定してください。");
        return 1;
    }

    EngineConfig config;
    config.captureDeviceId = captureId;
    config.renderDeviceId = renderId;
    config.captureLoopback = loopback;
    config.bufferMs = bufferMs;
    config.ringMs = ringMs;

    AudioEngine engine;
    dsp::DspChain chain;

    if (!presetId.empty()) {
        const Preset* preset = findBuiltinPreset(presetId);
        if (preset == nullptr) {
            std::printf("プリセット '%s' が見つかりません。指定できるのは:", presetId.c_str());
            for (const Preset& p : builtinPresets()) {
                std::printf(" %s", p.id.c_str());
            }
            std::puts("");
            return 1;
        }

        SliderValues sliders = preset->sliders;
        if (bass >= 0) sliders.bass = bass;
        if (clarity >= 0) sliders.clarity = clarity;
        if (leveling >= 0) sliders.leveling = leveling;

        chain.setParameters(resolveParameters(*preset, sliders));
        engine.setProcessor(&chain);

        std::printf("プリセット: %s (%s) / 低音 %d / 声の明瞭さ %d / 音量差 %d\n",
                    preset->name.c_str(), preset->id.c_str(), sliders.bass, sliders.clarity,
                    sliders.leveling);
    }

    // Taking over before starting, not after: between the two the audio has
    // nowhere to go, and that gap should be as short as possible.
    if (takeover) {
        const std::wstring previous = currentDefaultRenderDeviceId();
        if (previous == captureId) {
            std::puts("取り込み元は既にシステム既定の出力です。");
        } else {
            std::string takeoverError;
            if (!setDefaultRenderDevice(captureId, &takeoverError)) {
                std::printf("既定デバイスを変更できません: %s\n", takeoverError.c_str());
                return 1;
            }
            // Only recorded once the switch succeeded, so a failure here never
            // leaves a restore pointing at the wrong device.
            g_restoreDeviceId = previous;
            std::puts("取り込み元をシステム既定の出力にしました (終了時に戻します)。");
        }
    }

    std::string error;
    if (!engine.start(config, &error)) {
        std::printf("エンジンを開始できません: %s\n", error.c_str());
        restoreDefaultDevice();
        return 1;
    }

    if (!presetId.empty()) {
        std::printf("DSP 遅延: %.2f ms\n", chain.latencyMs());
    }
    if (abSeconds > 0 && !presetId.empty()) {
        std::printf("A/B 比較: %u 秒ごとに補正のオン/オフを切り替えます\n", abSeconds);
    }

    std::puts("\n再生を開始しました。Ctrl+C で停止します。\n");

    const auto startTime = std::chrono::steady_clock::now();
    auto nextReport = startTime + std::chrono::seconds(5);
    auto nextAbToggle = startTime + std::chrono::seconds(abSeconds > 0 ? abSeconds : 1);
    EngineStats previous;

    while (!g_stopRequested.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        if (!engine.faultReason().empty()) {
            std::printf("\n停止しました: %s\n", engine.faultReason().c_str());
            break;
        }

        const auto now = std::chrono::steady_clock::now();
        if (durationSec > 0 && now - startTime >= std::chrono::seconds(durationSec)) {
            break;
        }
        if (abSeconds > 0 && !presetId.empty() && now >= nextAbToggle) {
            nextAbToggle = now + std::chrono::seconds(abSeconds);
            chain.setBypass(!chain.bypassed());
            std::printf("        >>> 補正 %s\n", chain.bypassed() ? "オフ" : "オン");
            std::fflush(stdout);
        }

        if (now < nextReport) {
            continue;
        }
        nextReport = now + std::chrono::seconds(5);

        const EngineStats s = engine.stats();
        const auto elapsed = std::chrono::duration<double>(now - startTime).count();
        std::printf(
            "[%6.0fs] 遅延 %.1f ms (取込 %.1f / リング %.1f / 出力残 %.1f / 再生 %.1f)  "
            "underrun %llu (+%llu)  overrun %llu (+%llu)  不連続 %llu  クロック差 %+.0f ppm\n",
            elapsed, s.estimatedLatencyMs(), s.captureLatencyMs, s.ringFillMs, s.renderPaddingMs,
            s.renderLatencyMs,
            static_cast<unsigned long long>(s.underruns),
            static_cast<unsigned long long>(s.underruns - previous.underruns),
            static_cast<unsigned long long>(s.overruns),
            static_cast<unsigned long long>(s.overruns - previous.overruns),
            static_cast<unsigned long long>(s.discontinuities),
            s.driftPpm);
        std::fflush(stdout);
        previous = s;
    }

    const EngineStats final = engine.stats();
    engine.stop();
    restoreDefaultDevice();

    const auto totalSec =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - startTime).count();

    std::puts("\n== 実行結果 ==");
    std::printf("  実行時間      : %.1f 秒\n", totalSec);
    std::printf("  取り込み      : %llu フレーム\n",
                static_cast<unsigned long long>(final.capturedFrames));
    std::printf("  再生          : %llu フレーム\n",
                static_cast<unsigned long long>(final.renderedFrames));
    std::printf("  underrun      : %llu\n", static_cast<unsigned long long>(final.underruns));
    std::printf("  overrun       : %llu\n", static_cast<unsigned long long>(final.overruns));
    std::printf("  不連続        : %llu\n", static_cast<unsigned long long>(final.discontinuities));
    std::printf("  無音補填      : %llu フレーム\n",
                static_cast<unsigned long long>(final.silenceFills));
    std::printf("  推定遅延      : %.1f ms\n", final.estimatedLatencyMs());
    // The margin, stated plainly. An overrun is the fill reaching capacity and
    // an underrun is it reaching zero, so how close it came to either is the
    // only honest answer to "is this configuration safe?".
    std::printf("  リング容量    : %.1f ms\n", final.ringCapacityMs);
    std::printf("  リング充填    : 最小 %.1f / 最大 %.1f ms  (余裕 下 %.1f / 上 %.1f ms)\n",
                final.ringFillMinMs, final.ringFillMaxMs, final.ringFillMinMs,
                final.ringCapacityMs - final.ringFillMaxMs);
    std::printf("  サンプルレート: 取込 %u Hz / 再生 %u Hz%s\n", final.captureSampleRate,
                final.renderSampleRate,
                final.captureSampleRate == final.renderSampleRate ? "" : "(変換あり)");

    // Once the control loop has settled, the ratio trim *is* the measured
    // difference between the two endpoints' clocks. It only means anything
    // after real audio has flowed for a while.
    if (totalSec > 5.0 && final.capturedFrames > 0) {
        std::printf("  クロック差    : %+.0f ppm\n", final.driftPpm);
    }

    if (final.capturedFrames == 0) {
        std::puts(
            "\n注意: 取り込みフレームが 0 です。取り込み元のデバイスに音声が再生されていません。\n"
            "      効果を確認するには、取り込み元を既定の再生デバイスにしたうえで音楽や動画を"
            "再生してください。");
    }

    return 0;
}
