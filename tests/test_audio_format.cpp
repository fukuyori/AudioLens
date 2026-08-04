#include "test_support.h"

#include "engine/audio_format.h"

#include <cstring>
#include <vector>

using audiolens::convertFromFloat;
using audiolens::convertToFloat;
using audiolens::describeWaveFormat;
using audiolens::SampleType;
using audiolens::StreamFormat;

namespace {

StreamFormat makeFormat(SampleType type, std::uint32_t channels, std::uint32_t bytesPerSample) {
    StreamFormat format;
    format.sampleRate = 48000;
    format.channels = channels;
    format.sampleType = type;
    format.bytesPerFrame = channels * bytesPerSample;
    return format;
}

WAVEFORMATEXTENSIBLE makeExtensible(const GUID& subFormat, WORD bits, WORD validBits,
                                    WORD channels) {
    WAVEFORMATEXTENSIBLE ext{};
    ext.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    ext.Format.nChannels = channels;
    ext.Format.nSamplesPerSec = 48000;
    ext.Format.wBitsPerSample = bits;
    ext.Format.nBlockAlign = static_cast<WORD>(channels * bits / 8);
    ext.Format.nAvgBytesPerSec = ext.Format.nSamplesPerSec * ext.Format.nBlockAlign;
    ext.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    ext.Samples.wValidBitsPerSample = validBits;
    ext.SubFormat = subFormat;
    return ext;
}

}  // namespace

AL_TEST(describeWaveFormat_reads_plain_float32) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 44100;
    wfx.wBitsPerSample = 32;
    wfx.nBlockAlign = 8;

    const StreamFormat format = describeWaveFormat(&wfx);
    CHECK(format.valid());
    CHECK_EQ(format.sampleRate, std::uint32_t{44100});
    CHECK_EQ(format.channels, std::uint32_t{2});
    CHECK(format.sampleType == SampleType::Float32);
}

AL_TEST(describeWaveFormat_reads_extensible_float32) {
    // This is what WASAPI shared mode actually hands back on virtually every
    // endpoint, so it is the case that matters most.
    const WAVEFORMATEXTENSIBLE ext = makeExtensible(KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, 32, 32, 2);
    const StreamFormat format = describeWaveFormat(&ext.Format);
    CHECK(format.valid());
    CHECK(format.sampleType == SampleType::Float32);
    CHECK_EQ(format.channels, std::uint32_t{2});
}

AL_TEST(describeWaveFormat_reads_extensible_pcm_variants) {
    for (const WORD bits : {WORD{16}, WORD{24}, WORD{32}}) {
        const WAVEFORMATEXTENSIBLE ext = makeExtensible(KSDATAFORMAT_SUBTYPE_PCM, bits, bits, 2);
        const StreamFormat format = describeWaveFormat(&ext.Format);
        CHECK(format.valid());
    }

    // 24 valid bits inside a 32-bit container is a common driver layout.
    const WAVEFORMATEXTENSIBLE padded = makeExtensible(KSDATAFORMAT_SUBTYPE_PCM, 32, 24, 2);
    const StreamFormat format = describeWaveFormat(&padded.Format);
    CHECK(format.valid());
    CHECK(format.sampleType == SampleType::Int32);
}

AL_TEST(describeWaveFormat_rejects_unsupported_encodings) {
    WAVEFORMATEX wfx{};
    wfx.wFormatTag = WAVE_FORMAT_ADPCM;
    wfx.nChannels = 2;
    wfx.nSamplesPerSec = 48000;
    wfx.wBitsPerSample = 4;
    wfx.nBlockAlign = 1;

    const StreamFormat format = describeWaveFormat(&wfx);
    CHECK(!format.valid());

    CHECK(!describeWaveFormat(nullptr).valid());
}

AL_TEST(convertToFloat_copies_matching_float32_layout) {
    const StreamFormat source = makeFormat(SampleType::Float32, 2, 4);
    const std::vector<float> input = {0.25f, -0.5f, 1.0f, -1.0f};

    std::vector<float> output(4, 0.0f);
    convertToFloat(input.data(), source, output.data(), 2, 2);
    CHECK(output == input);
}

