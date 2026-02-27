#include "panaudia/jitter_buffer.h"

#include <cstring>

namespace panaudia {

static uint32_t ms_to_samples(uint32_t ms, uint32_t sample_rate) {
    return ms * sample_rate / 1000;
}

JitterBuffer::JitterBuffer(const JitterBufferConfig& config)
    : num_channels_(config.num_channels)
    , sample_rate_(config.sample_rate)
    , correction_interval_(config.correction_interval) {

    capacity_ = ms_to_samples(config.capacity_ms, sample_rate_) * num_channels_;
    data_.resize(capacity_, 0.0f);

    uint32_t target_low_ms = config.target_latency_ms - config.target_window_ms / 2;
    uint32_t target_high_ms = config.target_latency_ms + config.target_window_ms / 2;

    target_low_ = static_cast<int32_t>(ms_to_samples(target_low_ms, sample_rate_));
    target_high_ = static_cast<int32_t>(ms_to_samples(target_high_ms, sample_rate_));
    target_centre_ = static_cast<int32_t>(ms_to_samples(config.target_latency_ms, sample_rate_));
    min_samples_ = static_cast<int32_t>(ms_to_samples(config.min_latency_ms, sample_rate_));
    max_samples_ = static_cast<int32_t>(ms_to_samples(config.max_latency_ms, sample_rate_));
}

void JitterBuffer::write(const float* src, uint32_t float_count) {
    if (float_count == 0) return;

    uint32_t n = float_count;

    // If write is larger than capacity, only keep the tail
    if (n > capacity_) {
        src += (n - capacity_);
        n = capacity_;
    }

    int32_t buf = buffered_.load(std::memory_order_relaxed);

    // If ring would overflow, signal the reader to snap
    if (static_cast<uint32_t>(buf) + n > capacity_) {
        // We can't safely move readPos from the writer thread.
        // Set the overflow flag — the reader will handle the snap.
        overflow_flag_.store(true, std::memory_order_release);

        // We still need to write the data. Advance our own write_pos,
        // wrapping around. The reader will snap its position on next read.
        // To avoid corrupting data the reader is about to read, we limit
        // our write to capacity (the reader will snap past it anyway).
        // Reset buffered to capacity — reader will correct on snap.
        buffered_.store(static_cast<int32_t>(capacity_), std::memory_order_relaxed);
    } else {
        buffered_.fetch_add(static_cast<int32_t>(n), std::memory_order_release);
    }

    uint32_t wp = write_pos_.load(std::memory_order_relaxed);
    copy_to_ring(wp, src, n);
    write_pos_.store((wp + n) % capacity_, std::memory_order_release);
}

bool JitterBuffer::read(float* dst, uint32_t float_count) {
    if (float_count == 0) return true;

    // Check overflow flag from writer
    if (overflow_flag_.load(std::memory_order_acquire)) {
        overflow_flag_.store(false, std::memory_order_relaxed);
        // Snap readPos so fill equals targetCentre
        uint32_t snap_floats = static_cast<uint32_t>(target_centre_) * num_channels_;
        // Position read_pos_ behind write_pos_ by snap_floats
        uint32_t wp = write_pos_.load(std::memory_order_acquire);
        if (wp >= snap_floats) {
            read_pos_ = wp - snap_floats;
        } else {
            read_pos_ = capacity_ - (snap_floats - wp);
        }
        buffered_.store(static_cast<int32_t>(snap_floats), std::memory_order_relaxed);
        overrun_count_++;
    }

    int32_t buf = buffered_.load(std::memory_order_acquire);
    int32_t fill_samples = buf / static_cast<int32_t>(num_channels_);

    // FILLING state
    if (state_ == JitterBufferState::Filling) {
        if (fill_samples >= target_low_) {
            state_ = JitterBufferState::Playing;
            correction_counter_ = 0;
        } else {
            std::memset(dst, 0, float_count * sizeof(float));
            return false;
        }
    }

    // PLAYING state

    // Overrun snap (can still happen if writer wrote a lot between reads
    // without hitting the overflow flag path)
    if (fill_samples > max_samples_) {
        uint32_t snap_floats = static_cast<uint32_t>(target_centre_) * num_channels_;
        int32_t excess = buf - static_cast<int32_t>(snap_floats);
        read_pos_ = (read_pos_ + static_cast<uint32_t>(excess)) % capacity_;
        buffered_.fetch_sub(excess, std::memory_order_relaxed);
        buf = static_cast<int32_t>(snap_floats);
        fill_samples = buf / static_cast<int32_t>(num_channels_);
        overrun_count_++;
    }

    // Underrun
    if (fill_samples < min_samples_) {
        state_ = JitterBufferState::Filling;
        std::memset(dst, 0, float_count * sizeof(float));
        underrun_count_++;
        return false;
    }

    // Drift correction: ±1 sample when outside target window
    int32_t correction = 0;
    if (fill_samples < target_low_) {
        correction = -1;  // insert
    } else if (fill_samples > target_high_) {
        correction = +1;  // drop
    }

    // Apply correction only every N reads
    correction_counter_++;
    if (correction_counter_ < correction_interval_) {
        correction = 0;
    } else {
        correction_counter_ = 0;
    }

    int32_t nc = static_cast<int32_t>(num_channels_);
    int32_t floats_requested = static_cast<int32_t>(float_count);

    if (correction > 0) {
        // Dropping: output floatsRequested, consume extra from ring
        int32_t floats_to_consume = floats_requested + correction * nc;
        if (floats_to_consume > buf) floats_to_consume = buf;
        int32_t to_output = floats_requested;
        if (to_output > buf) to_output = buf;

        copy_from_ring(read_pos_, dst, static_cast<uint32_t>(to_output));
        // Zero-pad if needed
        for (int32_t i = to_output; i < floats_requested; i++) {
            dst[i] = 0.0f;
        }
        read_pos_ = (read_pos_ + static_cast<uint32_t>(floats_to_consume)) % capacity_;
        buffered_.fetch_sub(floats_to_consume, std::memory_order_release);
        samples_dropped_ += static_cast<uint32_t>(correction);

    } else if (correction < 0) {
        // Inserting: read fewer from ring, duplicate last sample
        int32_t real_floats = floats_requested + correction * nc;
        if (real_floats < nc) real_floats = nc;
        if (real_floats > buf) real_floats = buf;

        copy_from_ring(read_pos_, dst, static_cast<uint32_t>(real_floats));
        // Duplicate last sample (all channels) to fill remaining
        if (real_floats < floats_requested) {
            int32_t last_start = real_floats - nc;
            if (last_start < 0) last_start = 0;
            for (int32_t i = real_floats; i < floats_requested; i++) {
                dst[i] = dst[last_start + (i - real_floats) % nc];
            }
        }
        read_pos_ = (read_pos_ + static_cast<uint32_t>(real_floats)) % capacity_;
        buffered_.fetch_sub(real_floats, std::memory_order_release);
        samples_inserted_ += static_cast<uint32_t>(-correction);

    } else {
        // No correction
        int32_t to_read = floats_requested;
        if (to_read > buf) to_read = buf;

        copy_from_ring(read_pos_, dst, static_cast<uint32_t>(to_read));
        for (int32_t i = to_read; i < floats_requested; i++) {
            dst[i] = 0.0f;
        }
        read_pos_ = (read_pos_ + static_cast<uint32_t>(to_read)) % capacity_;
        buffered_.fetch_sub(to_read, std::memory_order_release);
    }

    return true;
}

JitterBufferStats JitterBuffer::get_stats() const {
    int32_t buf = buffered_.load(std::memory_order_relaxed);
    int32_t fill_samples = buf / static_cast<int32_t>(num_channels_);
    float fill_ms = static_cast<float>(fill_samples) / static_cast<float>(sample_rate_) * 1000.0f;

    int32_t zone = 0;
    if (fill_samples < target_low_) zone = -1;
    else if (fill_samples > target_high_) zone = 1;

    return {
        fill_samples,
        fill_ms,
        zone,
        underrun_count_,
        overrun_count_,
        samples_dropped_,
        samples_inserted_,
        state_,
    };
}

void JitterBuffer::copy_to_ring(uint32_t pos, const float* src, uint32_t floats) {
    uint32_t remain = capacity_ - pos;
    if (floats <= remain) {
        std::memcpy(&data_[pos], src, floats * sizeof(float));
    } else {
        std::memcpy(&data_[pos], src, remain * sizeof(float));
        std::memcpy(&data_[0], src + remain, (floats - remain) * sizeof(float));
    }
}

void JitterBuffer::copy_from_ring(uint32_t pos, float* dst, uint32_t floats) const {
    uint32_t remain = capacity_ - pos;
    if (floats <= remain) {
        std::memcpy(dst, &data_[pos], floats * sizeof(float));
    } else {
        std::memcpy(dst, &data_[pos], remain * sizeof(float));
        std::memcpy(dst + remain, &data_[0], (floats - remain) * sizeof(float));
    }
}

}  // namespace panaudia
