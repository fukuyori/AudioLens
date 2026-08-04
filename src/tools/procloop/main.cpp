// Spike for the driverless capture route (driver/README.md §5.0.5).
//
// Windows can hand an application the audio of *other* processes without any
// virtual device, through ActivateAudioInterfaceAsync with
// AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK. If AudioLens could tap system
// audio that way, it would need no driver of its own, and the whole EV
// certificate question would stop mattering.
//
// One thing decides whether that works. Capturing does not stop the original
// audio reaching the speakers, so the user would hear the untouched sound and
// the corrected sound at once. The only way out is to silence the source
// applications and play back only the processed signal — which is useless if
// silencing them also silences what we capture.
//
//     取り込みは、対象を消音しても届くのか?
//
// That is the question this tool exists to answer, and nothing more. It reports
// the captured level next to the target session's mute state, so the two can be
// read against each other while the mute is toggled in the volume mixer.
//
//     audiolens_procloop --list
//     audiolens_procloop --pid 1234
//     audiolens_procloop --exclude-self
//
// A second, separate question is which Windows versions offer this at all. The
// SDK gates the declarations on NTDDI_WIN10_FE (build 20348), which is past the
// last Windows 10 client build (19045). If that gate reflects runtime
// availability, this route is Windows 11 only and requirement N-06 has to
// change. This tool prints its own OS build so a run on Windows 10 settles it.

#include "common/com.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <wrl/implements.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <format>
#include <string>
#include <thread>
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
        "AudioLens プロセスループバック検証ツール (案 E)\n"
        "\n"
        "使い方:\n"
        "  audiolens_procloop --list                 再生中のセッションを一覧表示\n"
        "  audiolens_procloop --pid <PID>            そのプロセスの音だけを取り込む\n"
        "  audiolens_procloop --exclude-self         自分以外のすべての音を取り込む\n"
        "\n"
        "オプション:\n"
        "  --duration <秒>    実行時間。0 で Ctrl+C まで継続 (既定 0)\n"
        "  --auto-mute <秒>   その秒数だけ普通に測ったあと、対象を自動で消音して\n"
        "                     測り続ける。終了時に元の消音状態へ戻す\n"
        "\n"
        "判定のしかた(自動):\n"
        "  audiolens_tone --device default --duration 30      # 別窓で音を出す\n"
        "  audiolens_procloop --list                          # その PID を調べる\n"
        "  audiolens_procloop --pid <PID> --auto-mute 5 --duration 12\n"
        "\n"
        "判定のしかた(手動):\n"
        "  1. 音楽などを再生しているアプリの PID を --list で調べる\n"
        "  2. --pid <PID> で本ツールを動かし、RMS が動いていることを確認する\n"
        "  3. 音量ミキサーでそのアプリを消音する\n"
        "  4. 「消音=ON」に変わったあとも RMS が動き続けるかを見る\n"
        "\n"
        "     動き続ける → 案 E 成立。消音して取り込み、補正音だけを鳴らせる\n"
        "     無音になる → 案 E 不成立。仮想デバイスが要る\n");
}

std::string utf8(const std::wstring& text) {
    if (text.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()),
                                        nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text.c_str(), static_cast<int>(text.size()), out.data(), n,
                          nullptr, nullptr);
    return out;
}

