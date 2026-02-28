#include "panaudia/session_manager.h"
#include "panaudia/moq_protocol.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace panaudia {

// Ring buffer capacity: ~200ms of audio at the track's sample rate
static constexpr uint32_t RING_BUFFER_MS = 200;

// Session thread poll interval
static constexpr auto SESSION_POLL_INTERVAL = std::chrono::milliseconds(5);

SessionManager::SessionManager() = default;

SessionManager::~SessionManager() {
    disconnect();
}

// ---------------------------------------------------------------------------
// URL parsing
// ---------------------------------------------------------------------------

bool SessionManager::parse_url(const std::string& url,
                                std::string& host, uint16_t& port) {
    // Supported formats:
    //   https://host:port
    //   https://host
    //   host:port
    //   host
    std::string work = url;

    // Strip scheme
    auto scheme_end = work.find("://");
    if (scheme_end != std::string::npos) {
        work = work.substr(scheme_end + 3);
    }

    // Strip trailing path
    auto path_pos = work.find('/');
    if (path_pos != std::string::npos) {
        work = work.substr(0, path_pos);
    }

    if (work.empty()) return false;

    // Check for IPv6 bracketed address
    if (work[0] == '[') {
        auto bracket_end = work.find(']');
        if (bracket_end == std::string::npos) return false;
        host = work.substr(1, bracket_end - 1);
        if (bracket_end + 1 < work.size() && work[bracket_end + 1] == ':') {
            port = static_cast<uint16_t>(
                std::stoi(work.substr(bracket_end + 2)));
        } else {
            port = 443;
        }
        return !host.empty();
    }

    // IPv4 or hostname
    auto colon_pos = work.rfind(':');
    if (colon_pos != std::string::npos) {
        host = work.substr(0, colon_pos);
        port = static_cast<uint16_t>(std::stoi(work.substr(colon_pos + 1)));
    } else {
        host = work;
        port = 443;
    }

    return !host.empty();
}

// ---------------------------------------------------------------------------
// Logging helper
// ---------------------------------------------------------------------------

