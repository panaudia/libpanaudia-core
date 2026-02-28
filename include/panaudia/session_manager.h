#pragma once

#include "panaudia/core.h"
#include "panaudia/jitter_buffer.h"
#include "panaudia/opus_codec.h"
#include "panaudia/ring_buffer.h"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace panaudia {

// ---------------------------------------------------------------------------
// TrackHandle — created by configure(), looked up by get_track()
//
// Owns the per-track buffers and codecs. The host holds pointers to these
// (stable for the lifetime of a configure() call) and passes them to
// write_audio / read_audio / send_data.
// ---------------------------------------------------------------------------

struct TrackHandle {
    TrackConfig config;

    // MOQ state (populated during connect, Phase 4b)
    uint64_t moq_track_alias = 0;
    uint64_t moq_request_id = 0;
    uint64_t next_object_id = 0;
    uint64_t next_group_id = 0;

    // Buffers (owned, created during configure)
    std::unique_ptr<RingBuffer> ring_buffer;        // outbound audio only
    std::unique_ptr<JitterBuffer> jitter_buffer;     // inbound audio only

    // Codecs (owned, created during configure)
    std::unique_ptr<OpusEncoderWrapper> encoder;     // outbound Opus audio only
    std::unique_ptr<OpusDecoderWrapper> decoder;     // inbound Opus audio only
};

// ---------------------------------------------------------------------------
// SessionManager — owns tracks, delegates to MoqTransport (Phase 4b)
// ---------------------------------------------------------------------------

class SessionManager {
public:
    SessionManager();
    ~SessionManager();

    // Non-copyable, non-movable
    SessionManager(const SessionManager&) = delete;
    SessionManager& operator=(const SessionManager&) = delete;

    // Creates tracks with appropriate buffers and codecs.
    // Replaces any previous configuration.
    void configure(const SessionConfig& config);

    // Lookup a track by its config.name. Returns nullptr if not found.
    TrackHandle* get_track(const std::string& name);

    // JWT update (stored for reconnection)
    void update_jwt(const std::string& jwt);

    // Phase 4b: connect/disconnect/process_incoming
    void connect();
    void disconnect();

    // Phase 4c: write_audio/read_audio/send_data
    void write_audio(TrackHandle* track, const float* samples,
                     uint32_t frame_count, uint64_t host_time);
    uint32_t read_audio(TrackHandle* track, float* buffer,
                        uint32_t frame_count, uint64_t host_time);
    void send_data(TrackHandle* track, const uint8_t* data, uint32_t data_len);

    // Status
    ConnectionState get_connection_state() const;
    BufferStatus get_buffer_status(TrackHandle* track) const;
    SessionStats get_stats() const;

private:
    SessionConfig config_;
    std::vector<std::unique_ptr<TrackHandle>> tracks_;
    std::unordered_map<std::string, TrackHandle*> name_map_;
    ConnectionState state_ = ConnectionState::Disconnected;

    // Phase 4b adds: alias_map_, transport_, session_thread_
};

}  // namespace panaudia