std::string processName(DWORD pid) {
    if (pid == 0) return "(システム)";
    const UniqueHandle process(
        ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
    if (!process) return "(取得不可)";

    wchar_t path[MAX_PATH] = {};
    DWORD size = MAX_PATH;
    if (!::QueryFullProcessImageNameW(process.get(), 0, path, &size)) return "(取得不可)";

    const std::wstring full(path, size);
    const std::size_t slash = full.find_last_of(L'\\');
    return utf8(slash == std::wstring::npos ? full : full.substr(slash + 1));
}

double toDbfs(double amplitude) {
    return amplitude > 1e-9 ? 20.0 * std::log10(amplitude) : -120.0;
}

// --- audio session state -----------------------------------------------------

struct SessionState {
    bool found = false;
    bool muted = false;
    float volume = 0.0f;
    AudioSessionState state = AudioSessionStateInactive;
    std::string name;
};

/// Reads back what the volume mixer shows for a process, so the captured level
/// can be read against the mute the user just toggled.
class SessionInspector {
public:
    bool open(std::string* error) {
        ComPtr<IMMDeviceEnumerator> enumerator;
        HRESULT hr = ::CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                        IID_PPV_ARGS(&enumerator));
        if (FAILED(hr)) {
            *error = std::format("MMDeviceEnumerator の生成に失敗 (0x{:08X})",
                                 static_cast<unsigned>(hr));
            return false;
        }
        ComPtr<IMMDevice> device;
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
        if (FAILED(hr)) {
            *error = std::format("既定の再生デバイスを取得できません (0x{:08X})",
                                 static_cast<unsigned>(hr));
            return false;
        }
        hr = device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager_);
        if (FAILED(hr)) {
            *error = std::format("IAudioSessionManager2 を取得できません (0x{:08X})",
                                 static_cast<unsigned>(hr));
            return false;
        }
        return true;
    }

    /// The enumerator is a snapshot, so it is rebuilt on every call — sessions
    /// appearing and disappearing is exactly what we are watching for.
    std::vector<std::pair<DWORD, SessionState>> snapshot() {
        std::vector<std::pair<DWORD, SessionState>> result;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager_->GetSessionEnumerator(&sessions))) return result;

        int count = 0;
        if (FAILED(sessions->GetCount(&count))) return result;

        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, &control))) continue;

            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;

            DWORD pid = 0;
            if (FAILED(control2->GetProcessId(&pid))) continue;

            SessionState state;
            state.found = true;
            state.name = processName(pid);
            control2->GetState(&state.state);

            ComPtr<ISimpleAudioVolume> volume;
            if (SUCCEEDED(control2.As(&volume))) {
                BOOL muted = FALSE;
                volume->GetMute(&muted);
                state.muted = muted != FALSE;
                volume->GetMasterVolume(&state.volume);
            }
            result.emplace_back(pid, state);
        }
        return result;
    }

    SessionState find(DWORD pid) {
        for (auto& [sessionPid, state] : snapshot()) {
            if (sessionPid == pid) return state;
        }
        return {};
    }

    /// The volume mixer's mute is this same call, so driving it here reproduces
    /// exactly what a user would do by hand — and lets the experiment run
    /// unattended instead of depending on someone clicking at the right moment.
    bool setMute(DWORD pid, bool mute) {
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(manager_->GetSessionEnumerator(&sessions))) return false;

        int count = 0;
        if (FAILED(sessions->GetCount(&count))) return false;

        for (int i = 0; i < count; ++i) {
            ComPtr<IAudioSessionControl> control;
            if (FAILED(sessions->GetSession(i, &control))) continue;
            ComPtr<IAudioSessionControl2> control2;
            if (FAILED(control.As(&control2))) continue;

            DWORD sessionPid = 0;
            if (FAILED(control2->GetProcessId(&sessionPid)) || sessionPid != pid) continue;

            ComPtr<ISimpleAudioVolume> volume;
            if (FAILED(control2.As(&volume))) continue;
            return SUCCEEDED(volume->SetMute(mute ? TRUE : FALSE, nullptr));
        }
        return false;
    }

private:
    ComPtr<IAudioSessionManager2> manager_;
};

// --- process loopback activation ---------------------------------------------

