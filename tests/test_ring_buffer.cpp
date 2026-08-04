#include "test_support.h"

#include "engine/ring_buffer.h"

#include <numeric>
#include <vector>

using audiolens::RingBuffer;

namespace {

/// Builds `frames` stereo frames whose left channel counts up from `start` and
/// whose right channel is the negated left, so a mis-ordered or mis-strided
/// copy is immediately visible.
std::vector<float> makeFrames(int start, std::size_t frames) {
    std::vector<float> data(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        data[i * 2] = static_cast<float>(start + static_cast<int>(i));
        data[i * 2 + 1] = -static_cast<float>(start + static_cast<int>(i));
    }
    return data;
}

}  // namespace

AL_TEST(RingBuffer_starts_empty) {
    RingBuffer ring(100, 2);
    CHECK_EQ(ring.capacityFrames(), std::size_t{100});
    CHECK_EQ(ring.availableToRead(), std::size_t{0});
    CHECK_EQ(ring.availableToWrite(), std::size_t{100});
    CHECK_EQ(ring.channels(), std::uint32_t{2});
}

AL_TEST(RingBuffer_round_trips_frames_in_order) {
    RingBuffer ring(100, 2);
    const std::vector<float> input = makeFrames(0, 40);

    CHECK_EQ(ring.write(input.data(), 40), std::size_t{40});
    CHECK_EQ(ring.availableToRead(), std::size_t{40});

    std::vector<float> output(40 * 2, 0.0f);
    CHECK_EQ(ring.read(output.data(), 40), std::size_t{40});
    CHECK_EQ(ring.availableToRead(), std::size_t{0});
    CHECK(output == input);
}

AL_TEST(RingBuffer_preserves_data_across_wraparound) {
    // Capacity 16 with 10-frame blocks guarantees the write and read cursors
    // wrap at different points on every pass.
    RingBuffer ring(16, 2);
    int counter = 0;

    for (int pass = 0; pass < 50; ++pass) {
        const std::vector<float> input = makeFrames(counter, 10);
        CHECK_EQ(ring.write(input.data(), 10), std::size_t{10});

        std::vector<float> output(10 * 2, 0.0f);
        CHECK_EQ(ring.read(output.data(), 10), std::size_t{10});

        if (output != input) {
            CHECK(false);
            break;
        }
        counter += 10;
    }
}

AL_TEST(RingBuffer_write_is_partial_when_full) {
    RingBuffer ring(10, 2);
    const std::vector<float> input = makeFrames(0, 25);

    // Only the capacity fits; the remainder is the overrun the engine counts.
    CHECK_EQ(ring.write(input.data(), 25), std::size_t{10});
    CHECK_EQ(ring.availableToWrite(), std::size_t{0});
    CHECK_EQ(ring.write(input.data(), 1), std::size_t{0});

    // The frames that did fit are the leading ones, unmangled.
    std::vector<float> output(10 * 2, 0.0f);
    CHECK_EQ(ring.read(output.data(), 10), std::size_t{10});
    CHECK(std::equal(output.begin(), output.end(), input.begin()));
}

AL_TEST(RingBuffer_read_is_partial_when_empty) {
    RingBuffer ring(10, 2);
    const std::vector<float> input = makeFrames(7, 3);
    CHECK_EQ(ring.write(input.data(), 3), std::size_t{3});

    std::vector<float> output(10 * 2, 99.0f);
    CHECK_EQ(ring.read(output.data(), 10), std::size_t{3});
    CHECK_EQ(ring.availableToRead(), std::size_t{0});

    // Frames beyond what was available must be left untouched: the engine is
    // responsible for zero-filling them and counting the underrun.
    CHECK_EQ(output[3 * 2], 99.0f);
}

AL_TEST(RingBuffer_writeSilence_appends_zeros) {
    RingBuffer ring(20, 2);
    const std::vector<float> input = makeFrames(1, 5);
    CHECK_EQ(ring.write(input.data(), 5), std::size_t{5});
    CHECK_EQ(ring.writeSilence(5), std::size_t{5});
    CHECK_EQ(ring.availableToRead(), std::size_t{10});

    std::vector<float> output(10 * 2, 42.0f);
    CHECK_EQ(ring.read(output.data(), 10), std::size_t{10});
    for (std::size_t i = 0; i < 5; ++i) {
        CHECK_EQ(output[i * 2], input[i * 2]);
    }
    for (std::size_t i = 5; i < 10; ++i) {
        CHECK_EQ(output[i * 2], 0.0f);
        CHECK_EQ(output[i * 2 + 1], 0.0f);
    }
}

AL_TEST(RingBuffer_writeSilence_wraps_correctly) {
    RingBuffer ring(8, 2);
    // Push the cursors near the end of the storage so the silence write splits.
    const std::vector<float> filler = makeFrames(0, 6);
    CHECK_EQ(ring.write(filler.data(), 6), std::size_t{6});
    std::vector<float> drain(6 * 2, 0.0f);
    CHECK_EQ(ring.read(drain.data(), 6), std::size_t{6});

    CHECK_EQ(ring.writeSilence(8), std::size_t{8});
    std::vector<float> output(8 * 2, 42.0f);
    CHECK_EQ(ring.read(output.data(), 8), std::size_t{8});
    for (float sample : output) {
        CHECK_EQ(sample, 0.0f);
    }
}

AL_TEST(RingBuffer_discard_drops_oldest_frames) {
    RingBuffer ring(20, 2);
    const std::vector<float> input = makeFrames(0, 10);
    CHECK_EQ(ring.write(input.data(), 10), std::size_t{10});

    CHECK_EQ(ring.discard(3), std::size_t{3});
    CHECK_EQ(ring.availableToRead(), std::size_t{7});

    std::vector<float> output(7 * 2, 0.0f);
    CHECK_EQ(ring.read(output.data(), 7), std::size_t{7});
    // Frame 3 is now the oldest survivor.
    CHECK_EQ(output[0], 3.0f);
    CHECK_EQ(output[1], -3.0f);
}

AL_TEST(RingBuffer_discard_is_clamped_to_available) {
    RingBuffer ring(20, 2);
    CHECK_EQ(ring.discard(5), std::size_t{0});

    const std::vector<float> input = makeFrames(0, 4);
    CHECK_EQ(ring.write(input.data(), 4), std::size_t{4});
    CHECK_EQ(ring.discard(100), std::size_t{4});
    CHECK_EQ(ring.availableToRead(), std::size_t{0});
}

AL_TEST(RingBuffer_supports_non_stereo_channel_counts) {
    RingBuffer ring(10, 1);
    const std::vector<float> input = {1.0f, 2.0f, 3.0f};
    CHECK_EQ(ring.write(input.data(), 3), std::size_t{3});

    std::vector<float> output(3, 0.0f);
    CHECK_EQ(ring.read(output.data(), 3), std::size_t{3});
    CHECK(output == input);
}
