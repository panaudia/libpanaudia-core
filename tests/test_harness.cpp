// test_harness.cpp — Phase 5: Standalone integration test harness
//
// Usage: panaudia-test-harness --server <url> --jwt <token> [options]
//
// Simulates RT audio callbacks at 48kHz, generates test audio,
// measures timing jitter, and reports statistics.

#include "panaudia/core.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <numeric>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifndef _WIN32
#include <pthread.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

using Clock = std::chrono::steady_clock;
using namespace panaudia;

// ============================================================================
// Signal handling
// ============================================================================

static std::atomic<bool> g_running{true};

static void signal_handler(int) {
    g_running = false;
}

// ============================================================================
// Utilities
// ============================================================================

static const char* connection_state_str(ConnectionState s) {
    switch (s) {
        case ConnectionState::Disconnected: return "Disconnected";
        case ConnectionState::Connecting:   return "Connecting";
        case ConnectionState::Connected:    return "Connected";
        case ConnectionState::Reconnecting: return "Reconnecting";
        case ConnectionState::Failed:       return "Failed";
    }
    return "Unknown";
}

static const char* log_level_str(LogLevel l) {
    switch (l) {
        case LogLevel::Trace: return "TRACE";
        case LogLevel::Debug: return "DEBUG";
        case LogLevel::Info:  return "INFO";
        case LogLevel::Warn:  return "WARN";
        case LogLevel::Error: return "ERROR";
    }
    return "?";
}

static LogLevel parse_log_level(const char* s) {
    if (!strcmp(s, "trace")) return LogLevel::Trace;
    if (!strcmp(s, "debug")) return LogLevel::Debug;
    if (!strcmp(s, "info"))  return LogLevel::Info;
    if (!strcmp(s, "warn"))  return LogLevel::Warn;
    if (!strcmp(s, "error")) return LogLevel::Error;
    std::fprintf(stderr, "Unknown log level '%s', using info\n", s);
    return LogLevel::Info;
}

// Base64url decode (RFC 4648 §5, no padding required)
static std::string base64url_decode(const std::string& input) {
    std::string b64 = input;
    for (auto& c : b64) {
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
    }
    while (b64.size() % 4) b64 += '=';

    static const std::string chars =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    int val = 0, bits = -8;
    for (unsigned char c : b64) {
        if (c == '=') break;
        auto pos = chars.find(c);
        if (pos == std::string::npos) continue;
        val = (val << 6) | static_cast<int>(pos);
        bits += 6;
        if (bits >= 0) {
            result += static_cast<char>((val >> bits) & 0xFF);
            bits -= 8;
        }
    }
    return result;
}

// Extract "jti" field from JWT payload (second dot-separated segment)
static std::string extract_node_id_from_jwt(const std::string& jwt) {
    auto dot1 = jwt.find('.');
    if (dot1 == std::string::npos) return "";
    auto dot2 = jwt.find('.', dot1 + 1);
    if (dot2 == std::string::npos) return "";

    auto payload = base64url_decode(jwt.substr(dot1 + 1, dot2 - dot1 - 1));

    // Simple JSON field extraction (no external JSON library)
    auto pos = payload.find("\"jti\"");
    if (pos == std::string::npos) return "";
    pos = payload.find(':', pos);
    if (pos == std::string::npos) return "";
    pos = payload.find('"', pos + 1);
    if (pos == std::string::npos) return "";
    auto end = payload.find('"', pos + 1);
    if (end == std::string::npos) return "";

    return payload.substr(pos + 1, end - pos - 1);
}

// ============================================================================
// Configuration
// ============================================================================

enum class SignalType { Sine, Noise, Silence };

struct HarnessConfig {
    std::string server_url;
    std::string jwt;
    std::string node_id;       // extracted from JWT or provided via --node-id