AL_TEST(convertToFloat_scales_int16) {
    const StreamFormat source = makeFormat(SampleType::Int16, 2, 2);
    const std::vector<std::int16_t> input = {32767, -32768, 0, 16384};

    std::vector<float> output(4, 0.0f);
    convertToFloat(input.data(), source, output.data(), 2, 2);

    CHECK_NEAR(output[0], 1.0, 1e-4);
    CHECK_NEAR(output[1], -1.0, 1e-6);
    CHECK_NEAR(output[2], 0.0, 1e-6);
    CHECK_NEAR(output[3], 0.5, 1e-6);
}

AL_TEST(convertToFloat_reads_int24_including_negatives) {
    const StreamFormat source = makeFormat(SampleType::Int24, 1, 3);
    // Little-endian 24-bit: +8388607 (full scale), -8388608, -1.
    const std::vector<std::uint8_t> input = {0xFF, 0xFF, 0x7F, 0x00, 0x00, 0x80, 0xFF, 0xFF, 0xFF};

    std::vector<float> output(3, 0.0f);
    convertToFloat(input.data(), source, output.data(), 1, 3);

    CHECK_NEAR(output[0], 1.0, 1e-6);
    CHECK_NEAR(output[1], -1.0, 1e-6);
    CHECK_NEAR(output[2], 0.0, 1e-6);
}

AL_TEST(convertToFloat_fans_mono_out_to_every_channel) {
    const StreamFormat source = makeFormat(SampleType::Float32, 1, 4);
    const std::vector<float> input = {0.5f, -0.25f};

    std::vector<float> output(4, 0.0f);
    convertToFloat(input.data(), source, output.data(), 2, 2);

    CHECK_EQ(output[0], 0.5f);
    CHECK_EQ(output[1], 0.5f);
    CHECK_EQ(output[2], -0.25f);
    CHECK_EQ(output[3], -0.25f);
}

AL_TEST(convertToFloat_averages_stereo_down_to_mono) {
    const StreamFormat source = makeFormat(SampleType::Float32, 2, 4);
    const std::vector<float> input = {1.0f, 0.0f, -0.5f, 0.5f};

    std::vector<float> output(2, 0.0f);
    convertToFloat(input.data(), source, output.data(), 1, 2);

    CHECK_NEAR(output[0], 0.5, 1e-6);
    CHECK_NEAR(output[1], 0.0, 1e-6);
}

AL_TEST(convertToFloat_takes_leading_pair_from_surround) {
    const StreamFormat source = makeFormat(SampleType::Float32, 6, 4);
    const std::vector<float> input = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f};

    std::vector<float> output(2, 0.0f);
    convertToFloat(input.data(), source, output.data(), 2, 1);

    CHECK_NEAR(output[0], 0.1, 1e-6);
    CHECK_NEAR(output[1], 0.2, 1e-6);
}

AL_TEST(convertToFloat_zero_fills_missing_channels) {
    const StreamFormat source = makeFormat(SampleType::Float32, 2, 4);
    const std::vector<float> input = {0.1f, 0.2f};

    std::vector<float> output(4, 7.0f);
    convertToFloat(input.data(), source, output.data(), 4, 1);

    CHECK_NEAR(output[0], 0.1, 1e-6);
    CHECK_NEAR(output[1], 0.2, 1e-6);
    CHECK_EQ(output[2], 0.0f);
    CHECK_EQ(output[3], 0.0f);
}

AL_TEST(convertToFloat_honours_padded_frame_stride) {
    // A device frame that is wider than channels*sampleSize must not be read
    // as tightly packed, or every frame after the first is misaligned.
    StreamFormat source = makeFormat(SampleType::Int16, 2, 2);
    source.bytesPerFrame = 8;  // 4 bytes of payload + 4 bytes of padding.

    std::vector<std::uint8_t> input(16, 0);
    const std::int16_t frame0[2] = {16384, -16384};
    const std::int16_t frame1[2] = {8192, -8192};
    std::memcpy(&input[0], frame0, sizeof(frame0));
    std::memcpy(&input[8], frame1, sizeof(frame1));

    std::vector<float> output(4, 0.0f);
    convertToFloat(input.data(), source, output.data(), 2, 2);

    CHECK_NEAR(output[0], 0.5, 1e-6);
    CHECK_NEAR(output[1], -0.5, 1e-6);
    CHECK_NEAR(output[2], 0.25, 1e-6);
    CHECK_NEAR(output[3], -0.25, 1e-6);
}

