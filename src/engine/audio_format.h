#pragma once

#include "common/windows_lean.h"

#include <mmreg.h>

#include <cstdint>
#include <string>

namespace audiolens {

/// The sample encodings AudioLens can read from / write to a WASAPI endpoint.
/// Shared-mode mix formats are float32 in practice; the integer cases are kept
/// so an unusual endpoint degrades to a conversion rather than a hard failure.
enum class SampleType { Unknown, Float32, Int16, Int24, Int32 };

struct StreamFormat {
    std::uint32_t sampleRate = 0;
    std::uint32_t channels = 0;
    SampleType sampleType = SampleType::Unknown;
    std::uint32_t bytesPerFrame = 0;

    bool valid() const noexcept {
        return sampleRate != 0 && channels != 0 && sampleType != SampleType::Unknown;
    }
    std::string describe() const;
};

/// Interprets a WAVEFORMATEX (including WAVEFORMATEXTENSIBLE) as a StreamFormat.
/// Returns a format with sampleType == Unknown if the encoding is unsupported.
StreamFormat describeWaveFormat(const WAVEFORMATEX* wfx);

/// Converts `frames` frames of interleaved device samples to interleaved float32,
/// remapping `src.channels` channels onto `dstChannels`.
///
/// Channel remapping is deliberately simple: mono fans out to every output
/// channel, stereo-to-mono averages, and anything else copies the leading
/// channels and zero-fills the remainder. AudioLens targets stereo playback, so
/// surround endpoints are downmixed to the leading pair rather than folded.
void convertToFloat(const void* src, const StreamFormat& srcFormat, float* dst,
                    std::uint32_t dstChannels, std::size_t frames) noexcept;

/// Converts interleaved float32 to the device format, remapping `srcChannels`
/// channels onto `dstFormat.channels`. Samples are clamped to [-1, 1].
void convertFromFloat(const float* src, std::uint32_t srcChannels, void* dst,
                      const StreamFormat& dstFormat, std::size_t frames) noexcept;

}  // namespace audiolens
