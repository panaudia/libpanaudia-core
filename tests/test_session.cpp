#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstring>
#include "panaudia/core.h"
#include "panaudia/session_manager.h"

using namespace panaudia;

// Helper: build a SessionConfig with typical tracks
static SessionConfig make_test_config() {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";
    config.jitter_buffer_min_ms = 10;
    config.jitter_buffer_max_ms = 200;
    config.jitter_buffer_initial_ms = 60;
    config.max_reconnect_attempts = 0;  // disable reconnection in existing tests

    // Outbound Opus audio (mic)
    TrackConfig mic;
    mic.name = "mic";
    mic.moq_namespace = {"test", "audio"};
    mic.moq_track_name = "mic";
    mic.direction = TrackDirection::Outbound;
    mic.type = TrackType::Audio;
    mic.channels = 1;
    mic.sample_rate = 48000;
    mic.codec = AudioCodec::Opus;
    mic.opus_bitrate = 64000;
    mic.opus_frame_size_ms = 5;
    config.tracks.push_back(mic);

    // Inbound Opus audio (speaker)
    TrackConfig speaker;
    speaker.name = "speaker";
    speaker.moq_namespace = {"test", "audio"};
    speaker.moq_track_name = "speaker";
    speaker.direction = TrackDirection::Inbound;
    speaker.type = TrackType::Audio;
    speaker.channels = 1;
    speaker.sample_rate = 48000;
    speaker.codec = AudioCodec::Opus;
    config.tracks.push_back(speaker);

    // Outbound data track (state)
    TrackConfig state;
    state.name = "state_out";
    state.moq_namespace = {"test", "state"};
    state.moq_track_name = "state";
    state.direction = TrackDirection::Outbound;
    state.type = TrackType::Data;
    config.tracks.push_back(state);

    // Inbound data track (control)
    TrackConfig control;
    control.name = "control_in";
    control.moq_namespace = {"test", "control"};
    control.moq_track_name = "control";
    control.direction = TrackDirection::Inbound;
    control.type = TrackType::Data;
    config.tracks.push_back(control);

    return config;
}

// ---- PanaudiaCore basic tests ----