    uint32_t tracks_out = 1;   // outbound mono audio tracks
    uint32_t tracks_in = 1;    // inbound audio tracks
    uint32_t channels_in = 2;  // channels per inbound track (stereo default)
    AudioCodec codec = AudioCodec::Opus;
    uint32_t frame_ms = 5;     // codec frame size
    uint32_t duration_secs = 60;
    bool data_tracks = true;
    LogLevel log_level = LogLevel::Info;
    SignalType signal = SignalType::Sine;
    double sine_freq = 440.0;
    uint32_t sample_rate = 48000;
};

static void print_usage(const char* prog) {
    std::printf(
        "Usage: %s --server <url> --jwt <token> [options]\n"
        "\n"
        "Required:\n"
        "  --server <url>       Panaudia server URL (e.g. https://test.panaudia.com)\n"
        "  --jwt <token>        JWT for authentication (node ID extracted from jti field)\n"
        "\n"
        "Options:\n"
        "  --node-id <id>       Override node ID (default: extracted from JWT jti)\n"
        "  --tracks-out <N>     Outbound mono audio tracks (default: 1)\n"
        "  --tracks-in <N>      Inbound audio tracks (default: 1)\n"
        "  --channels-in <N>    Channels per inbound track (default: 2 = stereo)\n"
        "  --codec <opus|pcm>   Audio codec (default: opus)\n"
        "  --frame-ms <N>       Codec frame size in ms (default: 5)\n"
        "  --duration <secs>    Test duration (default: 60)\n"
        "  --no-data-tracks     Disable state/control data tracks\n"
        "  --log-level <level>  trace|debug|info|warn|error (default: info)\n"
        "  --signal <type>      sine|noise|silence (default: sine)\n"
        "  --freq <hz>          Sine wave frequency (default: 440)\n"
        "  --help               Show this help\n"
        "\n"
        "Notes:\n"
        "  - Standard Panaudia namespaces are used for the first outbound/inbound track.\n"
        "  - Additional tracks (tracks-out > 1) use indexed namespaces for client-side\n"
        "    stress testing; the server won't subscribe to them.\n"
        "  - PCM codec is for client-side testing only (server expects Opus).\n",
        prog);
}

static HarnessConfig parse_args(int argc, char** argv) {
    HarnessConfig cfg;

    for (int i = 1; i < argc; ++i) {
        auto arg = [&](const char* name) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "Missing value for %s\n", name);
                std::exit(1);
            }
            return argv[++i];
        };

        if (!strcmp(argv[i], "--server"))         cfg.server_url = arg("--server");
        else if (!strcmp(argv[i], "--jwt"))        cfg.jwt = arg("--jwt");
        else if (!strcmp(argv[i], "--node-id"))    cfg.node_id = arg("--node-id");
        else if (!strcmp(argv[i], "--tracks-out")) cfg.tracks_out = (uint32_t)std::atoi(arg("--tracks-out"));
        else if (!strcmp(argv[i], "--tracks-in"))  cfg.tracks_in = (uint32_t)std::atoi(arg("--tracks-in"));
        else if (!strcmp(argv[i], "--channels-in"))cfg.channels_in = (uint32_t)std::atoi(arg("--channels-in"));
        else if (!strcmp(argv[i], "--codec")) {
            const char* v = arg("--codec");
            if (!strcmp(v, "opus")) cfg.codec = AudioCodec::Opus;
            else if (!strcmp(v, "pcm")) cfg.codec = AudioCodec::PCM;
            else { std::fprintf(stderr, "Unknown codec '%s'\n", v); std::exit(1); }
        }
        else if (!strcmp(argv[i], "--frame-ms"))   cfg.frame_ms = (uint32_t)std::atoi(arg("--frame-ms"));
        else if (!strcmp(argv[i], "--duration"))    cfg.duration_secs = (uint32_t)std::atoi(arg("--duration"));
        else if (!strcmp(argv[i], "--no-data-tracks")) cfg.data_tracks = false;
        else if (!strcmp(argv[i], "--log-level"))   cfg.log_level = parse_log_level(arg("--log-level"));
        else if (!strcmp(argv[i], "--signal")) {
            const char* v = arg("--signal");
            if (!strcmp(v, "sine")) cfg.signal = SignalType::Sine;
            else if (!strcmp(v, "noise")) cfg.signal = SignalType::Noise;
            else if (!strcmp(v, "silence")) cfg.signal = SignalType::Silence;
            else { std::fprintf(stderr, "Unknown signal '%s'\n", v); std::exit(1); }
        }
        else if (!strcmp(argv[i], "--freq"))       cfg.sine_freq = std::atof(arg("--freq"));
        else if (!strcmp(argv[i], "--help"))        { print_usage(argv[0]); std::exit(0); }
        else {
            std::fprintf(stderr, "Unknown option: %s\n", argv[i]);
            print_usage(argv[0]);
            std::exit(1);
        }
    }

    if (cfg.server_url.empty() || cfg.jwt.empty()) {
        std::fprintf(stderr, "Error: --server and --jwt are required\n\n");
        print_usage(argv[0]);
        std::exit(1);
    }

    // Extract node ID from JWT if not provided
    if (cfg.node_id.empty()) {
        cfg.node_id = extract_node_id_from_jwt(cfg.jwt);
        if (cfg.node_id.empty()) {
            std::fprintf(stderr, "Error: Could not extract node ID from JWT.\n"
                                 "Use --node-id to provide it manually.\n");
            std::exit(1);
        }
    }

    return cfg;
}

