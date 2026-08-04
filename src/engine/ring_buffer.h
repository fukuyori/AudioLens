#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

namespace audiolens {

/// Lock-free single-producer / single-consumer ring buffer of interleaved float frames.
///
/// The capture thread is the sole producer and the render thread the sole consumer.
/// Storage is allocated once at construction so neither audio thread ever allocates.
class RingBuffer {
public:
    RingBuffer(std::size_t frameCapacity, std::uint32_t channels)
        : channels_(channels),
          // One slot is left permanently empty so that write==read means "empty"
          // rather than being ambiguous with "full".
          slots_(frameCapacity + 1),
          data_(slots_ * channels, 0.0f) {}

    std::uint32_t channels() const noexcept { return channels_; }
    std::size_t capacityFrames() const noexcept { return slots_ - 1; }

    std::size_t availableToRead() const noexcept {
        const std::size_t w = write_.load(std::memory_order_acquire);
        const std::size_t r = read_.load(std::memory_order_acquire);
        return (w >= r) ? (w - r) : (slots_ - r + w);
    }

    std::size_t availableToWrite() const noexcept { return capacityFrames() - availableToRead(); }

    /// Producer side. Returns the number of frames actually written, which is
    /// less than `frames` when the buffer is full (an overrun).
    std::size_t write(const float* src, std::size_t frames) noexcept {
        const std::size_t writable = std::min(frames, availableToWrite());
        if (writable == 0) {
            return 0;
        }
        std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t firstChunk = std::min(writable, slots_ - w);
        std::memcpy(&data_[w * channels_], src, firstChunk * channels_ * sizeof(float));
        const std::size_t rest = writable - firstChunk;
        if (rest > 0) {
            std::memcpy(&data_[0], src + firstChunk * channels_, rest * channels_ * sizeof(float));
        }
        w = advance(w, writable);
        write_.store(w, std::memory_order_release);
        return writable;
    }

    /// Producer side: append `frames` frames of silence.
    std::size_t writeSilence(std::size_t frames) noexcept {
        const std::size_t writable = std::min(frames, availableToWrite());
        if (writable == 0) {
            return 0;
        }
        std::size_t w = write_.load(std::memory_order_relaxed);
        const std::size_t firstChunk = std::min(writable, slots_ - w);
        std::memset(&data_[w * channels_], 0, firstChunk * channels_ * sizeof(float));
        const std::size_t rest = writable - firstChunk;
        if (rest > 0) {
            std::memset(&data_[0], 0, rest * channels_ * sizeof(float));
        }
        w = advance(w, writable);
        write_.store(w, std::memory_order_release);
        return writable;
    }

    /// Consumer side. Returns the number of frames actually read, which is less
    /// than `frames` when the buffer has run dry (an underrun).
    std::size_t read(float* dst, std::size_t frames) noexcept {
        const std::size_t readable = std::min(frames, availableToRead());
        if (readable == 0) {
            return 0;
        }
        std::size_t r = read_.load(std::memory_order_relaxed);
        const std::size_t firstChunk = std::min(readable, slots_ - r);
        std::memcpy(dst, &data_[r * channels_], firstChunk * channels_ * sizeof(float));
        const std::size_t rest = readable - firstChunk;
        if (rest > 0) {
            std::memcpy(dst + firstChunk * channels_, &data_[0], rest * channels_ * sizeof(float));
        }
        r = advance(r, readable);
        read_.store(r, std::memory_order_release);
        return readable;
    }

    /// Consumer side: drop frames without copying them out. Used to pull the fill
    /// level back down when the capture clock runs faster than the render clock.
    std::size_t discard(std::size_t frames) noexcept {
        const std::size_t droppable = std::min(frames, availableToRead());
        if (droppable == 0) {
            return 0;
        }
        const std::size_t r = advance(read_.load(std::memory_order_relaxed), droppable);
        read_.store(r, std::memory_order_release);
        return droppable;
    }

private:
    std::size_t advance(std::size_t index, std::size_t by) const noexcept {
        index += by;
        if (index >= slots_) {
            index -= slots_;
        }
        return index;
    }

    std::uint32_t channels_;
    std::size_t slots_;
    std::vector<float> data_;

    alignas(64) std::atomic<std::size_t> write_{0};
    alignas(64) std::atomic<std::size_t> read_{0};
};

}  // namespace audiolens
