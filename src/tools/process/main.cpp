// Offline verification tool for the DSP chain.
//
// Runs a WAV file through a preset and reports what the processing did to it in
// the terms the requirements are written in: integrated loudness, loudness
// range (how far apart the quiet and loud passages are), peak, and the level in
// each of the bands the presets act on.
//
//     audiolens_process --input movie.wav --output out.wav --preset movie
//     audiolens_process --input in.wav --preset conversation --clarity 100

#include "analysis/loudness.h"
#include "audiofile/wav.h"
#include "common/denormals.h"
#include "core/preset.h"
#include "dsp/dsp_chain.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

using namespace audiolens;

namespace {

struct BandProbe {
    const char* label;
    double centreHz;
    double q;
};

// Chosen to line up with what each slider claims to do: the first two bands are
// where「低音」acts, the middle two where「声の明瞭さ」does, the last is air.
constexpr BandProbe kBands[] = {
    {"  60 Hz", 60.0, 1.4},   {" 150 Hz", 150.0, 1.4},  {" 400 Hz", 400.0, 1.4},
    {"1.0 kHz", 1000.0, 1.4}, {"2.5 kHz", 2500.0, 1.4}, {"4.0 kHz", 4000.0, 1.4},
    {"8.0 kHz", 8000.0, 1.4},
};

void printUsage() {
    std::puts(
        "AudioLens オフライン処理・測定ツール\n"
        "\n"
        "使い方:\n"
        "  audiolens_process --input <in.wav> [--output <out.wav>] [オプション]\n"
        "\n"
        "オプション:\n"
        "  --input <path>     入力 WAV(必須)\n"
        "  --output <path>    処理結果の書き出し先。省略時は測定のみ\n"
        "  --preset <id>      プリセット id(既定 standard)\n"
        "  --bass <0-100>     「低音」。プリセットの値を上書き\n"
        "  --clarity <0-100>  「声の明瞭さ」\n"
        "  --leveling <0-100> 「音量差」\n"
        "  --list-presets     プリセット一覧を表示して終了\n");
}

void printPresetList() {
    std::puts("プリセット一覧:\n");
    for (const Preset& preset : builtinPresets()) {
        std::printf("  %-14s %-8s %s\n", preset.id.c_str(), preset.name.c_str(),
                    preset.description.c_str());
        std::printf("  %-14s   低音 %d / 声の明瞭さ %d / 音量差 %d\n", "", preset.sliders.bass,
                    preset.sliders.clarity, preset.sliders.leveling);
    }
}

std::string formatLufs(double value) {
    return value > -1e8 ? std::to_string(value) : std::string("-inf");
}

void printMeasurement(const char* label, const analysis::LoudnessResult& m) {
    std::printf("  %-10s  統合 %7.2f LUFS   音量差(LRA) %6.2f LU   最大瞬時 %7.2f LUFS   ピーク %6.2f dBFS\n",
                label, m.integratedLufs, m.loudnessRangeLu, m.maxMomentaryLufs, m.samplePeakDbfs);
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

}  // namespace

int main(int argc, char** argv) {
    ::system("");  // Enables UTF-8 console output when run from a plain cmd shell.
    std::setvbuf(stdout, nullptr, _IOLBF, 4096);

    std::string inputPath;
    std::string outputPath;
    std::string presetId = "standard";
    int bass = -1;
    int clarity = -1;
    int leveling = -1;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto next = [&](const char* name) -> const char* {
            if (i + 1 >= argc) {
                std::printf("%s には値が必要です\n", name);
                std::exit(2);
            }
            return argv[++i];
        };

        if (arg == "--input") {
            inputPath = next("--input");
        } else if (arg == "--output") {
            outputPath = next("--output");
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
        } else if (arg == "--list-presets") {
            printPresetList();
            return 0;
        } else if (arg == "--help" || arg == "-h") {
            printUsage();
            return 0;
        } else {
            std::printf("不明な引数: %s\n\n", arg.c_str());
            printUsage();
            return 2;
        }
    }

    if (inputPath.empty()) {
        printUsage();
        return 2;
    }

    const Preset* preset = findBuiltinPreset(presetId);
    if (preset == nullptr) {
        std::printf("プリセット '%s' が見つかりません。--list-presets を参照してください。\n",
                    presetId.c_str());
        return 1;
    }

    SliderValues sliders = preset->sliders;
    if (bass >= 0) sliders.bass = bass;
    if (clarity >= 0) sliders.clarity = clarity;
    if (leveling >= 0) sliders.leveling = leveling;

    AudioBuffer input;
    std::string error;
    if (!readWav(inputPath, &input, &error)) {
        std::printf("入力を読み込めません: %s\n", error.c_str());
        return 1;
    }

    std::printf("\n入力: %s\n  %u Hz / %u ch / %.2f 秒\n", inputPath.c_str(), input.sampleRate,
                input.channels, input.durationSeconds());
    std::printf("プリセット: %s (%s)\n  低音 %d / 声の明瞭さ %d / 音量差 %d\n\n",
                preset->name.c_str(), preset->id.c_str(), sliders.bass, sliders.clarity,
                sliders.leveling);

    AudioBuffer output = input;

    {
        ScopedNoDenormals noDenormals;
        dsp::DspChain chain;
        chain.prepare(input.sampleRate, input.channels, 512);
        chain.setParameters(resolveParameters(*preset, sliders));
        // prepare() settled on whatever was published before; re-settle so the
        // measurement is not skewed by a 40 ms ramp at the head of the file.
        chain.prepare(input.sampleRate, input.channels, 512);
        chain.process(output.samples.data(), output.frames(), output.channels);

        std::printf("チェーン遅延: %.2f ms", chain.latencyMs());
        if (chain.limiterClippedSamples() > 0) {
            std::printf("   ※リミッター後のクリップ %llu サンプル",
                        static_cast<unsigned long long>(chain.limiterClippedSamples()));
        }
        std::puts("\n");
    }

    const analysis::LoudnessResult before =
        analysis::measureLoudness(input.samples, input.channels, input.sampleRate);
    const analysis::LoudnessResult after =
        analysis::measureLoudness(output.samples, output.channels, output.sampleRate);

    std::puts("== ラウドネス ==");
    printMeasurement("処理前", before);
    printMeasurement("処理後", after);
    std::printf("  %-10s  統合 %+7.2f LU      音量差(LRA) %+6.2f LU\n", "差分",
                after.integratedLufs - before.integratedLufs,
                after.loudnessRangeLu - before.loudnessRangeLu);

    std::puts("\n== 帯域レベル (dBFS) ==");
    std::puts("  帯域      処理前    処理後     差分");
    for (const BandProbe& band : kBands) {
        const double a =
            analysis::bandLevelDbfs(input.samples, input.channels, input.sampleRate, band.centreHz,
                                    band.q);
        const double b = analysis::bandLevelDbfs(output.samples, output.channels,
                                                 output.sampleRate, band.centreHz, band.q);
        std::printf("  %s  %7.2f  %7.2f  %+7.2f\n", band.label, a, b, b - a);
    }

    if (!outputPath.empty()) {
        if (!writeWav(outputPath, output, WavSampleFormat::Float32, &error)) {
            std::printf("\n出力を書き込めません: %s\n", error.c_str());
            return 1;
        }
        std::printf("\n書き出しました: %s\n", outputPath.c_str());
    }

    return 0;
}