AL_TEST(convertFromFloat_clamps_out_of_range_samples) {
    const StreamFormat destination = makeFormat(SampleType::Float32, 2, 4);
    const std::vector<float> input = {2.5f, -3.0f};

    std::vector<float> output(2, 0.0f);
    convertFromFloat(input.data(), 2, output.data(), destination, 1);

    CHECK_EQ(output[0], 1.0f);
    CHECK_EQ(output[1], -1.0f);
}

AL_TEST(convertFromFloat_clamps_when_writing_integers) {
    const StreamFormat destination = makeFormat(SampleType::Int16, 1, 2);
    const std::vector<float> input = {5.0f, -5.0f};

    std::vector<std::int16_t> output(2, 0);
    convertFromFloat(input.data(), 1, output.data(), destination, 2);

    CHECK_EQ(output[0], std::int16_t{32767});
    CHECK_EQ(output[1], std::int16_t{-32767});
}

AL_TEST(convertFromFloat_round_trips_through_int16) {
    const StreamFormat format = makeFormat(SampleType::Int16, 2, 2);
    const std::vector<float> input = {0.0f, 0.5f, -0.5f, 0.25f, -0.75f, 0.125f};

    std::vector<std::int16_t> encoded(6, 0);
    convertFromFloat(input.data(), 2, encoded.data(), format, 3);

    std::vector<float> decoded(6, 0.0f);
    convertToFloat(encoded.data(), format, decoded.data(), 2, 3);

    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK_NEAR(decoded[i], input[i], 1.0 / 32767.0);
    }
}

AL_TEST(convertFromFloat_round_trips_through_int24) {
    const StreamFormat format = makeFormat(SampleType::Int24, 2, 3);
    const std::vector<float> input = {0.0f, 0.5f, -0.5f, 0.999f, -0.999f, 0.001f};

    std::vector<std::uint8_t> encoded(3 * 2 * 3, 0);
    convertFromFloat(input.data(), 2, encoded.data(), format, 3);

    std::vector<float> decoded(6, 0.0f);
    convertToFloat(encoded.data(), format, decoded.data(), 2, 3);

    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK_NEAR(decoded[i], input[i], 1e-6);
    }
}

AL_TEST(convertFromFloat_round_trips_through_int32) {
    const StreamFormat format = makeFormat(SampleType::Int32, 1, 4);
    const std::vector<float> input = {0.0f, 0.5f, -0.5f, 0.25f};

    std::vector<std::int32_t> encoded(4, 0);
    convertFromFloat(input.data(), 1, encoded.data(), format, 4);

    std::vector<float> decoded(4, 0.0f);
    convertToFloat(encoded.data(), format, decoded.data(), 1, 4);

    for (std::size_t i = 0; i < input.size(); ++i) {
        CHECK_NEAR(decoded[i], input[i], 1e-6);
    }
}

AL_TEST(convertFromFloat_fans_mono_out_to_device_channels) {
    const StreamFormat destination = makeFormat(SampleType::Float32, 2, 4);
    const std::vector<float> input = {0.3f, -0.3f};

    std::vector<float> output(4, 0.0f);
    convertFromFloat(input.data(), 1, output.data(), destination, 2);

    CHECK_NEAR(output[0], 0.3, 1e-6);
    CHECK_NEAR(output[1], 0.3, 1e-6);
    CHECK_NEAR(output[2], -0.3, 1e-6);
    CHECK_NEAR(output[3], -0.3, 1e-6);
}

AL_TEST(convertFromFloat_zero_fills_extra_device_channels) {
    const StreamFormat destination = makeFormat(SampleType::Float32, 6, 4);
    const std::vector<float> input = {0.1f, 0.2f};

    std::vector<float> output(6, 9.0f);
    convertFromFloat(input.data(), 2, output.data(), destination, 1);

    CHECK_NEAR(output[0], 0.1, 1e-6);
    CHECK_NEAR(output[1], 0.2, 1e-6);
    for (std::size_t i = 2; i < 6; ++i) {
        CHECK_EQ(output[i], 0.0f);
    }
}