// ============================================================================
// Callback context (thread-safe state tracking)
// ============================================================================

struct CallbackContext {
    std::atomic<ConnectionState> current_state{ConnectionState::Disconnected};

    std::mutex mtx;
    std::vector<ConnectionState> state_history;

    // Data recv stats
    std::atomic<uint64_t> data_messages_received{0};
    std::atomic<uint64_t> data_bytes_received{0};

    void record_state(ConnectionState s) {
        current_state.store(s, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(mtx);
        state_history.push_back(s);
    }
};

static void status_callback(ConnectionState state, const char* message, void* ctx) {
    auto* cb = static_cast<CallbackContext*>(ctx);
    cb->record_state(state);
    std::printf("[STATUS] %s%s%s\n",
                connection_state_str(state),
                message ? " — " : "",
                message ? message : "");
}

static void data_recv_callback(TrackHandle* /*track*/,
                                const uint8_t* /*data*/, uint32_t data_len,
                                void* ctx) {
    auto* cb = static_cast<CallbackContext*>(ctx);
    cb->data_messages_received.fetch_add(1, std::memory_order_relaxed);
    cb->data_bytes_received.fetch_add(data_len, std::memory_order_relaxed);
}

static void log_callback(LogLevel level, const char* message, void* /*ctx*/) {
    std::printf("[%s] %s\n", log_level_str(level), message);
}

// ============================================================================
// Audio generation
// ============================================================================

static void generate_sine(float* buffer, uint32_t frames, uint32_t channels,
                           double freq, double sample_rate, uint64_t& phase) {
    double phase_inc = 2.0 * M_PI * freq / sample_rate;
    for (uint32_t i = 0; i < frames; ++i) {
        float sample = static_cast<float>(std::sin(phase_inc * static_cast<double>(phase)));
        sample *= 0.5f;  // -6 dBFS to avoid clipping after server mix
        for (uint32_t ch = 0; ch < channels; ++ch) {
            buffer[i * channels + ch] = sample;
        }
        ++phase;
    }
}

static void generate_noise(float* buffer, uint32_t frames, uint32_t channels,
                            std::mt19937& rng) {
    std::uniform_real_distribution<float> dist(-0.5f, 0.5f);
    for (uint32_t i = 0; i < frames * channels; ++i) {
        buffer[i] = dist(rng);
    }
}

static void generate_silence(float* buffer, uint32_t frames, uint32_t channels) {
    std::memset(buffer, 0, frames * channels * sizeof(float));
}

// ============================================================================
// Build SessionConfig from HarnessConfig
// ============================================================================

static SessionConfig build_session_config(const HarnessConfig& cfg,
                                           CallbackContext& cb_ctx) {
    SessionConfig sc;
    sc.server_url = cfg.server_url;
    sc.jwt = cfg.jwt;
    sc.log_callback = log_callback;
    sc.log_ctx = &cb_ctx;
    sc.log_level = cfg.log_level;
    sc.status_callback = status_callback;
    sc.status_ctx = &cb_ctx;
    sc.data_recv_callback = data_recv_callback;
    sc.data_recv_ctx = &cb_ctx;

    const auto& nid = cfg.node_id;

    // --- Outbound audio tracks ---
    for (uint32_t i = 0; i < cfg.tracks_out; ++i) {
        TrackConfig tc;
        tc.name = "audio-out-" + std::to_string(i);
        tc.direction = TrackDirection::Outbound;
        tc.type = TrackType::Audio;
        tc.channels = 1;  // outbound is always mono
        tc.sample_rate = cfg.sample_rate;
        tc.codec = cfg.codec;
        tc.opus_bitrate = 64000;
        tc.opus_frame_size_ms = cfg.frame_ms;
        tc.pcm_frame_size_ms = cfg.frame_ms;

        if (i == 0) {
            // Standard Panaudia namespace
            tc.moq_namespace = {"in", "audio", "opus-mono", nid};
        } else {
            // Indexed namespace for stress testing (server won't subscribe)
            tc.moq_namespace = {"in", "audio", "opus-mono", nid, std::to_string(i)};
        }

        sc.tracks.push_back(std::move(tc));
    }

    // --- Inbound audio tracks ---
    for (uint32_t i = 0; i < cfg.tracks_in; ++i) {
        TrackConfig tc;
        tc.name = "audio-in-" + std::to_string(i);
        tc.direction = TrackDirection::Inbound;
        tc.type = TrackType::Audio;
        tc.channels = cfg.channels_in;
        tc.sample_rate = cfg.sample_rate;
        tc.codec = cfg.codec;
        tc.opus_bitrate = 64000;
        tc.opus_frame_size_ms = cfg.frame_ms;
        tc.pcm_frame_size_ms = cfg.frame_ms;

        if (i == 0) {
            tc.moq_namespace = {"out", "audio", "opus-stereo", nid};
        } else {
            tc.moq_namespace = {"out", "audio", "opus-stereo", nid, std::to_string(i)};
        }

        sc.tracks.push_back(std::move(tc));
    }

    // --- Data tracks ---
    if (cfg.data_tracks) {
        // State output (outbound — client publishes state to server)
        {
            TrackConfig tc;
            tc.name = "state-out";
            tc.direction = TrackDirection::Outbound;
            tc.type = TrackType::Data;
            tc.moq_namespace = {"state", nid};
            sc.tracks.push_back(std::move(tc));
        }
        // State input (inbound — server publishes other nodes' state)
        {
            TrackConfig tc;
            tc.name = "state-in";
            tc.direction = TrackDirection::Inbound;
            tc.type = TrackType::Data;
            tc.moq_namespace = {"out", "state", nid};
            sc.tracks.push_back(std::move(tc));
        }
        // Attributes input (inbound — server publishes participant metadata)
        {
            TrackConfig tc;
            tc.name = "attrs-in";
            tc.direction = TrackDirection::Inbound;
            tc.type = TrackType::Data;
            tc.moq_namespace = {"out", "attributes", nid};
            sc.tracks.push_back(std::move(tc));
        }
        // Control output (outbound — client publishes control messages)
        {
            TrackConfig tc;
            tc.name = "control-out";
            tc.direction = TrackDirection::Outbound;
            tc.type = TrackType::Data;
            tc.moq_namespace = {"in", "control", nid};
            sc.tracks.push_back(std::move(tc));
        }
    }

    return sc;
}

// ============================================================================
// RT thread — simulates audio callback at 48kHz
// ============================================================================

struct RTStats {
    std::mutex mtx;
    std::vector<double> intervals_us;  // actual callback intervals
    double expected_interval_us = 0;

