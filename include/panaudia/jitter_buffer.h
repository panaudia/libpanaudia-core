#pragma once

#include <atomic>
#include <cstdint>
#include <vector>

namespace panaudia {

// Port of Go CircularBuffer (core/buffers/circular_buffer.go) with
// SPSC thread safety for use in the C++ core.
//
// Recv thread writes decoded audio (producer).
// RT thread reads for playback (consumer).
//
// State machine: FILLING -> PLAYING (with drift correction).
// See jitter_buffer_design.md for full design rationale.

struct JitterBufferConfig {
    uint32_t sample_rate = 48000;
    uint32_t num_channels = 1;         // 1=mono, 2=stereo, etc.
    uint32_t target_latency_ms = 60;   // centre of target window
    uint32_t target_window_ms = 20;    // width of target window
    uint32_t min_latency_ms = 10;      // below this = underrun
    uint32_t max_latency_ms = 200;     // above this = overrun snap
    uint32_t capacity_ms = 1000;       // total ring size
    uint32_t correction_interval = 16; // reads between corrections
};

enum class JitterBufferState {
    Filling,
    Playing,
};

struct JitterBufferStats {
    int32_t fill_level_samples;    // per-channel samples buffered
    float fill_level_ms;
    int32_t current_zone;          // -1, 0, or +1
    uint32_t underrun_count;
    uint32_t overrun_count;
    uint32_t samples_dropped;
    uint32_t samples_inserted;
    JitterBufferState state;
};

class JitterBuffer {
public:
    explicit JitterBuffer(const JitterBufferConfig& config = {});

    // Writer side (recv thread only). Copies decoded audio into the ring.
    // Never blocks. On capacity overflow, sets overflow flag for reader.
    void write(const float* src, uint32_t float_count);

    // Reader side (RT thread only). Fills dst with audio.
    // Returns true if audio produced, false if silence (FILLING or underrun).
    // float_count must be a multiple of num_channels.
    bool read(float* dst, uint32_t float_count);

    JitterBufferStats get_stats() const;

    uint32_t num_channels() const { return num_channels_; }
    uint32_t sample_rate() const { return sample_rate_; }

private:
    void copy_to_ring(uint32_t pos, const float* src, uint32_t floats);
    void copy_from_ring(uint32_t pos, float* dst, uint32_t floats) const;

    std::vector<float> data_;
    uint32_t capacity_;          // total floats in ring
    uint32_t num_channels_;
    uint32_t sample_rate_;

    // Zone boundaries in per-channel samples
    int32_t target_low_;
    int32_t target_high_;
    int32_t target_centre_;
    int32_t min_samples_;
    int32_t max_samples_;

    // Writer-owned, but read by reader during overflow snap
    alignas(64) std::atomic<uint32_t> write_pos_{0};

    // Reader-owned only
    uint32_t read_pos_ = 0;
    JitterBufferState state_ = JitterBufferState::Filling;
    uint32_t correction_interval_;
    uint32_t correction_counter_ = 0;

    // Stats (reader-owned, except underrun_count which writer doesn't touch)
    uint32_t underrun_count_ = 0;
    uint32_t overrun_count_ = 0;
    uint32_t samples_dropped_ = 0;
    uint32_t samples_inserted_ = 0;

    // Shared between writer and reader
    alignas(64) std::atomic<int32_t> buffered_{0};
    // Writer sets this on overflow; reader checks and handles
    alignas(64) std::atomic<bool> overflow_flag_{false};
};

}  // namespace panaudia