/// ActivateAudioInterfaceAsync reports through a callback rather than returning
/// the interface, so the result has to be parked somewhere and waited on.
///
/// FtmBase is not optional. The callback is invoked on a Media Foundation work
/// queue thread, and without the free-threaded marshaler the object cannot be
/// handed across apartments — ActivateAudioInterfaceAsync then refuses the call
/// outright with E_ILLEGAL_METHOD_CALL, before any audio is involved.
class ActivationHandler
    : public Microsoft::WRL::RuntimeClass<
          Microsoft::WRL::RuntimeClassFlags<Microsoft::WRL::ClassicCom>, Microsoft::WRL::FtmBase,
          IActivateAudioInterfaceCompletionHandler> {
public:
    ActivationHandler() : done_(::CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    HANDLE event() const noexcept { return done_.get(); }
    HRESULT result() const noexcept { return result_; }
    ComPtr<IAudioClient> client() const { return client_; }

    HRESULT STDMETHODCALLTYPE
    ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activateHr = S_OK;
        ComPtr<IUnknown> unknown;
        HRESULT hr = operation->GetActivateResult(&activateHr, &unknown);
        if (SUCCEEDED(hr)) hr = activateHr;
        if (SUCCEEDED(hr)) hr = unknown.As(&client_);
        result_ = hr;
        ::SetEvent(done_.get());
        return S_OK;
    }

private:
    UniqueHandle done_;
    HRESULT result_ = E_FAIL;
    ComPtr<IAudioClient> client_;
};

struct CaptureFormat {
    const char* label;
    WORD formatTag;
    WORD bits;
    DWORD rate;
};

WAVEFORMATEX makeFormat(const CaptureFormat& spec) {
    WAVEFORMATEX format{};
    format.wFormatTag = spec.formatTag;
    format.nChannels = 2;
    format.nSamplesPerSec = spec.rate;
    format.wBitsPerSample = spec.bits;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * format.wBitsPerSample / 8);
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    return format;
}

/// Activates the pseudo-device and initialises it. `client` is only valid when
/// this returns true.
bool activateProcessLoopback(DWORD targetPid, PROCESS_LOOPBACK_MODE mode,
                             const WAVEFORMATEX& format, ComPtr<IAudioClient>* client,
                             std::string* error) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = targetPid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = mode;

    PROPVARIANT activateParams{};
    activateParams.vt = VT_BLOB;
    activateParams.blob.cbSize = sizeof(params);
    activateParams.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    const ComPtr<ActivationHandler> handler = Microsoft::WRL::Make<ActivationHandler>();
    if (!handler) {
        *error = "ハンドラーを生成できませんでした。";
        return false;
    }

    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ::ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK,
                                               __uuidof(IAudioClient), &activateParams,
                                               handler.Get(), &operation);
    if (FAILED(hr)) {
        *error = std::format("ActivateAudioInterfaceAsync に失敗 (0x{:08X})",
                             static_cast<unsigned>(hr));
        return false;
    }

    if (::WaitForSingleObject(handler->event(), 3000) != WAIT_OBJECT_0) {
        *error = "アクティブ化が 3 秒以内に完了しませんでした。";
        return false;
    }
    if (FAILED(handler->result())) {
        *error = std::format("アクティブ化が失敗を返しました (0x{:08X})",
                             static_cast<unsigned>(handler->result()));
        return false;
    }

    const ComPtr<IAudioClient> activated = handler->client();

    // The pseudo-device has no mix format to ask for, so the format is asserted
    // rather than negotiated. AUTOCONVERTPCM lets the OS resample the sources
    // into it. Buffer duration and periodicity must both be zero here: the
    // pseudo-device picks its own, and asking for a specific one fails.
    hr = activated->Initialize(AUDCLNT_SHAREMODE_SHARED,
                               AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                                   AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                               0, 0, &format, nullptr);
    if (FAILED(hr)) {
        *error = std::format("Initialize に失敗 (0x{:08X})", static_cast<unsigned>(hr));
        return false;
    }

    *client = activated;
    return true;
}

// --- level measurement -------------------------------------------------------

struct LevelAccumulator {
    double sumSquares = 0.0;
    double peak = 0.0;
    std::uint64_t frames = 0;
    std::uint64_t silentFrames = 0;

