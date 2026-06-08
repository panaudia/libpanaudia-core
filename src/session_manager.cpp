#include "panaudia/session_manager.h"
#include "panaudia/cache_map.h"
#include "panaudia/moq_protocol.h"
#include "panaudia/topic_merger.h"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <vector>

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

                // Pre-allocate send buffers
                uint32_t frame_size_samples;
                if (tc.codec == AudioCodec::Opus) {
                    frame_size_samples =
                        (tc.opus_frame_size_ms * tc.sample_rate) / 1000;
                } else {
                    frame_size_samples =
                        (tc.pcm_frame_size_ms * tc.sample_rate) / 1000;
                }
                handle->pcm_read_buffer.resize(frame_size_samples * tc.channels);

                if (tc.codec == AudioCodec::Opus) {
                    handle->encode_output_buffer.resize(512);
                } else {
                    handle->encode_output_buffer.resize(
                        frame_size_samples * tc.channels * sizeof(float));
                }
                handle->datagram_buffer.resize(
                    34 + handle->encode_output_buffer.size());
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

                // Pre-allocate decode buffer: 960 per-channel samples
                // covers up to 20ms @ 48kHz
                handle->decode_buffer.resize(960 * tc.channels);
            }
        }
        // Data tracks: no buffers or codecs needed.
        // For cached inbound data tracks, attach a TopicMerger that will
        // gate by op_id and surface accepted/tombstoned ops via the
        // cache callbacks on SessionConfig.
        if (tc.type == TrackType::Data
            && tc.direction == TrackDirection::Inbound
            && tc.cached) {
            handle->merger = std::make_unique<TopicMerger>();
        }

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
// make_transport_callbacks()
// ---------------------------------------------------------------------------

TransportCallbacks SessionManager::make_transport_callbacks() {
    TransportCallbacks cb;

    cb.on_state_changed = [this](TransportState ts, const char* message) {
        handle_transport_state_change(ts, message);
    };

    cb.on_control_message = [this](uint64_t message_type,
                                    const uint8_t* content,
                                    int32_t content_len) {
        handle_control_message(message_type, content, content_len);
    };

    cb.on_datagram = [this](uint64_t track_alias, uint64_t /*group_id*/,
                             uint64_t /*object_id*/, uint8_t /*priority*/,
                             const uint8_t* payload,
                             int32_t payload_len) {
        auto it = inbound_alias_map_.find(track_alias);
        if (it == inbound_alias_map_.end()) {
            log(LogLevel::Warn, "Datagram for unknown alias %llu",
                static_cast<unsigned long long>(track_alias));
            return;
        }
        TrackHandle* track = it->second;
        if (track->config.type == TrackType::Audio) {
            dispatch_audio_datagram(track, payload, payload_len);
        } else {
            dispatch_data_datagram(track, payload, payload_len);
        }
    };

    return cb;
}

// ---------------------------------------------------------------------------
// handle_transport_state_change()
// ---------------------------------------------------------------------------

