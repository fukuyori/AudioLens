#include "engine/audio_format.h"

#include <algorithm>
#include <cstring>
#include <format>

namespace audiolens {
namespace {

constexpr float kInt16Scale = 1.0f / 32768.0f;
constexpr float kInt24Scale = 1.0f / 8388608.0f;
constexpr float kInt32Scale = 1.0f / 2147483648.0f;

float clamp1(float v) noexcept { return std::clamp(v, -1.0f, 1.0f); }

/// Reads one sample of the given encoding from `p` and normalizes it to [-1, 1].
float readSample(const std::uint8_t* p, SampleType type) noexcept {
    switch (type) {
        case SampleType::Float32: {
            float v;
            std::memcpy(&v, p, sizeof(float));
            return v;
        }
        case SampleType::Int16: {
            std::int16_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) * kInt16Scale;
        }
        case SampleType::Int24: {
            const std::int32_t v =
                (static_cast<std::int32_t>(static_cast<std::int8_t>(p[2])) << 16) |
                (static_cast<std::int32_t>(p[1]) << 8) | static_cast<std::int32_t>(p[0]);
            return static_cast<float>(v) * kInt24Scale;
        }
        case SampleType::Int32: {
            std::int32_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) * kInt32Scale;
        }
        case SampleType::Unknown: break;
    }
    return 0.0f;
}

/// Quantises to the device's integer format. Rounds rather than truncates:
/// truncation costs a full LSB of accuracy and biases every sample toward zero.
void writeSample(std::uint8_t* p, SampleType type, float value) noexcept {
    switch (type) {
        case SampleType::Float32: {
            const float v = clamp1(value);
            std::memcpy(p, &v, sizeof(float));
            return;
        }
        case SampleType::Int16: {
            const auto v = static_cast<std::int16_t>(std::lround(clamp1(value) * 32767.0));
            std::memcpy(p, &v, sizeof(v));
            return;
        }
        case SampleType::Int24: {
            const auto v = static_cast<std::int32_t>(std::lround(clamp1(value) * 8388607.0));
            p[0] = static_cast<std::uint8_t>(v & 0xFF);
            p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
            p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
            return;
        }
        case SampleType::Int32: {
            const auto v = static_cast<std::int32_t>(std::llround(
                static_cast<double>(clamp1(value)) * 2147483520.0));
            std::memcpy(p, &v, sizeof(v));
            return;
        }
        case SampleType::Unknown: return;
    }
}

std::uint32_t bytesPerSample(SampleType type) noexcept {
    switch (type) {
        case SampleType::Float32: return 4;
        case SampleType::Int16:   return 2;
        case SampleType::Int24:   return 3;
        case SampleType::Int32:   return 4;
        case SampleType::Unknown: return 0;
    }
    return 0;
}

const char* sampleTypeName(SampleType type) noexcept {
    switch (type) {
        case SampleType::Float32: return "float32";
        case SampleType::Int16:   return "int16";
        case SampleType::Int24:   return "int24";
        case SampleType::Int32:   return "int32";
        case SampleType::Unknown: return "unknown";
    }
    return "unknown";
}

}  // namespace

std::string StreamFormat::describe() const {
    return std::format("{} Hz, {} ch, {}", sampleRate, channels, sampleTypeName(sampleType));
}

StreamFormat describeWaveFormat(const WAVEFORMATEX* wfx) {
    StreamFormat format;
    if (wfx == nullptr) {
        return format;
    }

    format.sampleRate = wfx->nSamplesPerSec;
    format.channels = wfx->nChannels;
    format.bytesPerFrame = wfx->nBlockAlign;

    WORD tag = wfx->wFormatTag;
    WORD bits = wfx->wBitsPerSample;
    WORD validBits = bits;

    if (tag == WAVE_FORMAT_EXTENSIBLE && wfx->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wfx);
        validBits = ext->Samples.wValidBitsPerSample;
        if (::IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT)) {
            tag = WAVE_FORMAT_IEEE_FLOAT;
        } else if (::IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM)) {
            tag = WAVE_FORMAT_PCM;
        } else {
            tag = 0;  // Compressed or otherwise unsupported subformat.
        }
    }

    if (tag == WAVE_FORMAT_IEEE_FLOAT && bits == 32) {
        format.sampleType = SampleType::Float32;
    } else if (tag == WAVE_FORMAT_PCM) {
        switch (bits) {
            case 16: format.sampleType = SampleType::Int16; break;
            case 24: format.sampleType = SampleType::Int24; break;
            // 32-bit containers carrying 24 valid bits still decode correctly as
            // int32: the low bits are simply zero.
            case 32: format.sampleType = (validBits == 24 || validBits == 32) ? SampleType::Int32
                                                                             : SampleType::Unknown;
                     break;
            default: break;
        }
    }

    return format;
}