    void addSamples(const void* data, std::uint32_t frames_, const WAVEFORMATEX& format,
                    bool silentFlag) {
        frames += frames_;
        if (silentFlag || data == nullptr) {
            silentFrames += frames_;
            return;
        }
        const std::uint32_t samples = frames_ * format.nChannels;
        for (std::uint32_t i = 0; i < samples; ++i) {
            double value = 0.0;
            if (format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT) {
                value = static_cast<const float*>(data)[i];
            } else {
                value = static_cast<const std::int16_t*>(data)[i] / 32768.0;
            }
            sumSquares += value * value;
            peak = std::max(peak, std::abs(value));
        }
    }

    double rms(const WAVEFORMATEX& format) const {
        const double samples = static_cast<double>(frames) * format.nChannels;
        return samples > 0.0 ? std::sqrt(sumSquares / samples) : 0.0;
    }

    void reset() { *this = LevelAccumulator{}; }
};

int runList() {
    SessionInspector inspector;
    std::string error;
    if (!inspector.open(&error)) {
        std::printf("エラー: %s\n", error.c_str());
        return 1;
    }

    std::puts("既定の再生デバイスのオーディオセッション:\n");
    std::puts("   PID  状態      消音  音量   プロセス");
    std::puts("  ----  --------  ----  -----  --------------------------");

    auto sessions = inspector.snapshot();
    if (sessions.empty()) {
        std::puts("  (セッションがありません)");
        return 0;
    }
    for (const auto& [pid, state] : sessions) {
        const char* stateText = state.state == AudioSessionStateActive     ? "再生中"
                                : state.state == AudioSessionStateInactive ? "停止中"
                                                                           : "期限切れ";
        std::printf("  %4lu  %-8s  %-4s  %4.0f%%  %s\n", pid, stateText,
                    state.muted ? "ON" : "OFF", state.volume * 100.0f, state.name.c_str());
    }
    std::puts("\n「再生中」のプロセスの PID を --pid に渡してください。");
    return 0;
}

