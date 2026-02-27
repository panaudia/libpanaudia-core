#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace panaudia {

// Plain SPSC lock-free ring buffer for outbound audio.
// RT thread writes (producer), send thread reads (consumer).
//
// Overflow policy: writer drops oldest data (advances read position).
// This is acceptable for outbound audio where overflow means the send
// thread has fallen behind.
class RingBuffer {
public:
    RingBuffer(uint32_t capacity_frames, uint32_t channels);

    // Writer side (one thread only)
    uint32_t write(const float* samples, uint32_t frame_count);
    uint32_t write_available() const;

    // Reader side (one thread only)
    uint32_t read(float* buffer, uint32_t frame_count);
    uint32_t read_available() const;

    // Discard all buffered data. Call from writer thread only (or when
    // neither thread is active).
    void flush();

    uint32_t channels() const { return channels_; }
    uint32_t capacity_frames() const { return capacity_frames_; }

private:
    void copy_to_ring(uint32_t pos, const float* src, uint32_t floats);
    void copy_from_ring(uint32_t pos, float* dst, uint32_t floats) const;

    std::vector<float> data_;
    uint32_t capacity_floats_;
    uint32_t capacity_frames_;
    uint32_t channels_;

    // Writer-owned position (float index into data_)
    alignas(64) std::atomic<uint32_t> write_pos_{0};
    // Reader-owned position (float index into data_)
    alignas(64) std::atomic<uint32_t> read_pos_{0};
    // Shared count of buffered floats
    alignas(64) std::atomic<uint32_t> buffered_{0};
};

}  // namespace panaudia
