#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace panaudia {

// --- Enums ---

enum class TrackType {
    Audio,  // has ring buffer, optional codec, RT read/write
    Data,   // opaque bytes, send/receive via callback
};

enum class TrackDirection {
    Outbound,  // host -> server (publish)
    Inbound,   // server -> host (subscribe)
};

enum class AudioCodec {
    Opus,  // compressed — lower bandwidth, ~5ms encode/decode latency
    PCM,   // uncompressed — higher bandwidth, zero codec latency
};

enum class ConnectionState {
    Disconnected,
    Connecting,
    Connected,
    Reconnecting,
    Failed,
};

enum class LogLevel {
    Trace,
    Debug,
    Info,
    Warn,
    Error,
};

// --- Forward declarations ---

struct TrackHandle;

// --- Callbacks ---

// Inbound data track delivery. Called from core's internal thread (not RT).
// Host must not block in this callback.
using DataRecvCallback = void (*)(TrackHandle* track,
                                  const uint8_t* data,
                                  uint32_t data_len,
                                  void* ctx);

// Core transport status changes.
using StatusCallback = void (*)(ConnectionState state,
                                const char* message,  // null unless error
                                void* ctx);

// Logging. Called from any core thread.
using LogCallback = void (*)(LogLevel level,
                             const char* message,
                             void* ctx);

// --- Configuration ---

struct TrackConfig {
    // Identity
    std::string name;                        // local handle name (for get_track lookup)
    std::vector<std::string> moq_namespace;  // full MOQ namespace tuple
    std::string moq_track_name;              // MOQ track name (usually empty)
    TrackDirection direction;

    // Type
    TrackType type = TrackType::Data;        // default to data; audio tracks set this

    // Audio-specific (ignored when type == Data)
    uint32_t channels = 1;                   // 1=mono, 2=stereo, 4=FOA, etc.
    uint32_t sample_rate = 48000;
    AudioCodec codec = AudioCodec::Opus;
    uint32_t opus_bitrate = 64000;
    uint32_t opus_frame_size_ms = 5;
    uint32_t pcm_frame_size_ms = 5;
};

struct SessionConfig {
    std::string server_url;
    std::string jwt;

    // All tracks — audio and data, inbound and outbound.
    std::vector<TrackConfig> tracks;

    // Jitter buffer defaults (inbound audio tracks only)
    uint32_t jitter_buffer_min_ms = 10;
    uint32_t jitter_buffer_max_ms = 200;
    uint32_t jitter_buffer_initial_ms = 60;

    // Called when data arrives on any inbound data track.
    DataRecvCallback data_recv_callback = nullptr;
    void* data_recv_ctx = nullptr;

    // Called when the core's transport state changes.
    StatusCallback status_callback = nullptr;
    void* status_ctx = nullptr;

    // Logging
    LogCallback log_callback = nullptr;
    void* log_ctx = nullptr;
    LogLevel log_level = LogLevel::Info;
};

// --- Status types ---

struct BufferStatus {
    uint32_t buffered_frames;
    uint32_t capacity_frames;
    float fill_ratio;            // 0.0 to 1.0
    uint64_t underrun_count;
    uint64_t overrun_count;
};

struct SessionStats {
    ConnectionState state;
    uint64_t bytes_sent;
    uint64_t bytes_received;
    uint64_t packets_sent;
    uint64_t packets_received;
    uint64_t packets_lost;
    double rtt_ms;
};

// --- PanaudiaCore ---

class PanaudiaCore {
public:
    PanaudiaCore();
    ~PanaudiaCore();

    // Non-copyable, non-movable (owns internal resources)
    PanaudiaCore(const PanaudiaCore&) = delete;
    PanaudiaCore& operator=(const PanaudiaCore&) = delete;

    // === Lifecycle (called from host's control thread) ===

    void configure(const SessionConfig& config);
    void connect();
    void disconnect();
    void update_jwt(const std::string& jwt);

    // === Track Handle Lookup ===
    // After configure(), the host resolves track names to opaque handles.
    // Handles are stable for the lifetime of the session.
    // Returns nullptr if name not found.

    TrackHandle* get_track(const std::string& name);

    // === Realtime Audio (called from host's RT thread) ===
    // MUST be lock-free: no allocation, no locks, no blocking, no syscalls.
    // Only valid for audio tracks.

    // Write captured audio into the outbound ring buffer.
    void write_audio(TrackHandle* track,
                     const float* samples,
                     uint32_t frame_count,
                     uint64_t host_time);

    // Read decoded audio from the inbound ring buffer.
    // Returns frames actually read. If < frame_count, caller fills remainder
    // with silence.
    uint32_t read_audio(TrackHandle* track,
                        float* buffer,
                        uint32_t frame_count,
                        uint64_t host_time);

    // === Data Send (called from host's control thread) ===
    // Valid for data tracks. Sends opaque bytes via MOQ.

    void send_data(TrackHandle* track,
                   const uint8_t* data,
                   uint32_t data_len);

    // === Status (thread-safe, lock-free reads) ===

    ConnectionState get_connection_state() const;
    BufferStatus get_buffer_status(TrackHandle* track) const;
    SessionStats get_stats() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace panaudia
