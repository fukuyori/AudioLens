#include "audiofile/wav.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>

namespace audiolens {
namespace {

constexpr std::uint16_t kFormatPcm = 0x0001;
constexpr std::uint16_t kFormatFloat = 0x0003;
constexpr std::uint16_t kFormatExtensible = 0xFFFE;

struct FileCloser {
    void operator()(std::FILE* f) const noexcept {
        if (f != nullptr) {
            std::fclose(f);
        }
    }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

FilePtr openFile(const std::string& path, const char* mode) {
    std::FILE* raw = nullptr;
    if (::fopen_s(&raw, path.c_str(), mode) != 0) {
        return nullptr;
    }
    return FilePtr(raw);
}

bool readExact(std::FILE* file, void* destination, std::size_t bytes) {
    return std::fread(destination, 1, bytes, file) == bytes;
}

bool writeExact(std::FILE* file, const void* source, std::size_t bytes) {
    return std::fwrite(source, 1, bytes, file) == bytes;
}

template <typename T>
bool readValue(std::FILE* file, T* value) {
    return readExact(file, value, sizeof(T));
}

template <typename T>
bool writeValue(std::FILE* file, T value) {
    return writeExact(file, &value, sizeof(T));
}

float decodeSample(const std::uint8_t* p, std::uint16_t formatTag, std::uint16_t bits) {
    if (formatTag == kFormatFloat) {
        float v;
        std::memcpy(&v, p, sizeof(float));
        return v;
    }
    switch (bits) {
        case 16: {
            std::int16_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(v) / 32768.0f;
        }
        case 24: {
            const std::int32_t v = (static_cast<std::int32_t>(static_cast<std::int8_t>(p[2])) << 16) |
                                   (static_cast<std::int32_t>(p[1]) << 8) |
                                   static_cast<std::int32_t>(p[0]);
            return static_cast<float>(v) / 8388608.0f;
        }
        case 32: {
            std::int32_t v;
            std::memcpy(&v, p, sizeof(v));
            return static_cast<float>(static_cast<double>(v) / 2147483648.0);
        }
        default:
            return 0.0f;
    }
}

bool fail(std::string* error, std::string message) {
    if (error != nullptr) {
        *error = std::move(message);
    }
    return false;
}

}  // namespace

bool readWav(const std::string& path, AudioBuffer* out, std::string* error) {
    FilePtr file = openFile(path, "rb");
    if (!file) {
        return fail(error, std::format("ファイルを開けません: {}", path));
    }

    char riff[4]{};
    std::uint32_t riffSize = 0;
    char wave[4]{};
    if (!readExact(file.get(), riff, 4) || !readValue(file.get(), &riffSize) ||
        !readExact(file.get(), wave, 4)) {
        return fail(error, "ヘッダーを読み取れません");
    }
    if (std::memcmp(riff, "RIFF", 4) != 0 || std::memcmp(wave, "WAVE", 4) != 0) {
        return fail(error, "RIFF/WAVE 形式ではありません");
    }

    std::uint16_t formatTag = 0;
    std::uint16_t channels = 0;
    std::uint32_t sampleRate = 0;
    std::uint16_t blockAlign = 0;
    std::uint16_t bitsPerSample = 0;
    bool haveFormat = false;

    // Walk the chunk list rather than assuming fmt is immediately followed by
    // data: real files carry LIST, fact and JUNK chunks in between.
    for (;;) {
        char chunkId[4]{};
        std::uint32_t chunkSize = 0;
        if (!readExact(file.get(), chunkId, 4) || !readValue(file.get(), &chunkSize)) {
            break;  // Clean end of file.
        }

        if (std::memcmp(chunkId, "fmt ", 4) == 0) {
            if (chunkSize < 16) {
                return fail(error, "fmt チャンクが不正です");
            }
            std::uint32_t bytesPerSecond = 0;
            if (!readValue(file.get(), &formatTag) || !readValue(file.get(), &channels) ||
                !readValue(file.get(), &sampleRate) || !readValue(file.get(), &bytesPerSecond) ||
                !readValue(file.get(), &blockAlign) || !readValue(file.get(), &bitsPerSample)) {
                return fail(error, "fmt チャンクを読み取れません");
            }

            if (formatTag == kFormatExtensible && chunkSize >= 40) {
                std::uint16_t extensionSize = 0;
                std::uint16_t validBits = 0;
                std::uint32_t channelMask = 0;
                std::uint8_t subFormat[16]{};
                if (!readValue(file.get(), &extensionSize) || !readValue(file.get(), &validBits) ||
                    !readValue(file.get(), &channelMask) || !readExact(file.get(), subFormat, 16)) {
                    return fail(error, "拡張 fmt チャンクを読み取れません");
                }
                // The first two bytes of the subformat GUID carry the tag the
                // non-extensible header would have used.
                std::memcpy(&formatTag, subFormat, sizeof(std::uint16_t));
                if (std::fseek(file.get(), static_cast<long>(chunkSize) - 40, SEEK_CUR) != 0) {
                    return fail(error, "fmt チャンクをスキップできません");
                }
            } else if (chunkSize > 16) {
                if (std::fseek(file.get(), static_cast<long>(chunkSize) - 16, SEEK_CUR) != 0) {
                    return fail(error, "fmt チャンクをスキップできません");
                }
            }
            haveFormat = true;
        } else if (std::memcmp(chunkId, "data", 4) == 0) {
            if (!haveFormat) {
                return fail(error, "data チャンクが fmt より先に現れました");
            }
            if (formatTag != kFormatPcm && formatTag != kFormatFloat) {
                return fail(error, std::format("未対応の音声形式です (タグ 0x{:04x})", formatTag));
            }
            if (channels == 0 || blockAlign == 0) {
                return fail(error, "チャンネル数またはブロックサイズが不正です");
            }

            const std::size_t frames = chunkSize / blockAlign;
            const std::uint16_t bytesPerSample = blockAlign / channels;

            std::vector<std::uint8_t> raw(static_cast<std::size_t>(frames) * blockAlign);
            if (!raw.empty() && !readExact(file.get(), raw.data(), raw.size())) {
                return fail(error, "音声データを読み取れません");
            }

            out->sampleRate = sampleRate;
            out->channels = channels;
            out->samples.resize(frames * channels);
            for (std::size_t f = 0; f < frames; ++f) {
                for (std::uint16_t c = 0; c < channels; ++c) {
                    const std::uint8_t* p = &raw[f * blockAlign + c * bytesPerSample];
                    out->samples[f * channels + c] = decodeSample(p, formatTag, bitsPerSample);
                }
            }
            return true;
        } else {
            // Chunks are word-aligned, so an odd size is followed by a pad byte.
            const long skip = static_cast<long>(chunkSize) + (chunkSize & 1u);
            if (std::fseek(file.get(), skip, SEEK_CUR) != 0) {
                break;
            }
        }
    }

    return fail(error, "data チャンクが見つかりません");
}

bool writeWav(const std::string& path, const AudioBuffer& buffer, WavSampleFormat format,
              std::string* error) {
    if (buffer.channels == 0) {
        return fail(error, "チャンネル数が 0 です");
    }

    const std::uint16_t bitsPerSample = format == WavSampleFormat::Int16   ? 16
                                        : format == WavSampleFormat::Int24 ? 24
                                                                           : 32;
    const std::uint16_t formatTag = format == WavSampleFormat::Float32 ? kFormatFloat : kFormatPcm;
    const std::uint16_t bytesPerSample = bitsPerSample / 8;
    const auto channels = static_cast<std::uint16_t>(buffer.channels);
    const std::uint16_t blockAlign = static_cast<std::uint16_t>(bytesPerSample * channels);
    const std::uint32_t bytesPerSecond = buffer.sampleRate * blockAlign;
    const std::size_t frames = buffer.frames();
    const auto dataBytes = static_cast<std::uint32_t>(frames * blockAlign);

    FilePtr file = openFile(path, "wb");
    if (!file) {
        return fail(error, std::format("ファイルを作成できません: {}", path));
    }

    const bool headerOk =
        writeExact(file.get(), "RIFF", 4) && writeValue(file.get(), std::uint32_t{36} + dataBytes) &&
        writeExact(file.get(), "WAVE", 4) && writeExact(file.get(), "fmt ", 4) &&
        writeValue(file.get(), std::uint32_t{16}) && writeValue(file.get(), formatTag) &&
        writeValue(file.get(), channels) && writeValue(file.get(), buffer.sampleRate) &&
        writeValue(file.get(), bytesPerSecond) && writeValue(file.get(), blockAlign) &&
        writeValue(file.get(), bitsPerSample) && writeExact(file.get(), "data", 4) &&
        writeValue(file.get(), dataBytes);
    if (!headerOk) {
        return fail(error, "ヘッダーを書き込めません");
    }

    std::vector<std::uint8_t> raw(static_cast<std::size_t>(frames) * blockAlign);
    for (std::size_t i = 0; i < frames * channels; ++i) {
        const float clamped = std::clamp(buffer.samples[i], -1.0f, 1.0f);
        std::uint8_t* p = &raw[i * bytesPerSample];
        // Rounding rather than truncating halves the quantisation error. At 16
        // bits the difference is a full LSB, which is enough to fail a
        // round-trip comparison.
        switch (format) {
            case WavSampleFormat::Int16: {
                const auto v = static_cast<std::int16_t>(std::lround(clamped * 32767.0));
                std::memcpy(p, &v, sizeof(v));
                break;
            }
            case WavSampleFormat::Int24: {
                const auto v = static_cast<std::int32_t>(std::lround(clamped * 8388607.0));
                p[0] = static_cast<std::uint8_t>(v & 0xFF);
                p[1] = static_cast<std::uint8_t>((v >> 8) & 0xFF);
                p[2] = static_cast<std::uint8_t>((v >> 16) & 0xFF);
                break;
            }
            case WavSampleFormat::Float32:
                std::memcpy(p, &clamped, sizeof(float));
                break;
        }
    }

    if (!raw.empty() && !writeExact(file.get(), raw.data(), raw.size())) {
        return fail(error, "音声データを書き込めません");
    }
    return true;
}

}  // namespace audiolens