TEST_CASE("PanaudiaCore can be created and destroyed", "[session]") {
    PanaudiaCore core;
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("PanaudiaCore can be configured", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("get_track returns nullptr before configure", "[session]") {
    PanaudiaCore core;
    REQUIRE(core.get_track("nonexistent") == nullptr);
}

// ---- get_track lookup ----

TEST_CASE("get_track returns correct handles after configure", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    REQUIRE(core.get_track("mic") != nullptr);
    REQUIRE(core.get_track("speaker") != nullptr);
    REQUIRE(core.get_track("state_out") != nullptr);
    REQUIRE(core.get_track("control_in") != nullptr);
}

TEST_CASE("get_track returns nullptr for unknown name", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    REQUIRE(core.get_track("nonexistent") == nullptr);
}

TEST_CASE("get_track handles are stable across calls", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    auto* mic1 = core.get_track("mic");
    auto* mic2 = core.get_track("mic");
    REQUIRE(mic1 == mic2);
}

// ---- TrackHandle config ----

TEST_CASE("TrackHandle preserves config", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    auto* mic = core.get_track("mic");
    REQUIRE(mic->config.name == "mic");
    REQUIRE(mic->config.direction == TrackDirection::Outbound);
    REQUIRE(mic->config.type == TrackType::Audio);
    REQUIRE(mic->config.channels == 1);
    REQUIRE(mic->config.sample_rate == 48000);
    REQUIRE(mic->config.codec == AudioCodec::Opus);

    auto* speaker = core.get_track("speaker");
    REQUIRE(speaker->config.direction == TrackDirection::Inbound);
    REQUIRE(speaker->config.type == TrackType::Audio);

    auto* state_out = core.get_track("state_out");
    REQUIRE(state_out->config.type == TrackType::Data);
    REQUIRE(state_out->config.direction == TrackDirection::Outbound);

    auto* ctrl = core.get_track("control_in");
    REQUIRE(ctrl->config.type == TrackType::Data);
    REQUIRE(ctrl->config.direction == TrackDirection::Inbound);
}

// ---- Buffer/codec allocation ----

TEST_CASE("Outbound audio track has ring buffer and encoder", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    auto* mic = core.get_track("mic");

    REQUIRE(mic->ring_buffer != nullptr);
    REQUIRE(mic->ring_buffer->channels() == 1);
    // ~200ms at 48kHz = 9600 frames
    REQUIRE(mic->ring_buffer->capacity_frames() == 9600);

    REQUIRE(mic->encoder != nullptr);
    REQUIRE(mic->encoder->is_initialized());
    REQUIRE(mic->encoder->channels() == 1);
    REQUIRE(mic->encoder->frame_size_samples() == 240);  // 5ms at 48kHz

    // Outbound should NOT have jitter buffer or decoder
    REQUIRE(mic->jitter_buffer == nullptr);
    REQUIRE(mic->decoder == nullptr);
}

TEST_CASE("Inbound audio track has jitter buffer and decoder", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    auto* speaker = core.get_track("speaker");

    REQUIRE(speaker->jitter_buffer != nullptr);
    REQUIRE(speaker->jitter_buffer->num_channels() == 1);
    REQUIRE(speaker->jitter_buffer->sample_rate() == 48000);

    REQUIRE(speaker->decoder != nullptr);
    REQUIRE(speaker->decoder->is_initialized());
    REQUIRE(speaker->decoder->channels() == 1);

    // Inbound should NOT have ring buffer or encoder
    REQUIRE(speaker->ring_buffer == nullptr);
    REQUIRE(speaker->encoder == nullptr);
}

TEST_CASE("Data tracks have no buffers or codecs", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    auto* state_out = core.get_track("state_out");
    REQUIRE(state_out->ring_buffer == nullptr);
    REQUIRE(state_out->jitter_buffer == nullptr);
    REQUIRE(state_out->encoder == nullptr);
    REQUIRE(state_out->decoder == nullptr);

    auto* ctrl = core.get_track("control_in");
    REQUIRE(ctrl->ring_buffer == nullptr);
    REQUIRE(ctrl->jitter_buffer == nullptr);
    REQUIRE(ctrl->encoder == nullptr);
    REQUIRE(ctrl->decoder == nullptr);
}

// ---- PCM codec tracks ----

TEST_CASE("Outbound PCM audio track has ring buffer but no encoder", "[session]") {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    TrackConfig pcm_out;
    pcm_out.name = "pcm_mic";
    pcm_out.moq_namespace = {"test"};
    pcm_out.direction = TrackDirection::Outbound;
    pcm_out.type = TrackType::Audio;
    pcm_out.channels = 2;
    pcm_out.sample_rate = 48000;
    pcm_out.codec = AudioCodec::PCM;
    config.tracks.push_back(pcm_out);

    PanaudiaCore core;
    core.configure(config);
    auto* track = core.get_track("pcm_mic");

    REQUIRE(track->ring_buffer != nullptr);
    REQUIRE(track->ring_buffer->channels() == 2);
    REQUIRE(track->encoder == nullptr);
    REQUIRE(track->jitter_buffer == nullptr);
    REQUIRE(track->decoder == nullptr);
}

TEST_CASE("Inbound PCM audio track has jitter buffer but no decoder", "[session]") {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    TrackConfig pcm_in;
    pcm_in.name = "pcm_speaker";
    pcm_in.moq_namespace = {"test"};
    pcm_in.direction = TrackDirection::Inbound;
    pcm_in.type = TrackType::Audio;
    pcm_in.channels = 2;
    pcm_in.sample_rate = 48000;
    pcm_in.codec = AudioCodec::PCM;
    config.tracks.push_back(pcm_in);

    PanaudiaCore core;
    core.configure(config);
    auto* track = core.get_track("pcm_speaker");

    REQUIRE(track->jitter_buffer != nullptr);
    REQUIRE(track->jitter_buffer->num_channels() == 2);
    REQUIRE(track->decoder == nullptr);
    REQUIRE(track->ring_buffer == nullptr);
    REQUIRE(track->encoder == nullptr);
}

// ---- Multi-channel / stereo ----

TEST_CASE("Stereo outbound audio has correct channel count", "[session]") {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    TrackConfig stereo;
    stereo.name = "stereo_mic";
    stereo.moq_namespace = {"test"};
    stereo.direction = TrackDirection::Outbound;
    stereo.type = TrackType::Audio;
    stereo.channels = 2;
    stereo.sample_rate = 48000;
    stereo.codec = AudioCodec::Opus;
    stereo.opus_bitrate = 128000;
    stereo.opus_frame_size_ms = 10;
    config.tracks.push_back(stereo);

    PanaudiaCore core;
    core.configure(config);
    auto* track = core.get_track("stereo_mic");

    REQUIRE(track->ring_buffer->channels() == 2);
    REQUIRE(track->encoder->channels() == 2);
    REQUIRE(track->encoder->frame_size_samples() == 480);  // 10ms at 48kHz
}

// ---- Edge cases ----

TEST_CASE("Configure with empty tracks list", "[session]") {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    PanaudiaCore core;
    core.configure(config);

    REQUIRE(core.get_track("anything") == nullptr);
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("Double configure replaces previous state", "[session]") {
    PanaudiaCore core;

    // First configure with mic track
    SessionConfig config1;
    config1.server_url = "https://test.panaudia.com";
    config1.jwt = "test-token";
    TrackConfig mic;
    mic.name = "mic";
    mic.moq_namespace = {"test"};
    mic.direction = TrackDirection::Outbound;
    mic.type = TrackType::Audio;
    mic.channels = 1;
    mic.sample_rate = 48000;
    mic.codec = AudioCodec::Opus;
    config1.tracks.push_back(mic);
    core.configure(config1);

    REQUIRE(core.get_track("mic") != nullptr);
    REQUIRE(core.get_track("speaker") == nullptr);

    // Second configure replaces with speaker track
    SessionConfig config2;
    config2.server_url = "https://test.panaudia.com";
    config2.jwt = "test-token";
    TrackConfig speaker;
    speaker.name = "speaker";
    speaker.moq_namespace = {"test"};
    speaker.direction = TrackDirection::Inbound;
    speaker.type = TrackType::Audio;
    speaker.channels = 1;
    speaker.sample_rate = 48000;
    speaker.codec = AudioCodec::Opus;
    config2.tracks.push_back(speaker);
    core.configure(config2);

    // Old track gone, new track present
    REQUIRE(core.get_track("mic") == nullptr);
    REQUIRE(core.get_track("speaker") != nullptr);
}

// ---- MOQ state defaults ----

TEST_CASE("TrackHandle MOQ state initialised to zero", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    auto* mic = core.get_track("mic");
    REQUIRE(mic->moq_track_alias == 0);
    REQUIRE(mic->moq_request_id == 0);
    REQUIRE(mic->next_object_id == 0);
    REQUIRE(mic->next_group_id == 0);
}

// ---- Buffer status ----

TEST_CASE("get_buffer_status for outbound audio track", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    auto* mic = core.get_track("mic");

    auto status = core.get_buffer_status(mic);
    REQUIRE(status.capacity_frames == 9600);
    REQUIRE(status.buffered_frames == 0);
    REQUIRE(status.fill_ratio == 0.0f);
}

TEST_CASE("get_buffer_status for inbound audio track", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    auto* speaker = core.get_track("speaker");

    auto status = core.get_buffer_status(speaker);
    REQUIRE(status.capacity_frames > 0);
    REQUIRE(status.buffered_frames == 0);
}

TEST_CASE("get_buffer_status for data track returns empty", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    auto* data_track = core.get_track("state_out");

    auto status = core.get_buffer_status(data_track);
    REQUIRE(status.capacity_frames == 0);
    REQUIRE(status.buffered_frames == 0);
}

TEST_CASE("get_buffer_status for nullptr returns empty", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    auto status = core.get_buffer_status(nullptr);
    REQUIRE(status.capacity_frames == 0);
    REQUIRE(status.buffered_frames == 0);
}

// ---- SessionManager directly ----

TEST_CASE("SessionManager configure and get_track", "[session]") {
    SessionManager sm;

    SessionConfig config = make_test_config();
    sm.configure(config);

    REQUIRE(sm.get_track("mic") != nullptr);
    REQUIRE(sm.get_track("speaker") != nullptr);
    REQUIRE(sm.get_track("state_out") != nullptr);
    REQUIRE(sm.get_track("control_in") != nullptr);
    REQUIRE(sm.get_track("bogus") == nullptr);
}

TEST_CASE("update_jwt stores new token", "[session]") {
    PanaudiaCore core;
    core.configure(make_test_config());
    // Just verify it doesn't crash; actual JWT usage is Phase 4b
    core.update_jwt("new-token-value");
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

// ---- Phase 4b: connect/disconnect ----

TEST_CASE("connect transitions to Connecting state", "[session][connect]") {
    PanaudiaCore core;
    auto config = make_test_config();

    ConnectionState last_state = ConnectionState::Disconnected;

    config.status_callback = [](ConnectionState state, const char* /*message*/,
                                 void* ctx) {
        auto* s = static_cast<ConnectionState*>(ctx);
        *s = state;
    };
    config.status_ctx = &last_state;

    core.configure(config);
    core.connect();

    // connect() should have started the transport — state should be
    // at least Connecting (might already be Failed if no server is running,
    // but it should NOT be Disconnected)
    auto state = core.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);

    // Clean up
    core.disconnect();
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("disconnect when not connected is safe", "[session][connect]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    // Should not crash
    core.disconnect();
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);

    // Double disconnect
    core.disconnect();
    REQUIRE(core.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("double connect is rejected", "[session][connect]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    core.connect();
    auto state_after_first = core.get_connection_state();

    // Second connect should be a no-op (not crash)
    core.connect();
    auto state_after_second = core.get_connection_state();
    REQUIRE(state_after_first == state_after_second);

    core.disconnect();
}

TEST_CASE("connect with invalid URL sets Failed state", "[session][connect]") {
    PanaudiaCore core;
    SessionConfig config = make_test_config();
    config.server_url = "";  // invalid

    core.configure(config);
    core.connect();

    REQUIRE(core.get_connection_state() == ConnectionState::Failed);
}

TEST_CASE("disconnect clears track MOQ state", "[session][connect]") {
    PanaudiaCore core;
    core.configure(make_test_config());

    core.connect();
    core.disconnect();

    // All track MOQ state should be reset
    auto* mic = core.get_track("mic");
    REQUIRE(mic->moq_track_alias == 0);
    REQUIRE(mic->moq_request_id == 0);
    REQUIRE(mic->next_object_id == 0);
    REQUIRE(mic->next_group_id == 0);

    auto* speaker = core.get_track("speaker");
    REQUIRE(speaker->moq_track_alias == 0);
    REQUIRE(speaker->moq_request_id == 0);
}

// ---- URL parsing (tested via SessionManager directly) ----

TEST_CASE("URL parsing: https with port", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "https://dev.panaudia.com:4433";
    config.jwt = "test";
    sm.configure(config);

    // We can't directly test parse_url since it's private, but we verify
    // connect doesn't fail on URL parsing (it will fail on actual QUIC connect,
    // which is expected since there's no server)
    sm.connect();
    // State should be Connecting or Failed (transport init), not Disconnected
    auto state = sm.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);
    sm.disconnect();
}

TEST_CASE("URL parsing: https without port defaults to 443", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "https://example.com";
    config.jwt = "test";
    sm.configure(config);

    sm.connect();
    auto state = sm.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);
    sm.disconnect();
}

TEST_CASE("URL parsing: bare host with port", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "example.com:4433";
    config.jwt = "test";
    sm.configure(config);

    sm.connect();
    auto state = sm.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);
    sm.disconnect();
}

TEST_CASE("URL parsing: bare host without port", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "example.com";
    config.jwt = "test";
    sm.configure(config);

    sm.connect();
    auto state = sm.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);
    sm.disconnect();
}

TEST_CASE("URL parsing: empty URL fails", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "";
    config.jwt = "test";
    sm.configure(config);

    sm.connect();
    REQUIRE(sm.get_connection_state() == ConnectionState::Failed);
}

TEST_CASE("URL parsing: URL with trailing path", "[session][url]") {
    SessionManager sm;
    SessionConfig config;
    config.server_url = "https://dev.panaudia.com:4433/moq";
    config.jwt = "test";
    sm.configure(config);

    sm.connect();
    auto state = sm.get_connection_state();
    REQUIRE(state != ConnectionState::Disconnected);
    sm.disconnect();
}

// ---- Phase 4c: write_audio / read_audio / send_data ----

TEST_CASE("write_audio writes to ring buffer", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());
    auto* mic = sm.get_track("mic");
    REQUIRE(mic != nullptr);
    REQUIRE(mic->ring_buffer != nullptr);

    // Write 480 mono frames
    std::vector<float> pcm(480, 0.5f);
    sm.write_audio(mic, pcm.data(), 480, 0);

    REQUIRE(mic->ring_buffer->read_available() == 480);
}

