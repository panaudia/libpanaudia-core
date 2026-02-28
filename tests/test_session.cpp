#include <catch2/catch_test_macros.hpp>
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