void convertToFloat(const void* src, const StreamFormat& srcFormat, float* dst,
                    std::uint32_t dstChannels, std::size_t frames) noexcept {
    const auto* in = static_cast<const std::uint8_t*>(src);
    const std::uint32_t srcChannels = srcFormat.channels;
    const std::uint32_t sampleBytes = bytesPerSample(srcFormat.sampleType);
    const std::uint32_t srcStride = srcFormat.bytesPerFrame != 0
                                        ? srcFormat.bytesPerFrame
                                        : srcChannels * sampleBytes;

    // Fast path: identical layout and float32 source.
    if (srcFormat.sampleType == SampleType::Float32 && srcChannels == dstChannels &&
        srcStride == srcChannels * sizeof(float)) {
        std::memcpy(dst, in, frames * srcChannels * sizeof(float));
        return;
    }

    for (std::size_t f = 0; f < frames; ++f) {
        const std::uint8_t* frame = in + f * srcStride;
        float* out = dst + f * dstChannels;

        if (srcChannels == 1) {
            const float v = readSample(frame, srcFormat.sampleType);
            for (std::uint32_t c = 0; c < dstChannels; ++c) {
                out[c] = v;
            }
        } else if (dstChannels == 1) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < srcChannels; ++c) {
                sum += readSample(frame + c * sampleBytes, srcFormat.sampleType);
            }
            out[0] = sum / static_cast<float>(srcChannels);
        } else {
            const std::uint32_t common = std::min(srcChannels, dstChannels);
            for (std::uint32_t c = 0; c < common; ++c) {
                out[c] = readSample(frame + c * sampleBytes, srcFormat.sampleType);
            }
            for (std::uint32_t c = common; c < dstChannels; ++c) {
                out[c] = 0.0f;
            }
        }
    }
}

void convertFromFloat(const float* src, std::uint32_t srcChannels, void* dst,
                      const StreamFormat& dstFormat, std::size_t frames) noexcept {
    auto* out = static_cast<std::uint8_t*>(dst);
    const std::uint32_t dstChannels = dstFormat.channels;
    const std::uint32_t sampleBytes = bytesPerSample(dstFormat.sampleType);
    const std::uint32_t dstStride = dstFormat.bytesPerFrame != 0
                                        ? dstFormat.bytesPerFrame
                                        : dstChannels * sampleBytes;

    for (std::size_t f = 0; f < frames; ++f) {
        const float* in = src + f * srcChannels;
        std::uint8_t* frame = out + f * dstStride;

        if (srcChannels == 1) {
            for (std::uint32_t c = 0; c < dstChannels; ++c) {
                writeSample(frame + c * sampleBytes, dstFormat.sampleType, in[0]);
            }
        } else if (dstChannels == 1) {
            float sum = 0.0f;
            for (std::uint32_t c = 0; c < srcChannels; ++c) {
                sum += in[c];
            }
            writeSample(frame, dstFormat.sampleType, sum / static_cast<float>(srcChannels));
        } else {
            const std::uint32_t common = std::min(srcChannels, dstChannels);
            for (std::uint32_t c = 0; c < common; ++c) {
                writeSample(frame + c * sampleBytes, dstFormat.sampleType, in[c]);
            }
            for (std::uint32_t c = common; c < dstChannels; ++c) {
                writeSample(frame + c * sampleBytes, dstFormat.sampleType, 0.0f);
            }
        }
    }
}

}  // namespace audiolens