TEST_CASE("write_audio with nullptr/inbound/data track is no-op", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());

    float dummy[240] = {};

    // nullptr
    sm.write_audio(nullptr, dummy, 240, 0);

    // inbound audio track
    auto* speaker = sm.get_track("speaker");
    sm.write_audio(speaker, dummy, 240, 0);
    // speaker has no ring buffer, should not crash

    // data track
    auto* data_track = sm.get_track("state_out");
    sm.write_audio(data_track, dummy, 240, 0);
    // no ring buffer, should not crash
}

TEST_CASE("read_audio returns 0 on empty jitter buffer", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());
    auto* speaker = sm.get_track("speaker");
    REQUIRE(speaker != nullptr);
    REQUIRE(speaker->jitter_buffer != nullptr);

    float buffer[480] = {};
    uint32_t frames_read = sm.read_audio(speaker, buffer, 480, 0);

    // Jitter buffer is in FILLING state, returns silence (0 frames)
    REQUIRE(frames_read == 0);
}

TEST_CASE("read_audio with nullptr/outbound/data track returns 0", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());

    float buffer[240] = {};

    // nullptr
    REQUIRE(sm.read_audio(nullptr, buffer, 240, 0) == 0);

    // outbound audio track
    auto* mic = sm.get_track("mic");
    REQUIRE(sm.read_audio(mic, buffer, 240, 0) == 0);

    // data track
    auto* data_track = sm.get_track("control_in");
    REQUIRE(sm.read_audio(data_track, buffer, 240, 0) == 0);
}

