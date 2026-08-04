#include "test_support.h"

#include "audiofile/wav.h"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <numbers>
#include <string>

using audiolens::AudioBuffer;
using audiolens::readWav;
using audiolens::WavSampleFormat;
using audiolens::writeWav;

namespace {

/// Temporary file that removes itself, so a failing assertion cannot leave
/// litter behind in the build tree.
class TempWav {
public:
    explicit TempWav(const char* name) {
        path_ = (std::filesystem::temp_directory_path() / name).string();
    }
    ~TempWav() {
        std::error_code ignored;
        std::filesystem::remove(path_, ignored);
    }
    const std::string& path() const { return path_; }

private:
    std::string path_;
};

AudioBuffer makeTestBuffer(std::uint32_t channels, std::uint32_t sampleRate, std::size_t frames) {
    AudioBuffer buffer;
    buffer.sampleRate = sampleRate;
    buffer.channels = channels;
    buffer.samples.resize(frames * channels);
    for (std::size_t f = 0; f < frames; ++f) {
        for (std::uint32_t c = 0; c < channels; ++c) {
            const double phase = 2.0 * std::numbers::pi * 440.0 * static_cast<double>(f) / sampleRate;
            buffer.samples[f * channels + c] =
                static_cast<float>(std::sin(phase + c) * 0.5);
        }
    }
    return buffer;
}

}  // namespace

AL_TEST(Wav_round_trips_through_float32) {
    TempWav file("audiolens_test_f32.wav");
    const AudioBuffer original = makeTestBuffer(2, 48000, 1000);

    std::string error;
    CHECK(writeWav(file.path(), original, WavSampleFormat::Float32, &error));

    AudioBuffer loaded;
    CHECK(readWav(file.path(), &loaded, &error));
    CHECK_EQ(loaded.sampleRate, std::uint32_t{48000});
    CHECK_EQ(loaded.channels, std::uint32_t{2});
    CHECK_EQ(loaded.frames(), std::size_t{1000});

    // Float32 is lossless, so this must be exact.
    for (std::size_t i = 0; i < original.samples.size(); ++i) {
        CHECK_NEAR(loaded.samples[i], original.samples[i], 1e-9);
    }
}

AL_TEST(Wav_round_trips_through_int16) {
    TempWav file("audiolens_test_i16.wav");
    const AudioBuffer original = makeTestBuffer(2, 44100, 500);

    std::string error;
    CHECK(writeWav(file.path(), original, WavSampleFormat::Int16, &error));

    AudioBuffer loaded;
    CHECK(readWav(file.path(), &loaded, &error));
    CHECK_EQ(loaded.sampleRate, std::uint32_t{44100});

    // Two LSB. One comes from rounding to 16 bits; the other from the
    // conventional asymmetry of scaling by 32767 on the way out and 32768 on
    // the way back, which no round trip can avoid.
    constexpr double kTolerance = 2.0 / 32767.0;
    for (std::size_t i = 0; i < original.samples.size(); ++i) {
        CHECK_NEAR(loaded.samples[i], original.samples[i], kTolerance);
    }
}

AL_TEST(Wav_round_trips_through_int24) {
    TempWav file("audiolens_test_i24.wav");
    const AudioBuffer original = makeTestBuffer(1, 48000, 500);

    std::string error;
    CHECK(writeWav(file.path(), original, WavSampleFormat::Int24, &error));

    AudioBuffer loaded;
    CHECK(readWav(file.path(), &loaded, &error));
    CHECK_EQ(loaded.channels, std::uint32_t{1});
    for (std::size_t i = 0; i < original.samples.size(); ++i) {
        CHECK_NEAR(loaded.samples[i], original.samples[i], 1e-6);
    }
}

AL_TEST(Wav_clamps_out_of_range_samples_on_write) {
    TempWav file("audiolens_test_clamp.wav");
    AudioBuffer original;
    original.sampleRate = 48000;
    original.channels = 1;
    original.samples = {2.0f, -2.0f, 0.0f};

    std::string error;
    CHECK(writeWav(file.path(), original, WavSampleFormat::Int16, &error));

    AudioBuffer loaded;
    CHECK(readWav(file.path(), &loaded, &error));
    CHECK_NEAR(loaded.samples[0], 1.0, 1e-4);
    CHECK_NEAR(loaded.samples[1], -1.0, 1e-4);
    CHECK_NEAR(loaded.samples[2], 0.0, 1e-9);
}

AL_TEST(Wav_reports_an_error_for_a_missing_file) {
    AudioBuffer loaded;
    std::string error;
    CHECK(!readWav("this_file_does_not_exist_12345.wav", &loaded, &error));
    CHECK(!error.empty());
}

AL_TEST(Wav_rejects_a_file_that_is_not_riff) {
    TempWav file("audiolens_test_bogus.wav");
    std::FILE* raw = nullptr;
    CHECK(::fopen_s(&raw, file.path().c_str(), "wb") == 0);
    if (raw != nullptr) {
        std::fputs("this is definitely not a wave file", raw);
        std::fclose(raw);
    }

    AudioBuffer loaded;
    std::string error;
    CHECK(!readWav(file.path(), &loaded, &error));
    CHECK(!error.empty());
}

AL_TEST(Wav_handles_an_empty_buffer) {
    TempWav file("audiolens_test_empty.wav");
    AudioBuffer original;
    original.sampleRate = 48000;
    original.channels = 2;

    std::string error;
    CHECK(writeWav(file.path(), original, WavSampleFormat::Float32, &error));

    AudioBuffer loaded;
    CHECK(readWav(file.path(), &loaded, &error));
    CHECK_EQ(loaded.frames(), std::size_t{0});
}