    // Inbound audio analysis
    double inbound_sum_sq = 0;
    uint64_t inbound_sample_count = 0;
    float inbound_peak = 0;
};

static void rt_thread_func(PanaudiaCore& core,
                             const HarnessConfig& cfg,
                             RTStats& stats) {
    // Try to elevate thread priority (requires root on macOS/Linux)
#ifndef _WIN32
#ifdef __APPLE__
    pthread_setname_np("panaudia-rt-sim");
#endif
    {
        struct sched_param param{};
        param.sched_priority = sched_get_priority_max(SCHED_FIFO);
        if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &param) != 0) {
            std::printf("[RT] Warning: could not set real-time priority (run as root for accurate timing)\n");
        }
    }
#endif

    const uint32_t frame_size = cfg.sample_rate * cfg.frame_ms / 1000;
    stats.expected_interval_us = cfg.frame_ms * 1000.0;

    // Per-track outbound buffers and phase counters
    struct OutTrack {
        TrackHandle* handle;
        std::vector<float> buffer;
        uint64_t phase = 0;
    };
    std::vector<OutTrack> out_tracks;
    for (uint32_t i = 0; i < cfg.tracks_out; ++i) {
        OutTrack ot;
        ot.handle = core.get_track("audio-out-" + std::to_string(i));
        ot.buffer.resize(frame_size * 1);  // mono
        out_tracks.push_back(std::move(ot));
    }

    // Per-track inbound buffers
    struct InTrack {
        TrackHandle* handle;
        std::vector<float> buffer;
    };
    std::vector<InTrack> in_tracks;
    for (uint32_t i = 0; i < cfg.tracks_in; ++i) {
        InTrack it;
        it.handle = core.get_track("audio-in-" + std::to_string(i));
        it.buffer.resize(frame_size * cfg.channels_in);
        in_tracks.push_back(std::move(it));
    }

    std::mt19937 rng(42);

    auto next_tick = Clock::now();
    auto prev_tick = next_tick;
    uint64_t host_time = 0;
    bool first = true;

    while (g_running) {
        next_tick += std::chrono::microseconds(cfg.frame_ms * 1000);

        // Measure timing jitter
        auto now = Clock::now();
        if (!first) {
            double interval = std::chrono::duration<double, std::micro>(now - prev_tick).count();
            std::lock_guard<std::mutex> lock(stats.mtx);
            stats.intervals_us.push_back(interval);
        }
        first = false;
        prev_tick = now;

        // Write outbound audio
        for (auto& ot : out_tracks) {
            if (!ot.handle) continue;

            switch (cfg.signal) {
                case SignalType::Sine:
                    generate_sine(ot.buffer.data(), frame_size, 1,
                                  cfg.sine_freq, cfg.sample_rate, ot.phase);
                    break;
                case SignalType::Noise:
                    generate_noise(ot.buffer.data(), frame_size, 1, rng);
                    break;
                case SignalType::Silence:
                    generate_silence(ot.buffer.data(), frame_size, 1);
                    break;
            }

            core.write_audio(ot.handle, ot.buffer.data(), frame_size, host_time);
        }

        // Read inbound audio
        for (auto& it : in_tracks) {
            if (!it.handle) continue;

            uint32_t read = core.read_audio(it.handle, it.buffer.data(),
                                             frame_size, host_time);

            // Analyze received audio
            if (read > 0) {
                uint32_t total_samples = read * cfg.channels_in;
                std::lock_guard<std::mutex> lock(stats.mtx);
                for (uint32_t s = 0; s < total_samples; ++s) {
                    float v = it.buffer[s];
                    stats.inbound_sum_sq += static_cast<double>(v) * v;
                    stats.inbound_sample_count++;
                    float abs_v = std::fabs(v);
                    if (abs_v > stats.inbound_peak) {
                        stats.inbound_peak = abs_v;
                    }
                }
            }
        }

        host_time += frame_size;

        // Sleep until next tick
        std::this_thread::sleep_until(next_tick);
    }
}