TEST_CASE("send_data with no transport/zero alias is no-op", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());

    auto* state_out = sm.get_track("state_out");
    uint8_t data[] = {1, 2, 3};

    // No transport (not connected), zero alias — should not crash
    sm.send_data(state_out, data, sizeof(data));

    // nullptr track
    sm.send_data(nullptr, data, sizeof(data));
}

TEST_CASE("Outbound Opus track has correct pre-allocated buffer sizes",
          "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());
    auto* mic = sm.get_track("mic");
    REQUIRE(mic != nullptr);

    // 5ms @ 48kHz mono = 240 samples
    REQUIRE(mic->pcm_read_buffer.size() == 240);
    REQUIRE(mic->encode_output_buffer.size() == 512);
    REQUIRE(mic->datagram_buffer.size() == 546);  // 34 + 512

    // Inbound buffers should be empty
    REQUIRE(mic->decode_buffer.empty());
}

TEST_CASE("Outbound PCM track has correct pre-allocated buffer sizes",
          "[session][4c]") {
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";

    TrackConfig pcm_out;
    pcm_out.name = "pcm_mic";
    pcm_out.moq_namespace = {"test"};
    pcm_out.direction = TrackDirection::Outbound;
    pcm_out.type = TrackType::Audio;
    pcm_out.channels = 1;
    pcm_out.sample_rate = 48000;
    pcm_out.codec = AudioCodec::PCM;
    pcm_out.pcm_frame_size_ms = 5;
    config.tracks.push_back(pcm_out);

    SessionManager sm;
    sm.configure(config);
    auto* track = sm.get_track("pcm_mic");
    REQUIRE(track != nullptr);

    // 5ms @ 48kHz mono = 240 samples
    REQUIRE(track->pcm_read_buffer.size() == 240);
    REQUIRE(track->encode_output_buffer.size() == 960);  // 240 * 1 * 4
    REQUIRE(track->datagram_buffer.size() == 994);        // 34 + 960

    REQUIRE(track->decode_buffer.empty());
}

