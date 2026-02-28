#pragma once

#include "panaudia/core.h"
#include "panaudia/jitter_buffer.h"
#include "panaudia/moq_transport.h"
#include "panaudia/opus_codec.h"
#include "panaudia/ring_buffer.h"

#include <atomic>
#include <memory>
#include <string>
#include <thread>
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

    // Pre-allocated send buffers (outbound audio only, sized in configure)
    std::vector<float> pcm_read_buffer;        // frame_size_samples * channels
    std::vector<uint8_t> encode_output_buffer; // Opus: 512 bytes, PCM: frame_size*channels*4
    std::vector<uint8_t> datagram_buffer;      // 34 (max header) + max payload

    // Pre-allocated recv decode buffer (inbound audio only, sized in configure)
    std::vector<float> decode_buffer;          // 960 * channels (covers up to 20ms @ 48kHz)
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
    std::atomic<ConnectionState> state_{ConnectionState::Disconnected};

    // Transport
    std::unique_ptr<MoqTransport> transport_;

    // Session thread — polls process_incoming() and drives orchestration
    std::unique_ptr<std::thread> session_thread_;
    std::atomic<bool> session_running_{false};

    // MOQ orchestration state
    uint64_t next_request_id_ = 0;      // even sequence: 0, 2, 4, ...
    uint64_t next_track_alias_ = 1;     // for incoming SUBSCRIBEs (we assign)
    std::unordered_map<uint64_t, TrackHandle*> request_id_map_;   // our SUBSCRIBE req_id → handle
    std::unordered_map<uint64_t, TrackHandle*> alias_map_;        // track_alias → handle
    bool orchestration_started_ = false;
    bool first_subscribe_sent_ = false;  // tracks whether JWT has been attached

    // URL parsing
    static bool parse_url(const std::string& url,
                          std::string& host, uint16_t& port);

    // Orchestration
    void session_thread_func();
    void start_orchestration();
    void handle_control_message(uint64_t message_type,
                                const uint8_t* content, int32_t content_len);

    // Phase 4c: send/recv
    void poll_outbound_tracks();
    void send_audio_frame(TrackHandle* track);
    void send_pcm_frame(TrackHandle* track);
    void dispatch_audio_datagram(TrackHandle* track,
                                  const uint8_t* payload, int32_t payload_len);
    void dispatch_data_datagram(TrackHandle* track,
                                 const uint8_t* payload, int32_t payload_len);

    // Logging
    void log(LogLevel level, const char* fmt, ...);
};

}  // namespace panaudia