// ============================================================================
// Data send thread — sends state data at 10Hz
// ============================================================================

struct DataStats {
    std::atomic<uint64_t> messages_sent{0};
};

static void data_send_thread_func(PanaudiaCore& core,
                                    DataStats& data_stats) {
    TrackHandle* state_track = core.get_track("state-out");
    if (!state_track) return;

    // 48-byte NodeInfo3 — position, rotation, volume packed as floats
    // For testing, send an incrementing counter in the position field
    struct alignas(4) NodeInfo3 {
        float pos_x, pos_y, pos_z;
        float euler_x, euler_y, euler_z;
        float volume;
        float reserved[5];  // pad to 48 bytes
    };
    static_assert(sizeof(NodeInfo3) == 48, "NodeInfo3 must be 48 bytes");

    NodeInfo3 state{};
    state.volume = 1.0f;
    uint32_t counter = 0;

    while (g_running) {
        // Update position with a slow circular motion
        float t = static_cast<float>(counter) * 0.1f;
        state.pos_x = std::cos(t) * 2.0f;
        state.pos_z = std::sin(t) * 2.0f;
        state.pos_y = 1.6f;  // head height

        core.send_data(state_track,
                        reinterpret_cast<const uint8_t*>(&state),
                        sizeof(state));

        data_stats.messages_sent.fetch_add(1, std::memory_order_relaxed);
        ++counter;

        std::this_thread::sleep_for(std::chrono::milliseconds(100));  // 10 Hz
    }
}