TEST_CASE("Inbound audio track has pre-allocated decode buffer",
          "[session][4c]") {
    // Mono
    {
        SessionManager sm;
        sm.configure(make_test_config());
        auto* speaker = sm.get_track("speaker");
        REQUIRE(speaker != nullptr);
        REQUIRE(speaker->decode_buffer.size() == 960);  // 960 * 1 channel
    }

    // Stereo
    {
        SessionConfig config;
        config.server_url = "https://test.panaudia.com";
        config.jwt = "test-token";
        config.jitter_buffer_min_ms = 10;
        config.jitter_buffer_max_ms = 200;
        config.jitter_buffer_initial_ms = 60;

        TrackConfig stereo_in;
        stereo_in.name = "stereo_speaker";
        stereo_in.moq_namespace = {"test"};
        stereo_in.direction = TrackDirection::Inbound;
        stereo_in.type = TrackType::Audio;
        stereo_in.channels = 2;
        stereo_in.sample_rate = 48000;
        stereo_in.codec = AudioCodec::Opus;
        config.tracks.push_back(stereo_in);

        SessionManager sm;
        sm.configure(config);
        auto* track = sm.get_track("stereo_speaker");
        REQUIRE(track != nullptr);
        REQUIRE(track->decode_buffer.size() == 1920);  // 960 * 2 channels
    }
}