int runCapture(DWORD targetPid, PROCESS_LOOPBACK_MODE mode, double durationSeconds,
               double autoMuteSeconds) {
    // The declarations are gated on NTDDI_WIN10_FE (build 20348). Whether that
    // gate also applies at runtime is the second open question, so the build is
    // reported for whatever machine this runs on.
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (HMODULE ntdll = ::GetModuleHandleW(L"ntdll.dll")) {
        using RtlGetVersionFn = LONG(WINAPI*)(OSVERSIONINFOEXW*);
        if (auto fn = reinterpret_cast<RtlGetVersionFn>(
                reinterpret_cast<void*>(::GetProcAddress(ntdll, "RtlGetVersion")))) {
            fn(&version);
        }
    }
    std::printf("OS ビルド: %lu.%lu (案 E の宣言は 20348 以降で有効)\n\n", version.dwBuildNumber,
                version.dwMinorVersion);

    // No mix format to query, so candidates are tried until one initialises.
    // Which one wins is itself a finding worth recording.
    static const CaptureFormat kCandidates[] = {
        {"float32 48000 Hz 2ch", WAVE_FORMAT_IEEE_FLOAT, 32, 48000},
        {"PCM16 48000 Hz 2ch", WAVE_FORMAT_PCM, 16, 48000},
        {"PCM16 44100 Hz 2ch", WAVE_FORMAT_PCM, 16, 44100},
    };

    ComPtr<IAudioClient> client;
    WAVEFORMATEX format{};
    std::string lastError;
    for (const CaptureFormat& candidate : kCandidates) {
        const WAVEFORMATEX attempt = makeFormat(candidate);
        std::string error;
        if (activateProcessLoopback(targetPid, mode, attempt, &client, &error)) {
            format = attempt;
            std::printf("取り込み形式: %s\n", candidate.label);
            break;
        }
        std::printf("  %s: %s\n", candidate.label, error.c_str());
        lastError = error;
        client.Reset();
    }
    if (!client) {
        std::printf("\nどの形式でも取り込みを開始できませんでした。\n最後のエラー: %s\n",
                    lastError.c_str());
        std::puts("\nこれ自体が結論になりえます。この OS でプロセスループバックが");
        std::puts("使えないなら、案 E はこの環境では成立しません。");
        return 1;
    }

    UniqueHandle bufferEvent(::CreateEventW(nullptr, FALSE, FALSE, nullptr));
    if (FAILED(client->SetEventHandle(bufferEvent.get()))) {
        std::puts("SetEventHandle に失敗しました。");
        return 1;
    }

    ComPtr<IAudioCaptureClient> capture;
    if (FAILED(client->GetService(IID_PPV_ARGS(&capture)))) {
        std::puts("IAudioCaptureClient を取得できませんでした。");
        return 1;
    }
    if (FAILED(client->Start())) {
        std::puts("Start に失敗しました。");
        return 1;
    }

    SessionInspector inspector;
    std::string inspectorError;
    const bool haveSessions = inspector.open(&inspectorError);
    if (!haveSessions) {
        std::printf("警告: セッション状態を読めません (%s)\n", inspectorError.c_str());
    }

    std::puts("");
    std::puts("音量ミキサーで対象アプリの消音を切り替えて、下の RMS を見てください。");
    std::puts("消音してもRMS が動き続けるなら案 E は成立します。Ctrl+C で終了。");
    std::puts("");

    // With --auto-mute the tool drives the mute itself, so the two halves are
    // measured under identical conditions and the comparison does not depend on
    // anyone's timing.
    const bool autoMute = autoMuteSeconds > 0.0 &&
                          mode == PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE && haveSessions;
    bool originalMute = false;
    if (autoMute) {
        originalMute = inspector.find(targetPid).muted;
        inspector.setMute(targetPid, false);
        std::printf("自動測定: %.0f 秒ごとに消音を切り替えます。\n\n", autoMuteSeconds);
    }

    // Audio already in flight keeps arriving for a moment after the mute takes
    // hold — endpoint buffers do not empty instantly. Counting that tail as
    // "captured while muted" inverts the verdict, so the window right after the
    // switch is measured but excluded from the judgement.
    constexpr double kSettleSeconds = 1.5;

    const auto started = std::chrono::steady_clock::now();
    auto lastReport = started;
    LevelAccumulator display;
    LevelAccumulator whileAudible;
    LevelAccumulator whileMuted;
    bool muteApplied = false;
    double muteAppliedAt = 0.0;
    bool sawAudioWhileMuted = false;
    bool sawMuted = false;

    while (!g_stopRequested.load(std::memory_order_acquire)) {
        ::WaitForSingleObject(bufferEvent.get(), 200);

        const auto now = std::chrono::steady_clock::now();
        const double elapsed = std::chrono::duration<double>(now - started).count();
        const bool settled = !muteApplied || elapsed >= muteAppliedAt + kSettleSeconds;

        for (;;) {
            BYTE* data = nullptr;
            std::uint32_t frames = 0;
            DWORD flags = 0;
            const HRESULT hr = capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (hr == AUDCLNT_S_BUFFER_EMPTY || FAILED(hr)) break;

            const bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
            display.addSamples(data, frames, format, silent);
            if (!muteApplied) {
                whileAudible.addSamples(data, frames, format, silent);
            } else if (settled) {
                whileMuted.addSamples(data, frames, format, silent);
            }
            capture->ReleaseBuffer(frames);
        }

        if (autoMute && !muteApplied && elapsed >= autoMuteSeconds) {
            muteApplied = inspector.setMute(targetPid, true);
            muteAppliedAt = elapsed;
            std::printf("  --- 消音しました (%.1f 秒の整定待ちを挟みます) ---\n", kSettleSeconds);
            if (!muteApplied) std::puts("  (消音に失敗しました)");
        }

        if (now - lastReport < std::chrono::milliseconds(500)) {
            if (durationSeconds > 0.0 && elapsed >= durationSeconds) break;
            continue;
        }
        lastReport = now;

        const double rms = display.rms(format);
        const double silentRatio =
            display.frames > 0 ? static_cast<double>(display.silentFrames) / display.frames : 1.0;

        std::string sessionText = "  (セッション情報なし)";
        if (haveSessions && mode == PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE) {
            const SessionState state = inspector.find(targetPid);
            if (state.found) {
                sessionText = std::format("  対象: {}  消音={}{}", state.name,
                                          state.muted ? "ON " : "OFF",
                                          state.muted && !settled ? "  (整定待ち)" : "");
                if (state.muted && settled) {
                    sawMuted = true;
                    if (rms > 1e-5) sawAudioWhileMuted = true;
                }
            } else {
                sessionText = "  対象: セッションが見つかりません";
            }
        }

        std::printf("[%5.1fs] RMS %6.1f dBFS  ピーク %6.1f dBFS  無音 %3.0f%%%s\n", elapsed,
                    toDbfs(rms), toDbfs(display.peak), silentRatio * 100.0, sessionText.c_str());
        display.reset();

        if (durationSeconds > 0.0 && elapsed >= durationSeconds) break;
    }

    client->Stop();
    if (autoMute) inspector.setMute(targetPid, originalMute);  // leave it as we found it

    std::puts("");
    std::puts("--- 判定 ---");
    if (mode != PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE) {
        std::puts("除外モードでは対象セッションの消音を追えません。");
        std::puts("判定には --pid <PID> を使ってください。");
        return 0;
    }

    const double audibleRms = whileAudible.rms(format);
    if (audibleRms <= 1e-5) {
        std::puts("消音していない間も無音でした。対象プロセスが実際に音を出していません。");
        std::puts("音を鳴らしている状態で測り直してください。");
        return 1;
    }
    std::printf("消音前: RMS %.1f dBFS\n", toDbfs(audibleRms));

    if (!sawMuted) {
        std::puts("消音された状態を観測していません。--auto-mute を使うか、");
        std::puts("音量ミキサーで対象アプリを消音してください。");
        return 1;
    }
    std::printf("消音後: RMS %.1f dBFS\n\n", toDbfs(whileMuted.rms(format)));

    if (sawAudioWhileMuted) {
        std::puts("消音中も音が取り込めました。案 E は成立します。");
        std::puts("消音したうえで補正音だけを鳴らす構成が組めます。");
    } else {
        std::puts("消音すると取り込みも無音になりました。案 E は成立しません。");
        std::puts("仮想デバイス(自作ドライバまたは他社ドライバ)が必要です。");
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    ::SetConsoleOutputCP(CP_UTF8);
    ::SetConsoleCtrlHandler(consoleHandler, TRUE);

    bool list = false;
    bool excludeSelf = false;
    DWORD pid = 0;
    double duration = 0.0;
    double autoMute = 0.0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s に値がありません。\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else if (arg == "--list") {
            list = true;
        } else if (arg == "--exclude-self") {
            excludeSelf = true;
        } else if (arg == "--pid") {
            pid = static_cast<DWORD>(std::strtoul(next("--pid"), nullptr, 10));
        } else if (arg == "--duration") {
            duration = std::strtod(next("--duration"), nullptr);
        } else if (arg == "--auto-mute") {
            autoMute = std::strtod(next("--auto-mute"), nullptr);
        } else {
            std::printf("不明な引数: %s\n\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    if (!list && !excludeSelf && pid == 0) {
        printUsage();
        return 2;
    }

    const ComApartment com;

    if (list) return runList();
    if (excludeSelf) {
        std::printf("自分自身 (PID %lu) を除いたすべての再生音を取り込みます。\n\n",
                    ::GetCurrentProcessId());
        return runCapture(::GetCurrentProcessId(), PROCESS_LOOPBACK_MODE_EXCLUDE_TARGET_PROCESS_TREE,
                          duration, 0.0);
    }
    std::printf("PID %lu の再生音を取り込みます。\n\n", pid);
    return runCapture(pid, PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE, duration, autoMute);
}