// ============================================================================
// Report
// ============================================================================

static void print_report(const HarnessConfig& cfg,
                          PanaudiaCore& core,
                          const CallbackContext& cb_ctx,
                          const RTStats& rt_stats,
                          const DataStats& data_stats,
                          double actual_duration_secs) {
    std::printf("\n");
    std::printf("=== Panaudia Test Harness Report ===\n\n");

    // Duration & connection
    std::printf("Duration:    %.1f s\n", actual_duration_secs);
    std::printf("Connection:  %s\n\n",
                connection_state_str(core.get_connection_state()));

    // Session stats
    auto ss = core.get_stats();
    std::printf("Session Stats:\n");
    std::printf("  Bytes sent:       %llu\n", (unsigned long long)ss.bytes_sent);
    std::printf("  Bytes received:   %llu\n", (unsigned long long)ss.bytes_received);
    std::printf("  Packets sent:     %llu\n", (unsigned long long)ss.packets_sent);
    std::printf("  Packets received: %llu\n", (unsigned long long)ss.packets_received);
    std::printf("  Packets lost:     %llu\n", (unsigned long long)ss.packets_lost);
    std::printf("  RTT:              %.1f ms\n", ss.rtt_ms);
    std::printf("  Reconnects:       %u\n\n", ss.reconnect_count);

    // RT callback jitter
    if (!rt_stats.intervals_us.empty()) {
        auto& intervals = rt_stats.intervals_us;
        double sum = 0, sum_sq = 0;
        double min_val = 1e9, max_val = 0;

        for (double v : intervals) {
            double jitter = v - rt_stats.expected_interval_us;
            sum += jitter;
            sum_sq += jitter * jitter;
            min_val = std::min(min_val, v);
            max_val = std::max(max_val, v);
        }

        double n = static_cast<double>(intervals.size());
        double mean_jitter = sum / n;
        double variance = (sum_sq / n) - (mean_jitter * mean_jitter);
        double stddev = std::sqrt(std::max(0.0, variance));

        std::printf("RT Callback Timing:\n");
        std::printf("  Callbacks:     %zu\n", intervals.size());
        std::printf("  Expected:      %.0f µs (%.0f ms)\n",
                    rt_stats.expected_interval_us, rt_stats.expected_interval_us / 1000.0);
        std::printf("  Actual range:  %.0f – %.0f µs\n", min_val, max_val);
        std::printf("  Mean jitter:   %.1f µs\n", mean_jitter);
        std::printf("  Jitter stddev: %.1f µs (%.3f ms)\n\n", stddev, stddev / 1000.0);
    }

    // Audio tracks
    std::printf("Audio Tracks:\n");
    for (uint32_t i = 0; i < cfg.tracks_out; ++i) {
        auto* track = core.get_track("audio-out-" + std::to_string(i));
        if (!track) continue;
        auto bs = core.get_buffer_status(track);
        std::printf("  audio-out-%u (outbound, %s, mono):\n", i,
                    cfg.codec == AudioCodec::Opus ? "opus" : "pcm");
        std::printf("    Buffer: %u/%u frames (%.1f%%)\n",
                    bs.buffered_frames, bs.capacity_frames, bs.fill_ratio * 100.0f);
        std::printf("    Underruns: %llu, Overruns: %llu\n",
                    (unsigned long long)bs.underrun_count,
                    (unsigned long long)bs.overrun_count);
    }
    for (uint32_t i = 0; i < cfg.tracks_in; ++i) {
        auto* track = core.get_track("audio-in-" + std::to_string(i));
        if (!track) continue;
        auto bs = core.get_buffer_status(track);
        std::printf("  audio-in-%u (inbound, %s, %uch):\n", i,
                    cfg.codec == AudioCodec::Opus ? "opus" : "pcm",
                    cfg.channels_in);
        std::printf("    Buffer: %u/%u frames (%.1f%%)\n",
                    bs.buffered_frames, bs.capacity_frames, bs.fill_ratio * 100.0f);
        std::printf("    Underruns: %llu, Overruns: %llu\n",
                    (unsigned long long)bs.underrun_count,
                    (unsigned long long)bs.overrun_count);

        // RMS and peak of received audio
        if (rt_stats.inbound_sample_count > 0) {
            double rms = std::sqrt(rt_stats.inbound_sum_sq /
                                   static_cast<double>(rt_stats.inbound_sample_count));
            double rms_db = (rms > 0) ? 20.0 * std::log10(rms) : -100.0;
            double peak_db = (rt_stats.inbound_peak > 0)
                                 ? 20.0 * std::log10(rt_stats.inbound_peak) : -100.0;
            std::printf("    Received RMS:  %.1f dBFS\n", rms_db);
            std::printf("    Received Peak: %.1f dBFS\n", peak_db);
        } else {
            std::printf("    No audio received\n");
        }
    }

    // Data tracks
    if (cfg.data_tracks) {
        std::printf("\nData Tracks:\n");
        std::printf("  state-out:   %llu messages sent (48 bytes each)\n",
                    (unsigned long long)data_stats.messages_sent.load());
        std::printf("  data recv:   %llu messages, %llu bytes\n",
                    (unsigned long long)cb_ctx.data_messages_received.load(),
                    (unsigned long long)cb_ctx.data_bytes_received.load());
    }

    // Connection state history
    {
        std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(cb_ctx.mtx));
        std::printf("\nConnection History:");
        for (auto s : cb_ctx.state_history) {
            std::printf(" %s →", connection_state_str(s));
        }
        std::printf(" (end)\n");
    }

    // Verdict
    bool audio_received = rt_stats.inbound_sample_count > 0;
    bool connected = (core.get_connection_state() == ConnectionState::Connected);
    bool no_errors = (ss.reconnect_count == 0);

    std::printf("\nResult: ");
    if (connected && audio_received && no_errors) {
        std::printf("PASS — connected, audio flowing, no errors\n");
    } else if (connected && audio_received) {
        std::printf("PASS (with reconnects) — audio flowing, %u reconnect(s)\n",
                    ss.reconnect_count);
    } else if (connected) {
        std::printf("PARTIAL — connected but no audio received on inbound tracks\n");
    } else {
        std::printf("FAIL — final state: %s\n",
                    connection_state_str(core.get_connection_state()));
    }
}