TEST_CASE("Data tracks have no pre-allocated buffers", "[session][4c]") {
    SessionManager sm;
    sm.configure(make_test_config());

    auto* state_out = sm.get_track("state_out");
    REQUIRE(state_out->pcm_read_buffer.empty());
    REQUIRE(state_out->encode_output_buffer.empty());
    REQUIRE(state_out->datagram_buffer.empty());
    REQUIRE(state_out->decode_buffer.empty());

    auto* ctrl = sm.get_track("control_in");
    REQUIRE(ctrl->pcm_read_buffer.empty());
    REQUIRE(ctrl->encode_output_buffer.empty());
    REQUIRE(ctrl->datagram_buffer.empty());
    REQUIRE(ctrl->decode_buffer.empty());
}

TEST_CASE("Opus encode-decode round trip through track buffers",
          "[session][4c]") {
    // Set up outbound + inbound Opus tracks
    SessionConfig config;
    config.server_url = "https://test.panaudia.com";
    config.jwt = "test-token";
    config.jitter_buffer_min_ms = 10;
    config.jitter_buffer_max_ms = 200;
    config.jitter_buffer_initial_ms = 20;

    TrackConfig mic;
    mic.name = "mic";
    mic.moq_namespace = {"test"};
    mic.direction = TrackDirection::Outbound;
    mic.type = TrackType::Audio;
    mic.channels = 1;
    mic.sample_rate = 48000;
    mic.codec = AudioCodec::Opus;
    mic.opus_bitrate = 64000;
    mic.opus_frame_size_ms = 5;
    config.tracks.push_back(mic);

    TrackConfig speaker;
    speaker.name = "speaker";
    speaker.moq_namespace = {"test"};
    speaker.direction = TrackDirection::Inbound;
    speaker.type = TrackType::Audio;
    speaker.channels = 1;
    speaker.sample_rate = 48000;
    speaker.codec = AudioCodec::Opus;
    config.tracks.push_back(speaker);

    SessionManager sm;
    sm.configure(config);
    auto* mic_h = sm.get_track("mic");
    auto* spk_h = sm.get_track("speaker");
    REQUIRE(mic_h != nullptr);
    REQUIRE(spk_h != nullptr);

    // Generate a 5ms 440Hz sine wave (240 samples @ 48kHz)
    constexpr uint32_t frame_size = 240;
    std::vector<float> sine(frame_size);
    for (uint32_t i = 0; i < frame_size; i++) {
        sine[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f *
                                    static_cast<float>(i) / 48000.0f);
    }

    // Encode using the outbound track's encoder
    REQUIRE(mic_h->encoder != nullptr);
    std::vector<uint8_t> encoded(512);
    int enc_bytes = mic_h->encoder->encode(
        sine.data(), frame_size, encoded.data(),
        static_cast<uint32_t>(encoded.size()));
    REQUIRE(enc_bytes > 0);

    // Decode using the inbound track's decoder → into its decode_buffer
    REQUIRE(spk_h->decoder != nullptr);
    int dec_frames = spk_h->decoder->decode(
        encoded.data(), static_cast<uint32_t>(enc_bytes),
        spk_h->decode_buffer.data(),
        static_cast<uint32_t>(spk_h->decode_buffer.size()));
    REQUIRE(dec_frames == static_cast<int>(frame_size));

    // Write decoded audio into jitter buffer — prime it
    // Need enough to transition from FILLING to PLAYING
    // target_latency = 20ms = 960 samples. Write 4 frames (4 * 240 = 960).
    for (int i = 0; i < 4; i++) {
        spk_h->jitter_buffer->write(spk_h->decode_buffer.data(),
                                     static_cast<uint32_t>(dec_frames));
    }

    // read_audio should now return audio (jitter buffer transitioned to PLAYING)
    std::vector<float> output(frame_size, 0.0f);
    uint32_t frames_read = sm.read_audio(spk_h, output.data(), frame_size, 0);
    REQUIRE(frames_read == frame_size);

    // Verify output is not all zeros (lossy codec, so we just check non-silence).
    // First frame after Opus init has lower amplitude due to codec startup.
    float sum = 0.0f;
    for (uint32_t i = 0; i < frame_size; i++) {
        sum += std::fabs(output[i]);
    }
    REQUIRE(sum > 0.01f);
}

