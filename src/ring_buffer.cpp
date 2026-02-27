#include "panaudia/ring_buffer.h"

#include <algorithm>

namespace panaudia {

RingBuffer::RingBuffer(uint32_t capacity_frames, uint32_t channels)
    : capacity_floats_(capacity_frames * channels)
    , capacity_frames_(capacity_frames)
    , channels_(channels) {
    data_.resize(capacity_floats_, 0.0f);
}

uint32_t RingBuffer::write(const float* samples, uint32_t frame_count) {
    if (frame_count == 0) return 0;

    uint32_t floats = frame_count * channels_;

    // If write is larger than capacity, only keep the tail
    if (floats > capacity_floats_) {
        samples += (floats - capacity_floats_);
        floats = capacity_floats_;
        frame_count = capacity_frames_;
    }

    uint32_t buf = buffered_.load(std::memory_order_relaxed);

    // If ring would overflow, advance read position to discard oldest
    if (buf + floats > capacity_floats_) {
        uint32_t excess = (buf + floats) - capacity_floats_;
        uint32_t rp = read_pos_.load(std::memory_order_relaxed);
        read_pos_.store((rp + excess) % capacity_floats_, std::memory_order_relaxed);
        buffered_.fetch_sub(excess, std::memory_order_relaxed);
    }

    uint32_t wp = write_pos_.load(std::memory_order_relaxed);
    copy_to_ring(wp, samples, floats);
    write_pos_.store((wp + floats) % capacity_floats_, std::memory_order_release);
    buffered_.fetch_add(floats, std::memory_order_release);

    return frame_count;
}

uint32_t RingBuffer::write_available() const {
    uint32_t buf = buffered_.load(std::memory_order_relaxed);
    return (capacity_floats_ - buf) / channels_;
}

uint32_t RingBuffer::read(float* buffer, uint32_t frame_count) {
    if (frame_count == 0) return 0;

    uint32_t floats_requested = frame_count * channels_;
    uint32_t buf = buffered_.load(std::memory_order_acquire);
    uint32_t floats_to_read = std::min(floats_requested, buf);

    if (floats_to_read == 0) return 0;

    uint32_t rp = read_pos_.load(std::memory_order_relaxed);
    copy_from_ring(rp, buffer, floats_to_read);
    read_pos_.store((rp + floats_to_read) % capacity_floats_, std::memory_order_relaxed);
    buffered_.fetch_sub(floats_to_read, std::memory_order_release);

    return floats_to_read / channels_;
}

uint32_t RingBuffer::read_available() const {
    uint32_t buf = buffered_.load(std::memory_order_relaxed);
    return buf / channels_;
}

void RingBuffer::flush() {
    read_pos_.store(write_pos_.load(std::memory_order_relaxed), std::memory_order_relaxed);
    buffered_.store(0, std::memory_order_release);
}

void RingBuffer::copy_to_ring(uint32_t pos, const float* src, uint32_t floats) {
    uint32_t remain = capacity_floats_ - pos;
    if (floats <= remain) {
        std::memcpy(&data_[pos], src, floats * sizeof(float));
    } else {
        std::memcpy(&data_[pos], src, remain * sizeof(float));
        std::memcpy(&data_[0], src + remain, (floats - remain) * sizeof(float));
    }
}

void RingBuffer::copy_from_ring(uint32_t pos, float* dst, uint32_t floats) const {
    uint32_t remain = capacity_floats_ - pos;
    if (floats <= remain) {
        std::memcpy(dst, &data_[pos], floats * sizeof(float));
    } else {
        std::memcpy(dst, &data_[pos], remain * sizeof(float));
        std::memcpy(dst + remain, &data_[0], (floats - remain) * sizeof(float));
    }
}

}  // namespace panaudia