void SessionManager::handle_transport_state_change(TransportState ts,
                                                    const char* message) {
    if (reconnecting_) {
        // During reconnection, suppress intermediate state callbacks
        switch (ts) {
        case TransportState::Disconnected:
            // Old transport cleanup — ignore
            break;
        case TransportState::Connecting:
        case TransportState::Connected:
            // Still reconnecting — stay in Reconnecting state
            break;
        case TransportState::Ready:
            // Reconnect succeeded!
            reconnecting_ = false;
            reconnect_attempt_ = 0;
            state_.store(ConnectionState::Connected);
            log(LogLevel::Info, "Reconnected successfully (total reconnects: %u)",
                total_reconnect_count_);
            if (config_.status_callback) {
                config_.status_callback(ConnectionState::Connected,
                                        "Reconnected", config_.status_ctx);
            }
            break;
        case TransportState::Failed:
            // Schedule next attempt or give up
            if (reconnect_attempt_ < config_.max_reconnect_attempts) {
                uint32_t delay = calculate_reconnect_delay_ms();
                reconnect_deadline_ = std::chrono::steady_clock::now() +
                                      std::chrono::milliseconds(delay);
                log(LogLevel::Info,
                    "Reconnect attempt %u failed, retrying in %ums",
                    reconnect_attempt_, delay);
            } else {
                reconnecting_ = false;
                state_.store(ConnectionState::Failed);
                log(LogLevel::Error,
                    "Reconnection failed after %u attempts", reconnect_attempt_);
                if (config_.status_callback) {
                    config_.status_callback(ConnectionState::Failed,
                                            "Reconnection failed",
                                            config_.status_ctx);
                }
            }
            break;
        }
        return;
    }

    // Normal (not reconnecting) state mapping
    ConnectionState cs = ConnectionState::Failed;
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
        reconnect_attempt_ = 0;  // reset on successful connection
        break;
    case TransportState::Failed:
        if (config_.max_reconnect_attempts > 0 &&
            !manual_disconnect_.load()) {
            // Start reconnection cycle
            reconnecting_ = true;
            reconnect_attempt_ = 0;
            cs = ConnectionState::Reconnecting;
            state_.store(cs);
            log(LogLevel::Info,
                "Connection lost, starting reconnection (max %u attempts)",
                config_.max_reconnect_attempts);
            if (config_.status_callback) {
                config_.status_callback(cs, message, config_.status_ctx);
            }
            // Schedule first attempt immediately
            reconnect_deadline_ = std::chrono::steady_clock::now();
            return;
        }
        cs = ConnectionState::Failed;
        break;
    }
    state_.store(cs);
    if (config_.status_callback) {
        config_.status_callback(cs, message, config_.status_ctx);
    }
}

// ---------------------------------------------------------------------------
// reset_moq_state()
// ---------------------------------------------------------------------------

void SessionManager::reset_moq_state() {
    request_id_map_.clear();
    inbound_alias_map_.clear();
    outbound_alias_map_.clear();
    next_request_id_ = 0;
    next_track_alias_ = 1;
    orchestration_started_ = false;
    first_subscribe_sent_ = false;

    for (auto& t : tracks_) {
        t->moq_track_alias = 0;
        t->moq_request_id = 0;
        t->next_object_id = 0;
        t->next_group_id = 0;
    }
}

// ---------------------------------------------------------------------------
// calculate_reconnect_delay_ms()
// ---------------------------------------------------------------------------

uint32_t SessionManager::calculate_reconnect_delay_ms() const {
    // delay = base * 2^attempt, capped at max
    uint32_t delay = config_.reconnect_base_delay_ms;
    for (uint32_t i = 0; i < reconnect_attempt_; i++) {
        delay *= 2;
        if (delay >= config_.reconnect_max_delay_ms) {
            return config_.reconnect_max_delay_ms;
        }
    }
    return delay;
}

// ---------------------------------------------------------------------------
// attempt_reconnect()
// ---------------------------------------------------------------------------

void SessionManager::attempt_reconnect() {
    reconnect_attempt_++;
    total_reconnect_count_++;

    // Check max attempts
    if (reconnect_attempt_ > config_.max_reconnect_attempts) {
        reconnecting_ = false;
        state_.store(ConnectionState::Failed);
        log(LogLevel::Error,
            "Reconnection failed after %u attempts", reconnect_attempt_ - 1);
        if (config_.status_callback) {
            config_.status_callback(ConnectionState::Failed,
                                    "Reconnection failed",
                                    config_.status_ctx);
        }
        return;
    }

    log(LogLevel::Info, "Reconnect attempt %u/%u",
        reconnect_attempt_, config_.max_reconnect_attempts);

    // Tear down old transport
    if (transport_) {
        transport_->disconnect();
        transport_.reset();
    }

    // Reset MOQ state
    reset_moq_state();

    // Flush outbound ring buffers (discard stale audio)
    for (auto& t : tracks_) {
        if (t->ring_buffer) {
            t->ring_buffer->flush();
        }
    }

    // Create new transport and connect
    transport_ = std::make_unique<MoqTransport>();

    TransportConfig tc;
    tc.host = parsed_host_;
    tc.port = parsed_port_;
    tc.skip_cert_validation = true;

    auto cb = make_transport_callbacks();

    if (!transport_->connect(tc, cb)) {
        log(LogLevel::Warn, "Reconnect transport init failed");
        transport_.reset();
        // Schedule next attempt
        uint32_t delay = calculate_reconnect_delay_ms();
        reconnect_deadline_ = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(delay);
        log(LogLevel::Info, "Next reconnect in %ums", delay);
    }
}