// ---- Phase 4d: Auto-reconnection ----
//
// IMPORTANT: In tests with status callbacks, declare SessionManager AFTER
// the callback context variables. C++ destroys locals in reverse declaration
// order, so sm is destroyed first — its destructor calls disconnect() while
// the callback context is still valid.

#include <chrono>
#include <thread>
#include <mutex>

// Callback context for collecting state history
struct StateHistory {
    std::mutex mtx;
    std::vector<ConnectionState> states;

    static void callback(ConnectionState state, const char*, void* ctx) {
        auto* self = static_cast<StateHistory*>(ctx);
        std::lock_guard<std::mutex> lock(self->mtx);
        self->states.push_back(state);
    }

    bool saw(ConnectionState s) {
        std::lock_guard<std::mutex> lock(mtx);
        for (auto st : states) {
            if (st == s) return true;
        }
        return false;
    }

    bool saw_after(ConnectionState first, ConnectionState second) {
        std::lock_guard<std::mutex> lock(mtx);
        bool saw_first = false;
        for (auto st : states) {
            if (st == first) saw_first = true;
            if (saw_first && st == second) return true;
        }
        return false;
    }
};

// make_test_config variant using localhost for fast connection refusal.
// Using test.panaudia.com causes DNS/TCP timeouts in Docker (~30s per attempt).
// 127.0.0.1:19999 gets "connection refused" immediately on all platforms.
static SessionConfig make_reconnect_test_config() {
    auto config = make_test_config();
    config.server_url = "127.0.0.1:19999";
    return config;
}

// Helper to poll until a state is reached or timeout
static bool wait_for_state(SessionManager& sm, ConnectionState target,
                            int timeout_ms) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (sm.get_connection_state() == target) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return sm.get_connection_state() == target;
}

TEST_CASE("Default SessionConfig has reconnection enabled", "[session][4d]") {
    SessionConfig config;
    REQUIRE(config.max_reconnect_attempts == 10);
    REQUIRE(config.reconnect_base_delay_ms == 100);
    REQUIRE(config.reconnect_max_delay_ms == 10000);
}

TEST_CASE("Reconnection disabled when max_reconnect_attempts=0",
          "[session][4d]") {
    // Callback context declared first (destroyed last)
    StateHistory history;

    auto config = make_reconnect_test_config();
    config.status_callback = StateHistory::callback;
    config.status_ctx = &history;

    // sm declared after history (destroyed first)
    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for connection to fail (no server running)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Should never see Reconnecting state
    REQUIRE_FALSE(history.saw(ConnectionState::Reconnecting));

    sm.disconnect();
}