void SessionManager::log(LogLevel level, const char* fmt, ...) {
    if (!config_.log_callback) return;
    if (level < config_.log_level) return;

    char buf[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    config_.log_callback(level, buf, config_.log_ctx);
}

// ---------------------------------------------------------------------------
// configure()
// ---------------------------------------------------------------------------

void SessionManager::configure(const SessionConfig& config) {
    // Replace previous state
    tracks_.clear();
    name_map_.clear();
    config_ = config;
    state_.store(ConnectionState::Disconnected);

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

// ---------------------------------------------------------------------------
// connect()
// ---------------------------------------------------------------------------

void SessionManager::connect() {
    // Reject if already connected/connecting
    auto current = state_.load();
    if (current == ConnectionState::Connecting ||
        current == ConnectionState::Connected) {
        return;
    }

    // Parse URL
    std::string host;
    uint16_t port = 443;
    if (!parse_url(config_.server_url, host, port)) {
        log(LogLevel::Error, "Failed to parse server URL: %s",
            config_.server_url.c_str());
        state_.store(ConnectionState::Failed);
        if (config_.status_callback) {
            config_.status_callback(ConnectionState::Failed,
                                    "Invalid server URL", config_.status_ctx);
        }
        return;
    }

    // Create transport
    transport_ = std::make_unique<MoqTransport>();

    // Set up transport config
    TransportConfig tc;
    tc.host = host;
    tc.port = port;
    tc.skip_cert_validation = true;  // dev/self-signed certs

    // Set up transport callbacks
    TransportCallbacks cb;

    cb.on_state_changed = [this](TransportState ts, const char* message) {
        // Map TransportState → ConnectionState
        ConnectionState cs;
        switch (ts) {
        case TransportState::Disconnected:
            cs = ConnectionState::Disconnected;
            break;
        case TransportState::Connecting:
        case TransportState::Connected:  // still doing MOQ handshake
            cs = ConnectionState::Connecting;
            break;
        case TransportState::Ready:
            cs = ConnectionState::Connected;
            break;
        case TransportState::Failed:
            cs = ConnectionState::Failed;
            break;
        }
        state_.store(cs);
        if (config_.status_callback) {
            config_.status_callback(cs, message, config_.status_ctx);
        }
    };

    cb.on_control_message = [this](uint64_t message_type,
                                    const uint8_t* content,
                                    int32_t content_len) {
        handle_control_message(message_type, content, content_len);
    };

    cb.on_datagram = [this](uint64_t track_alias, uint64_t /*group_id*/,
                             uint64_t /*object_id*/, uint8_t /*priority*/,
                             const uint8_t* /*payload*/,
                             int32_t /*payload_len*/) {
        // Phase 4c will add decode + dispatch. For now just verify alias lookup.
        auto it = alias_map_.find(track_alias);
        if (it == alias_map_.end()) {
            log(LogLevel::Warn, "Datagram for unknown alias %llu",
                static_cast<unsigned long long>(track_alias));
        }
    };

    // Connect transport
    if (!transport_->connect(tc, cb)) {
        log(LogLevel::Error, "Transport connect failed");
        transport_.reset();
        // State already set to Failed by transport callback
        return;
    }

    // Start session thread
    session_running_.store(true);
    session_thread_ = std::make_unique<std::thread>(
        &SessionManager::session_thread_func, this);
}

// ---------------------------------------------------------------------------
// disconnect()
// ---------------------------------------------------------------------------

void SessionManager::disconnect() {
    // Stop session thread
    if (session_running_.load()) {
        session_running_.store(false);
        if (session_thread_ && session_thread_->joinable()) {
            session_thread_->join();
        }
        session_thread_.reset();
    }

    // Disconnect transport
    if (transport_) {
        transport_->disconnect();
        transport_.reset();
    }

    // Clear orchestration state
    request_id_map_.clear();
    alias_map_.clear();
    next_request_id_ = 0;
    next_track_alias_ = 1;
    orchestration_started_ = false;
    first_subscribe_sent_ = false;

    // Reset track MOQ state
    for (auto& t : tracks_) {
        t->moq_track_alias = 0;
        t->moq_request_id = 0;
        t->next_object_id = 0;
        t->next_group_id = 0;
    }

    state_.store(ConnectionState::Disconnected);
    if (config_.status_callback) {
        config_.status_callback(ConnectionState::Disconnected,
                                "Disconnected", config_.status_ctx);
    }
}

// ---------------------------------------------------------------------------
// Session thread
// ---------------------------------------------------------------------------

void SessionManager::session_thread_func() {
    while (session_running_.load()) {
        if (transport_) {
            // Drain msquic queues, fires on_control_message callback
            transport_->process_incoming();

            // If transport is Ready and we haven't orchestrated yet, start
            if (!orchestration_started_ &&
                transport_->state() == TransportState::Ready) {
                start_orchestration();
            }
        }

        std::this_thread::sleep_for(SESSION_POLL_INTERVAL);
    }
}

// ---------------------------------------------------------------------------
// start_orchestration()
// ---------------------------------------------------------------------------

void SessionManager::start_orchestration() {
    orchestration_started_ = true;
    log(LogLevel::Info, "Starting MOQ orchestration");

    // SUBSCRIBE to each inbound track
    for (auto& t : tracks_) {
        if (t->config.direction == TrackDirection::Inbound) {
            moq::SubscribeConfig sub;
            sub.request_id = next_request_id_;
            sub.track_namespace = t->config.moq_namespace;
            sub.track_name = t->config.moq_track_name;
            sub.filter_type = moq::kFilterLatestGroup;

            // Attach JWT on first SUBSCRIBE only
            if (!first_subscribe_sent_ && !config_.jwt.empty()) {
                sub.authorization = config_.jwt;
                first_subscribe_sent_ = true;
            }

            t->moq_request_id = next_request_id_;
            request_id_map_[next_request_id_] = t.get();
            next_request_id_ += 2;  // even sequence

            auto msg = moq::build_subscribe(sub);
            auto framed = moq::build_control_message(
                moq::MessageType::Subscribe, msg);
            transport_->send_control(framed.data(),
                                     static_cast<uint32_t>(framed.size()));

            log(LogLevel::Debug, "SUBSCRIBE sent for track '%s' (req_id=%llu)",
                t->config.name.c_str(),
                static_cast<unsigned long long>(t->moq_request_id));
        }
    }

    // ANNOUNCE each outbound track namespace
    for (auto& t : tracks_) {
        if (t->config.direction == TrackDirection::Outbound) {
            moq::AnnounceConfig ann;
            ann.request_id = next_request_id_;
            ann.track_namespace = t->config.moq_namespace;

            t->moq_request_id = next_request_id_;
            request_id_map_[next_request_id_] = t.get();
            next_request_id_ += 2;

            auto msg = moq::build_announce(ann);
            auto framed = moq::build_control_message(
                moq::MessageType::Announce, msg);
            transport_->send_control(framed.data(),
                                     static_cast<uint32_t>(framed.size()));

            log(LogLevel::Debug, "ANNOUNCE sent for track '%s' (req_id=%llu)",
                t->config.name.c_str(),
                static_cast<unsigned long long>(t->moq_request_id));
        }
    }
}

// ---------------------------------------------------------------------------
// Control message dispatch
// ---------------------------------------------------------------------------

void SessionManager::handle_control_message(uint64_t message_type,
                                             const uint8_t* content,
                                             int32_t content_len) {
    auto type = static_cast<moq::MessageType>(message_type);

    switch (type) {

    case moq::MessageType::SubscribeOk: {
        moq::SubscribeOkResult result;
        if (!moq::parse_subscribe_ok(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse SUBSCRIBE_OK");
            break;
        }

        auto it = request_id_map_.find(result.request_id);
        if (it != request_id_map_.end()) {
            TrackHandle* handle = it->second;
            handle->moq_track_alias = result.track_alias;
            alias_map_[result.track_alias] = handle;
            log(LogLevel::Info,
                "SUBSCRIBE_OK: track '%s' alias=%llu",
                handle->config.name.c_str(),
                static_cast<unsigned long long>(result.track_alias));
        } else {
            log(LogLevel::Warn,
                "SUBSCRIBE_OK for unknown req_id=%llu",
                static_cast<unsigned long long>(result.request_id));
        }
        break;
    }

    case moq::MessageType::SubscribeError: {
        moq::SubscribeErrorResult result;
        if (!moq::parse_subscribe_error(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse SUBSCRIBE_ERROR");
            break;
        }

        auto it = request_id_map_.find(result.request_id);
        const char* track_name = (it != request_id_map_.end())
                                     ? it->second->config.name.c_str()
                                     : "unknown";
        log(LogLevel::Error,
            "SUBSCRIBE_ERROR: track '%s' code=%llu reason='%s'",
            track_name,
            static_cast<unsigned long long>(result.error_code),
            result.reason.c_str());
        break;
    }

    case moq::MessageType::Subscribe: {
        // Server subscribing to one of our announced tracks
        moq::SubscribeResult result;
        if (!moq::parse_subscribe(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse incoming SUBSCRIBE");
            break;
        }

        // Match namespace to outbound track
        TrackHandle* matched = nullptr;
        for (auto& t : tracks_) {
            if (t->config.direction == TrackDirection::Outbound &&
                t->config.moq_namespace == result.track_namespace) {
                matched = t.get();
                break;
            }
        }

        if (matched) {
            uint64_t alias = next_track_alias_++;
            matched->moq_track_alias = alias;
            alias_map_[alias] = matched;

            // Send SUBSCRIBE_OK
            moq::SubscribeOkConfig ok;
            ok.request_id = result.request_id;
            ok.track_alias = alias;

            auto msg = moq::build_subscribe_ok(ok);
            auto framed = moq::build_control_message(
                moq::MessageType::SubscribeOk, msg);
            transport_->send_control(framed.data(),
                                     static_cast<uint32_t>(framed.size()));

            log(LogLevel::Info,
                "Incoming SUBSCRIBE matched track '%s', assigned alias=%llu",
                matched->config.name.c_str(),
                static_cast<unsigned long long>(alias));
        } else {
            log(LogLevel::Warn,
                "Incoming SUBSCRIBE for unmatched namespace, ignoring");
        }
        break;
    }

    case moq::MessageType::Announce: {
        // Server announcing — we just ACK
        moq::AnnounceResult result;
        if (!moq::parse_announce(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse incoming ANNOUNCE");
            break;
        }

        auto msg = moq::build_announce_ok(result.request_id);
        auto framed = moq::build_control_message(
            moq::MessageType::AnnounceOk, msg);
        transport_->send_control(framed.data(),
                                 static_cast<uint32_t>(framed.size()));

        log(LogLevel::Debug, "ANNOUNCE_OK sent for req_id=%llu",
            static_cast<unsigned long long>(result.request_id));
        break;
    }

    case moq::MessageType::AnnounceOk: {
        moq::AnnounceOkResult result;
        if (!moq::parse_announce_ok(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse ANNOUNCE_OK");
            break;
        }

        auto it = request_id_map_.find(result.request_id);
        if (it != request_id_map_.end()) {
            log(LogLevel::Info, "ANNOUNCE_OK for track '%s'",
                it->second->config.name.c_str());
        } else {
            log(LogLevel::Debug, "ANNOUNCE_OK for req_id=%llu",
                static_cast<unsigned long long>(result.request_id));
        }
        break;
    }

    case moq::MessageType::SubscribeAnnounces: {
        moq::SubscribeAnnouncesResult result;
        if (!moq::parse_subscribe_announces(content, content_len, result)) {
            log(LogLevel::Warn, "Failed to parse SUBSCRIBE_ANNOUNCES");
            break;
        }

        auto msg = moq::build_subscribe_announces_ok(result.request_id);
        auto framed = moq::build_control_message(
            moq::MessageType::SubscribeAnnouncesOk, msg);
        transport_->send_control(framed.data(),
                                 static_cast<uint32_t>(framed.size()));

        log(LogLevel::Debug, "SUBSCRIBE_ANNOUNCES_OK sent for req_id=%llu",
            static_cast<unsigned long long>(result.request_id));
        break;
    }

    case moq::MessageType::ServerSetup:
        // Already handled by MoqTransport (transitions to Ready), ignore here
        break;

    default:
        log(LogLevel::Debug, "Unhandled control message type=0x%llx",
            static_cast<unsigned long long>(message_type));
        break;
    }
}

// ---------------------------------------------------------------------------
// Phase 4c stubs
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

ConnectionState SessionManager::get_connection_state() const {
    return state_.load();
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
    return {state_.load(), 0, 0, 0, 0, 0, 0.0};
}

}  // namespace panaudia