// ---------------------------------------------------------------------------
// connect()
// ---------------------------------------------------------------------------

void SessionManager::connect() {
    // Reject if already connected/connecting/reconnecting
    auto current = state_.load();
    if (current == ConnectionState::Connecting ||
        current == ConnectionState::Connected ||
        current == ConnectionState::Reconnecting) {
        return;
    }

    // Parse URL
    if (!parse_url(config_.server_url, parsed_host_, parsed_port_)) {
        log(LogLevel::Error, "Failed to parse server URL: %s",
            config_.server_url.c_str());
        state_.store(ConnectionState::Failed);
        if (config_.status_callback) {
            config_.status_callback(ConnectionState::Failed,
                                    "Invalid server URL", config_.status_ctx);
        }
        return;
    }

    // Reset reconnection state
    manual_disconnect_.store(false);
    reconnecting_ = false;
    reconnect_attempt_ = 0;
    total_reconnect_count_ = 0;

    // Create transport
    transport_ = std::make_unique<MoqTransport>();

    // Set up transport config
    TransportConfig tc;
    tc.host = parsed_host_;
    tc.port = parsed_port_;
    tc.skip_cert_validation = true;  // dev/self-signed certs

    auto cb = make_transport_callbacks();

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
    // Nothing to do if already fully disconnected (prevents destructor
    // from re-firing status callback after explicit disconnect)
    if (state_.load() == ConnectionState::Disconnected &&
        !session_running_.load() && !transport_) {
        return;
    }

    // Stop reconnection cycle
    manual_disconnect_.store(true);
    reconnecting_ = false;

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
    reset_moq_state();

    // Flush outbound ring buffers
    for (auto& t : tracks_) {
        if (t->ring_buffer) {
            t->ring_buffer->flush();
        }
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
        // Reconnection timer check
        if (reconnecting_ &&
            std::chrono::steady_clock::now() >= reconnect_deadline_) {
            attempt_reconnect();
        }

        if (transport_) {
            // Drain msquic queues, fires on_control_message callback
            transport_->process_incoming();

            // If transport is Ready and we haven't orchestrated yet, start
            if (!orchestration_started_ &&
                transport_->state() == TransportState::Ready) {
                start_orchestration();
            }

            // Poll outbound audio ring buffers → encode → send
            if (state_.load() == ConnectionState::Connected) {
                poll_outbound_tracks();
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

            // Cached tracks send their resume opID. On first connect this
            // is 0 (server treats it as "send full snapshot"); on reconnect
            // it filters down to ops the client hasn't seen yet.
            if (t->merger) {
                sub.extra_params.push_back(
                    moq::make_resume_op_id_param(t->merger->resume_op_id()));
            }

            t->moq_request_id = next_request_id_;
            request_id_map_[next_request_id_] = t.get();
            next_request_id_ += 2;  // even sequence

            auto framed = moq::build_subscribe(sub);
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

            auto framed = moq::build_announce(ann);
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
            inbound_alias_map_[result.track_alias] = handle;
            log(LogLevel::Info,
                "SUBSCRIBE_OK: track '%s' req_id=%llu alias=%llu",
                handle->config.name.c_str(),
                static_cast<unsigned long long>(result.request_id),
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
            outbound_alias_map_[alias] = matched;

            // Send SUBSCRIBE_OK
            moq::SubscribeOkConfig ok;
            ok.request_id = result.request_id;
            ok.track_alias = alias;

            auto framed = moq::build_subscribe_ok(ok);
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

        auto framed = moq::build_announce_ok(result.request_id);
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

        auto framed = moq::build_subscribe_announces_ok(result.request_id);
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
// Phase 4c: write_audio / read_audio / send_data
// ---------------------------------------------------------------------------

void SessionManager::write_audio(TrackHandle* track,
                                  const float* samples,
                                  uint32_t frame_count,
                                  uint64_t /*host_time*/) {
    if (!track || !track->ring_buffer) return;
    if (track->config.type != TrackType::Audio) return;
    if (track->config.direction != TrackDirection::Outbound) return;
    track->ring_buffer->write(samples, frame_count);
}

uint32_t SessionManager::read_audio(TrackHandle* track,
                                     float* buffer,
                                     uint32_t frame_count,
                                     uint64_t /*host_time*/) {
    if (!track || !track->jitter_buffer) return 0;
    if (track->config.type != TrackType::Audio) return 0;
    if (track->config.direction != TrackDirection::Inbound) return 0;
    uint32_t float_count = frame_count * track->config.channels;
    bool ok = track->jitter_buffer->read(buffer, float_count);
    return ok ? frame_count : 0;
}

void SessionManager::send_data(TrackHandle* track,
                                const uint8_t* data,
                                uint32_t data_len) {
    if (!track || !transport_) return;
    if (track->config.type != TrackType::Data) return;
    if (track->moq_track_alias == 0) return;
    if (state_.load() != ConnectionState::Connected) return;

    auto dg = moq::build_object_datagram(
        track->moq_track_alias, track->next_group_id,
        track->next_object_id++, 128, data, data_len);
    transport_->send_datagram(dg.data(), static_cast<uint32_t>(dg.size()));
}

// ---------------------------------------------------------------------------
// Phase 4c: poll_outbound_tracks / send_audio_frame / send_pcm_frame
// ---------------------------------------------------------------------------

void SessionManager::poll_outbound_tracks() {
    for (auto& t : tracks_) {
        if (t->config.direction != TrackDirection::Outbound) continue;
        if (t->config.type != TrackType::Audio) continue;
        if (t->moq_track_alias == 0) continue;

        if (t->config.codec == AudioCodec::Opus) {
            send_audio_frame(t.get());
        } else {
            send_pcm_frame(t.get());
        }
    }
}

void SessionManager::send_audio_frame(TrackHandle* track) {
    if (!track->encoder || !track->ring_buffer) return;

    uint32_t frame_size = track->encoder->frame_size_samples();
    while (track->ring_buffer->read_available() >= frame_size) {
        // Read PCM from ring buffer
        track->ring_buffer->read(track->pcm_read_buffer.data(), frame_size);

        // Opus encode
        int encoded_bytes = track->encoder->encode(
            track->pcm_read_buffer.data(), frame_size,
            track->encode_output_buffer.data(),
            static_cast<uint32_t>(track->encode_output_buffer.size()));

        if (encoded_bytes <= 0) continue;

        // Build datagram header into pre-allocated buffer
        int32_t header_len = moq::build_object_datagram_header(
            track->moq_track_alias, track->next_group_id,
            track->next_object_id, 128, track->datagram_buffer.data());

        // Copy encoded payload after header
        std::memcpy(track->datagram_buffer.data() + header_len,
                     track->encode_output_buffer.data(), encoded_bytes);

        // Send
        transport_->send_datagram(
            track->datagram_buffer.data(),
            static_cast<uint32_t>(header_len + encoded_bytes));

        track->next_object_id++;
    }
}

void SessionManager::send_pcm_frame(TrackHandle* track) {
    if (!track->ring_buffer) return;

    uint32_t frame_size =
        (track->config.pcm_frame_size_ms * track->config.sample_rate) / 1000;
    while (track->ring_buffer->read_available() >= frame_size) {
        // Read PCM from ring buffer
        track->ring_buffer->read(track->pcm_read_buffer.data(), frame_size);

        // Frame as bytes
        uint32_t payload_bytes = pcm_frame(
            track->pcm_read_buffer.data(), frame_size,
            track->config.channels, track->encode_output_buffer.data());

        // Build datagram header
        int32_t header_len = moq::build_object_datagram_header(
            track->moq_track_alias, track->next_group_id,
            track->next_object_id, 128, track->datagram_buffer.data());

        // Copy payload after header
        std::memcpy(track->datagram_buffer.data() + header_len,
                     track->encode_output_buffer.data(), payload_bytes);

        // Send
        transport_->send_datagram(
            track->datagram_buffer.data(),
            static_cast<uint32_t>(header_len + payload_bytes));

        track->next_object_id++;
    }
}

// ---------------------------------------------------------------------------
// Phase 4c: dispatch_audio_datagram / dispatch_data_datagram
// ---------------------------------------------------------------------------

void SessionManager::dispatch_audio_datagram(TrackHandle* track,
                                              const uint8_t* payload,
                                              int32_t payload_len) {
    if (!track->jitter_buffer) return;
    if (payload_len <= 0) return;

    if (track->config.codec == AudioCodec::Opus) {
        if (!track->decoder) return;
        int frames_decoded = track->decoder->decode(
            payload, static_cast<uint32_t>(payload_len),
            track->decode_buffer.data(),
            static_cast<uint32_t>(track->decode_buffer.size() /
                                   track->config.channels));

        if (frames_decoded > 0) {
            uint32_t float_count =
                static_cast<uint32_t>(frames_decoded) * track->config.channels;
            track->jitter_buffer->write(track->decode_buffer.data(),
                                         float_count);
        }
    } else {
        // PCM: unframe bytes → floats
        uint32_t frames_decoded = pcm_unframe(
            payload, static_cast<uint32_t>(payload_len),
            track->decode_buffer.data(), track->config.channels);

        if (frames_decoded > 0) {
            uint32_t float_count = frames_decoded * track->config.channels;
            track->jitter_buffer->write(track->decode_buffer.data(),
                                         float_count);
        }
    }
}

void SessionManager::dispatch_data_datagram(TrackHandle* track,
                                              const uint8_t* payload,
                                              int32_t payload_len) {
    if (payload_len <= 0) return;

    // Cached track: try cache-envelope path first.
    if (track->merger) {
        auto result = track->merger->apply_envelope(
            payload, static_cast<size_t>(payload_len));
        if (result.has_value()) {
            const uint64_t op_id = track->merger->resume_op_id();

            if (!result->accepted.empty() && config_.cache_values_callback) {
                std::vector<CacheValueView> views;
                views.reserve(result->accepted.size());
                for (const auto& v : result->accepted) {
                    CacheValueView vv;
                    vv.key       = v.key.data();
                    vv.key_len   = static_cast<uint32_t>(v.key.size());
                    vv.value     = v.value.data();
                    vv.value_len = static_cast<uint32_t>(v.value.size());
                    vv.node_id   = 0;  // see TODO below
                    views.push_back(vv);
                }
                config_.cache_values_callback(
                    track, views.data(),
                    static_cast<uint32_t>(views.size()),
                    op_id, config_.cache_values_ctx);
            }

            if (!result->tombstoned.empty() && config_.cache_removed_callback) {
                std::vector<const char*> ptrs;
                ptrs.reserve(result->tombstoned.size());
                for (const auto& k : result->tombstoned) {
                    ptrs.push_back(k.c_str());
                }
                config_.cache_removed_callback(
                    track, ptrs.data(),
                    static_cast<uint32_t>(ptrs.size()),
                    op_id, config_.cache_removed_ctx);
            }
            return;
        }
        // Fall-through: payload was not a cache envelope. Treat as raw —
        // matches TS behaviour that preserves backward compat with
        // pre-cache servers.
    }

    if (config_.data_recv_callback) {
        config_.data_recv_callback(
            track, payload, static_cast<uint32_t>(payload_len),
            config_.data_recv_ctx);
    }
}

const CacheMap* SessionManager::get_cache_map(TrackHandle* track) const {
    if (!track || !track->merger) return nullptr;
    return track->merger->cache().get();
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
    return {state_.load(), 0, 0, 0, 0, 0, 0.0, total_reconnect_count_};
}

}  // namespace panaudia