TEST_CASE("Connection failure with reconnect enabled transitions to Reconnecting",
          "[session][4d]") {
    StateHistory history;

    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 3;
    config.reconnect_base_delay_ms = 50;
    config.status_callback = StateHistory::callback;
    config.status_ctx = &history;

    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for connection failure + reconnect transition
    std::this_thread::sleep_for(std::chrono::milliseconds(1000));

    REQUIRE(history.saw(ConnectionState::Reconnecting));

    sm.disconnect();
}

TEST_CASE("disconnect() during Reconnecting stops cycle", "[session][4d]") {
    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 10;
    config.reconnect_base_delay_ms = 200;

    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for connection to fail and enter Reconnecting
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    // Disconnect should stop the reconnect cycle
    sm.disconnect();
    REQUIRE(sm.get_connection_state() == ConnectionState::Disconnected);

    // Wait and verify it stays Disconnected
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    REQUIRE(sm.get_connection_state() == ConnectionState::Disconnected);
}

TEST_CASE("Max attempts exhausted transitions to Failed", "[session][4d]") {
    StateHistory history;

    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 2;
    config.reconnect_base_delay_ms = 50;
    config.reconnect_max_delay_ms = 200;
    config.status_callback = StateHistory::callback;
    config.status_ctx = &history;

    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for initial fail + 2 reconnect attempts (each takes msquic time + backoff)
    REQUIRE(wait_for_state(sm, ConnectionState::Failed, 30000));

    // Verify we saw Reconnecting then Failed
    REQUIRE(history.saw(ConnectionState::Reconnecting));
    REQUIRE(history.saw_after(ConnectionState::Reconnecting,
                               ConnectionState::Failed));

    sm.disconnect();
}

TEST_CASE("Reconnect count tracked in stats", "[session][4d]") {
    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 2;
    config.reconnect_base_delay_ms = 50;
    config.reconnect_max_delay_ms = 200;

    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for reconnection attempts to complete
    REQUIRE(wait_for_state(sm, ConnectionState::Failed, 30000));

    auto stats = sm.get_stats();
    REQUIRE(stats.reconnect_count > 0);

    sm.disconnect();
}

TEST_CASE("connect() rejected during Reconnecting", "[session][4d]") {
    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 10;
    config.reconnect_base_delay_ms = 500;

    SessionManager sm;
    sm.configure(config);
    sm.connect();

    // Wait for Reconnecting state
    REQUIRE(wait_for_state(sm, ConnectionState::Reconnecting, 5000));

    // Try second connect — should be no-op
    sm.connect();
    REQUIRE(sm.get_connection_state() == ConnectionState::Reconnecting);

    sm.disconnect();
}

TEST_CASE("Track MOQ state reset during reconnection", "[session][4d]") {
    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 2;
    config.reconnect_base_delay_ms = 50;
    config.reconnect_max_delay_ms = 200;

    SessionManager sm;
    sm.configure(config);

    // Write some audio to a track first
    auto* mic = sm.get_track("mic");
    REQUIRE(mic != nullptr);
    std::vector<float> pcm(480, 0.5f);
    sm.write_audio(mic, pcm.data(), 480, 0);

    sm.connect();

    // Wait for reconnection cycle to complete
    REQUIRE(wait_for_state(sm, ConnectionState::Failed, 30000));

    // After reconnection cycle, MOQ state should be reset
    REQUIRE(mic->moq_track_alias == 0);
    REQUIRE(mic->moq_request_id == 0);
    REQUIRE(mic->next_object_id == 0);

    sm.disconnect();
}

TEST_CASE("Exponential backoff timing", "[session][4d]") {
    auto config = make_reconnect_test_config();
    config.max_reconnect_attempts = 3;
    config.reconnect_base_delay_ms = 100;
    config.reconnect_max_delay_ms = 10000;

    SessionManager sm;
    sm.configure(config);

    auto start = std::chrono::steady_clock::now();
    sm.connect();

    // Wait for all 3 attempts to exhaust
    REQUIRE(wait_for_state(sm, ConnectionState::Failed, 30000));

    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Backoff delays: 100ms + 200ms = 300ms minimum between attempts
    // (first attempt is immediate, delay is between failures and next attempt)
    REQUIRE(elapsed.count() >= 200);

    sm.disconnect();
}
