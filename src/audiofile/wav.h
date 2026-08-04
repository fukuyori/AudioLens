#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace audiolens {

/// Deinterleaved would be friendlier for analysis, but the DSP chain works on
/// interleaved frames, so the file layer matches it and avoids a shuffle.
struct AudioBuffer {
    std::uint32_t sampleRate = 48000;
    std::uint32_t channels = 2;
    std::vector<float> samples;  ///< Interleaved, normalized to [-1, 1].

    std::size_t frames() const {
        return channels == 0 ? 0 : samples.size() / channels;
    }
    double durationSeconds() const {
        return sampleRate == 0 ? 0.0 : static_cast<double>(frames()) / sampleRate;
    }
};

enum class WavSampleFormat { Int16, Int24, Float32 };

/// Reads a RIFF/WAVE file. Handles PCM 16/24/32-bit and IEEE float 32-bit,
/// including WAVE_FORMAT_EXTENSIBLE. Returns false and fills `error` otherwise.
bool readWav(const std::string& path, AudioBuffer* out, std::string* error);

bool writeWav(const std::string& path, const AudioBuffer& buffer, WavSampleFormat format,
              std::string* error);

}  // namespace audiolens
