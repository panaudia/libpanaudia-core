#include "panaudia/session_manager.h"

namespace panaudia {

// Ring buffer capacity: ~200ms of audio at the track's sample rate
static constexpr uint32_t RING_BUFFER_MS = 200;

SessionManager::SessionManager() = default;
SessionManager::~SessionManager() = default;

void SessionManager::configure(const SessionConfig& config) {
    // Replace previous state
    tracks_.clear();
    name_map_.clear();
    config_ = config;
    state_ = ConnectionState::Disconnected;

    for (const auto& tc : config_.tracks) {
        auto handle = std::make_unique<TrackHandle>();
        handle->config = tc;

        if (tc.type == TrackType::Audio) {
            uint32_t ring_capacity_frames =
                (tc.sample_rate * RING_BUFFER_MS) / 1000;

            if (tc.direction == TrackDirection::Outbound) {
                // Outbound audio: ring buffer + optional encoder
                handle->ring_buffer =
                    std::make_unique<RingBuffer>(ring_capacity_frames, tc.channels);

                if (tc.codec == AudioCodec::Opus) {
                    auto enc = std::make_unique<OpusEncoderWrapper>();
                    OpusEncoderConfig enc_cfg;
                    enc_cfg.sample_rate = tc.sample_rate;
                    enc_cfg.channels = tc.channels;
                    enc_cfg.bitrate = tc.opus_bitrate;
                    enc_cfg.frame_size_ms = tc.opus_frame_size_ms;
                    enc->init(enc_cfg);
                    handle->encoder = std::move(enc);
                }
            } else {
                // Inbound audio: jitter buffer + optional decoder
                JitterBufferConfig jb_cfg;
                jb_cfg.sample_rate = tc.sample_rate;
                jb_cfg.num_channels = tc.channels;
                jb_cfg.min_latency_ms = config_.jitter_buffer_min_ms;
                jb_cfg.max_latency_ms = config_.jitter_buffer_max_ms;
                jb_cfg.target_latency_ms = config_.jitter_buffer_initial_ms;
                handle->jitter_buffer = std::make_unique<JitterBuffer>(jb_cfg);

                if (tc.codec == AudioCodec::Opus) {
                    auto dec = std::make_unique<OpusDecoderWrapper>();
                    OpusDecoderConfig dec_cfg;
                    dec_cfg.sample_rate = tc.sample_rate;
                    dec_cfg.channels = tc.channels;
                    dec->init(dec_cfg);
                    handle->decoder = std::move(dec);
                }
            }
        }
        // Data tracks: no buffers or codecs needed

        name_map_[tc.name] = handle.get();
        tracks_.push_back(std::move(handle));
    }
}

TrackHandle* SessionManager::get_track(const std::string& name) {
    auto it = name_map_.find(name);
    if (it == name_map_.end()) return nullptr;
    return it->second;
}

void SessionManager::update_jwt(const std::string& jwt) {
    config_.jwt = jwt;
}

void SessionManager::connect() {
    // Phase 4b
}

void SessionManager::disconnect() {
    // Phase 4b
}

void SessionManager::write_audio(TrackHandle* /*track*/,
                                  const float* /*samples*/,
                                  uint32_t /*frame_count*/,
                                  uint64_t /*host_time*/) {
    // Phase 4c
}

uint32_t SessionManager::read_audio(TrackHandle* /*track*/,
                                     float* /*buffer*/,
                                     uint32_t /*frame_count*/,
                                     uint64_t /*host_time*/) {
    // Phase 4c
    return 0;
}

void SessionManager::send_data(TrackHandle* /*track*/,
                                const uint8_t* /*data*/,
                                uint32_t /*data_len*/) {
    // Phase 4c
}

ConnectionState SessionManager::get_connection_state() const {
    return state_;
}

BufferStatus SessionManager::get_buffer_status(TrackHandle* track) const {
    if (!track) return {};

    if (track->config.type == TrackType::Audio) {
        if (track->config.direction == TrackDirection::Outbound &&
            track->ring_buffer) {
            uint32_t cap = track->ring_buffer->capacity_frames();
            uint32_t avail = track->ring_buffer->read_available();
            return BufferStatus{
                avail,
                cap,
                cap > 0 ? static_cast<float>(avail) / static_cast<float>(cap)
                        : 0.0f,
                0, 0};
        }
        if (track->config.direction == TrackDirection::Inbound &&
            track->jitter_buffer) {
            auto stats = track->jitter_buffer->get_stats();
            uint32_t buffered = stats.fill_level_samples > 0
                                    ? static_cast<uint32_t>(stats.fill_level_samples)
                                    : 0;
            // Approximate capacity from max_latency_ms
            uint32_t cap_frames =
                (track->config.sample_rate * config_.jitter_buffer_max_ms) / 1000;
            return BufferStatus{
                buffered,
                cap_frames,
                cap_frames > 0
                    ? static_cast<float>(buffered) / static_cast<float>(cap_frames)
                    : 0.0f,
                stats.underrun_count,
                stats.overrun_count};
        }
    }
    return {};
}

SessionStats SessionManager::get_stats() const {
    return {state_, 0, 0, 0, 0, 0, 0.0};
}

}  // namespace panaudia