// ============================================================================
// Main
// ============================================================================

int main(int argc, char** argv) {
    auto cfg = parse_args(argc, argv);

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    std::printf("Panaudia Test Harness\n");
    std::printf("  Server:     %s\n", cfg.server_url.c_str());
    std::printf("  Node ID:    %s\n", cfg.node_id.c_str());
    std::printf("  Tracks out: %u (mono, %s)\n", cfg.tracks_out,
                cfg.codec == AudioCodec::Opus ? "opus" : "pcm");
    std::printf("  Tracks in:  %u (%uch, %s)\n", cfg.tracks_in, cfg.channels_in,
                cfg.codec == AudioCodec::Opus ? "opus" : "pcm");
    std::printf("  Data tracks: %s\n", cfg.data_tracks ? "yes" : "no");
    std::printf("  Frame size:  %u ms (%u samples)\n",
                cfg.frame_ms, cfg.sample_rate * cfg.frame_ms / 1000);
    std::printf("  Duration:    %u s\n", cfg.duration_secs);
    std::printf("  Signal:      %s",
                cfg.signal == SignalType::Sine ? "sine" :
                cfg.signal == SignalType::Noise ? "noise" : "silence");
    if (cfg.signal == SignalType::Sine) {
        std::printf(" (%.0f Hz)", cfg.sine_freq);
    }
    std::printf("\n\n");

    // --- Create and configure ---
    CallbackContext cb_ctx;
    PanaudiaCore core;

    auto session_config = build_session_config(cfg, cb_ctx);
    core.configure(session_config);

    // Verify track handles
    for (uint32_t i = 0; i < cfg.tracks_out; ++i) {
        if (!core.get_track("audio-out-" + std::to_string(i))) {
            std::fprintf(stderr, "Error: audio-out-%u track handle is null\n", i);
            return 1;
        }
    }
    for (uint32_t i = 0; i < cfg.tracks_in; ++i) {
        if (!core.get_track("audio-in-" + std::to_string(i))) {
            std::fprintf(stderr, "Error: audio-in-%u track handle is null\n", i);
            return 1;
        }
    }

    // --- Connect ---
    std::printf("Connecting...\n");
    core.connect();

    // Wait for Connected (up to 30 seconds)
    auto connect_deadline = Clock::now() + std::chrono::seconds(30);
    while (g_running && Clock::now() < connect_deadline) {
        auto state = core.get_connection_state();
        if (state == ConnectionState::Connected) break;
        if (state == ConnectionState::Failed) {
            std::fprintf(stderr, "Connection failed\n");
            return 1;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }

    if (core.get_connection_state() != ConnectionState::Connected) {
        std::fprintf(stderr, "Connection timed out (state: %s)\n",
                    connection_state_str(core.get_connection_state()));
        core.disconnect();
        return 1;
    }

    std::printf("Connected! Starting audio...\n\n");

    // --- Run RT thread + data thread ---
    RTStats rt_stats;
    DataStats data_stats;

    auto start_time = Clock::now();

    std::thread rt_thread(rt_thread_func, std::ref(core), std::ref(cfg),
                           std::ref(rt_stats));

    std::thread data_thread;
    if (cfg.data_tracks) {
        data_thread = std::thread(data_send_thread_func, std::ref(core),
                                   std::ref(data_stats));
    }

    // Main loop: wait for duration or Ctrl+C
    auto end_time = start_time + std::chrono::seconds(cfg.duration_secs);
    while (g_running && Clock::now() < end_time) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        // Periodic status (every 10 seconds)
        auto elapsed = std::chrono::duration<double>(Clock::now() - start_time).count();
        static double last_status = 0;
        if (elapsed - last_status >= 10.0) {
            last_status = elapsed;
            auto ss = core.get_stats();
            std::printf("[%.0fs] pkts sent=%llu recv=%llu, state=%s\n",
                        elapsed,
                        (unsigned long long)ss.packets_sent,
                        (unsigned long long)ss.packets_received,
                        connection_state_str(core.get_connection_state()));
        }
    }

    // --- Stop ---
    g_running = false;

    if (rt_thread.joinable()) rt_thread.join();
    if (data_thread.joinable()) data_thread.join();

    auto actual_duration = std::chrono::duration<double>(Clock::now() - start_time).count();

    // --- Report ---
    print_report(cfg, core, cb_ctx, rt_stats, data_stats, actual_duration);

    // --- Disconnect ---
    std::printf("\nDisconnecting...\n");
    core.disconnect();
    std::printf("Done.\n");

    return 0;
}
