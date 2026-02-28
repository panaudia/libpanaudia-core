#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace panaudia {

// ---------------------------------------------------------------------------
// Transport State Machine
// ---------------------------------------------------------------------------
//
// Disconnected -> Connecting -> Connected (QUIC up) -> Ready (SERVER_SETUP rx)
//                     |                                    |
//                   Failed                              Failed

enum class TransportState {
    Disconnected,
    Connecting,
    Connected,    // QUIC handshake complete
    Ready,        // MOQ SERVER_SETUP received
    Failed,
};

// ---------------------------------------------------------------------------
// TransportConfig
// ---------------------------------------------------------------------------

struct TransportConfig {
    std::string host;
    uint16_t port = 4433;
    bool skip_cert_validation = false;   // for dev/self-signed certs
    uint32_t idle_timeout_ms = 30000;
};

// ---------------------------------------------------------------------------
// TransportCallbacks — set once at connect() time
// ---------------------------------------------------------------------------

struct TransportCallbacks {
    // State changes. May be called from msquic thread or caller thread.
    std::function<void(TransportState state, const char* message)> on_state_changed;

    // Parsed control message. Called from process_incoming() caller's thread.
    // content points into internal buffer and is valid only for the callback duration.
    std::function<void(uint64_t message_type, const uint8_t* content,
                       int32_t content_len)> on_control_message;

    // Datagram received. Called on msquic callback thread — must be fast.
    // All fields are parsed from the MOQ Object Datagram header.
    // payload points into msquic's receive buffer (zero-copy, valid only for callback duration).
    std::function<void(uint64_t track_alias, uint64_t group_id, uint64_t object_id,
                       uint8_t priority, const uint8_t* payload,
                       int32_t payload_len)> on_datagram;

    // Max datagram size changed (from msquic DATAGRAM_STATE_CHANGED event).
    // Called on msquic callback thread.
    std::function<void(uint32_t max_send_length)> on_datagram_state_changed;
};

// ---------------------------------------------------------------------------
// MoqTransport — QUIC connection + MOQ handshake + datagram/control I/O
//
// This is an internal building block for SessionManager (Phase 4).
// Handles QUIC lifecycle and MOQ framing only — no track management,
// no subscribe/announce orchestration, no audio decode.
// ---------------------------------------------------------------------------

class MoqTransport {
public:
    MoqTransport();
    ~MoqTransport();

    // Non-copyable, non-movable
    MoqTransport(const MoqTransport&) = delete;
    MoqTransport& operator=(const MoqTransport&) = delete;

    // Start QUIC connection + MOQ handshake. Returns false if init fails.
    // Callbacks must remain valid until disconnect() or destruction.
    bool connect(const TransportConfig& config, const TransportCallbacks& callbacks);

    // Shut down connection. Safe to call when already disconnected or from callbacks.
    void disconnect();

    // Send a pre-built framed control message on the control stream.
    // Data should be built with moq_protocol.h build_control_message().
    // Thread-safe (msquic handles internal locking).
    // Returns false if not connected.
    bool send_control(const uint8_t* data, uint32_t len);

    // Send a pre-built MOQ datagram.
    // Data should be built with moq_protocol.h build_object_datagram().
    // Thread-safe. Returns false if not connected.
    bool send_datagram(const uint8_t* data, uint32_t len);

    // Drain incoming control stream data, reassemble messages, fire on_control_message.
    // Must be called periodically from the host's control/tick thread.
    void process_incoming();

    // Current transport state.
    TransportState state() const;

    // Max datagram payload size (0 if datagrams not yet enabled).
    uint32_t max_datagram_size() const;

private:
    struct Impl;
    Impl* impl_;
};

}  // namespace panaudia
