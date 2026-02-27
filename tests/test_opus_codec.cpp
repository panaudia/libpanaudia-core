#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include "panaudia/opus_codec.h"

#include <cmath>
#include <vector>
#include <numeric>

using namespace panaudia;

// Config helpers (avoid C++20 designated initializers for GCC C++17 compat)
static OpusEncoderConfig enc_config(uint32_t channels = 1, uint32_t frame_ms = 5,
                                     uint32_t bitrate = 64000, bool dtx = true) {
    OpusEncoderConfig c;
    c.channels = channels;
    c.frame_size_ms = frame_ms;
    c.bitrate = bitrate;
    c.dtx = dtx;
    return c;
}

static OpusDecoderConfig dec_config(uint32_t channels = 1) {
    OpusDecoderConfig c;
    c.channels = channels;
    return c;
}

// Helper: generate a sine wave
static void generate_sine(float* buf, uint32_t frame_count, uint32_t channels,
                          float freq = 440.0f, float sample_rate = 48000.0f) {
    for (uint32_t i = 0; i < frame_count; ++i) {
        float val = std::sin(2.0f * static_cast<float>(M_PI) * freq * static_cast<float>(i) / sample_rate);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            buf[i * channels + ch] = val;
        }
    }
}

// Helper: compute RMS of a buffer
static float compute_rms(const float* buf, uint32_t count) {
    double sum = 0.0;
    for (uint32_t i = 0; i < count; ++i) {
        sum += static_cast<double>(buf[i]) * static_cast<double>(buf[i]);
    }
    return static_cast<float>(std::sqrt(sum / count));
}

// =============================================================================
// Encoder init & config
// =============================================================================

TEST_CASE("OpusEncoder default construction", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE_FALSE(enc.is_initialized());
}

TEST_CASE("OpusEncoder init mono 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));
    REQUIRE(enc.is_initialized());
    REQUIRE(enc.frame_size_samples() == 240);
    REQUIRE(enc.channels() == 1);
}

TEST_CASE("OpusEncoder init stereo 10ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(2, 10)));
    REQUIRE(enc.is_initialized());
    REQUIRE(enc.frame_size_samples() == 480);
    REQUIRE(enc.channels() == 2);
}

TEST_CASE("OpusEncoder init FOA 4ch 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(4, 5)));
    REQUIRE(enc.is_initialized());
    REQUIRE(enc.frame_size_samples() == 240);
    REQUIRE(enc.channels() == 4);
}

TEST_CASE("OpusEncoder init SOA 9ch 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(9, 5)));
    REQUIRE(enc.is_initialized());
}

TEST_CASE("OpusEncoder init TOA 16ch 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(16, 5)));
    REQUIRE(enc.is_initialized());
}

// =============================================================================
// Encoder encode
// =============================================================================

TEST_CASE("OpusEncoder encode mono 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));

    std::vector<float> pcm(240);
    generate_sine(pcm.data(), 240, 1);

    std::vector<uint8_t> out(4000);
    int bytes = enc.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
    REQUIRE(bytes > 0);
}

TEST_CASE("OpusEncoder encode stereo 10ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(2, 10)));

    std::vector<float> pcm(480 * 2);
    generate_sine(pcm.data(), 480, 2);

    std::vector<uint8_t> out(4000);
    int bytes = enc.encode(pcm.data(), 480, out.data(), static_cast<uint32_t>(out.size()));
    REQUIRE(bytes > 0);
}

TEST_CASE("OpusEncoder encode FOA 5ms", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(4, 5)));

    std::vector<float> pcm(240 * 4);
    generate_sine(pcm.data(), 240, 4);

    std::vector<uint8_t> out(8000);
    int bytes = enc.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
    REQUIRE(bytes > 0);
}

TEST_CASE("OpusEncoder wrong frame size", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));

    std::vector<float> pcm(480);  // 10ms, but encoder expects 5ms (240)
    std::vector<uint8_t> out(4000);
    int bytes = enc.encode(pcm.data(), 480, out.data(), static_cast<uint32_t>(out.size()));
    REQUIRE(bytes < 0);
}

TEST_CASE("OpusEncoder encode without init", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    std::vector<float> pcm(240);
    std::vector<uint8_t> out(4000);
    int bytes = enc.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
    REQUIRE(bytes < 0);
}

// =============================================================================
// DTX
// =============================================================================

TEST_CASE("OpusEncoder DTX produces small silence packets", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5, 64000, true)));

    std::vector<float> silence(240, 0.0f);
    std::vector<uint8_t> out(4000);

    int min_bytes = 4000;
    for (int i = 0; i < 50; ++i) {
        int bytes = enc.encode(silence.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
        REQUIRE(bytes > 0);
        if (bytes < min_bytes) min_bytes = bytes;
    }
    // DTX should produce very small packets for silence (typically 2-3 bytes)
    REQUIRE(min_bytes <= 6);
}

// =============================================================================
// Decoder init
// =============================================================================

TEST_CASE("OpusDecoder default construction", "[opus_decoder]") {
    OpusDecoderWrapper dec;
    REQUIRE_FALSE(dec.is_initialized());
}

TEST_CASE("OpusDecoder init mono", "[opus_decoder]") {
    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));
    REQUIRE(dec.is_initialized());
    REQUIRE(dec.channels() == 1);
}

TEST_CASE("OpusDecoder init stereo", "[opus_decoder]") {
    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(2)));
    REQUIRE(dec.is_initialized());
}

TEST_CASE("OpusDecoder init FOA", "[opus_decoder]") {
    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(4)));
    REQUIRE(dec.is_initialized());
}

// =============================================================================
// Roundtrip (encode -> decode)
// =============================================================================

TEST_CASE("Opus roundtrip mono 5ms 440Hz", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));

    std::vector<float> pcm_in(240);
    generate_sine(pcm_in.data(), 240, 1, 440.0f);

    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(240);

    // Prime the codec with a few frames for convergence
    for (int i = 0; i < 5; ++i) {
        int bytes = enc.encode(pcm_in.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 240);
        REQUIRE(frames == 240);
    }

    float rms_in = compute_rms(pcm_in.data(), 240);
    float rms_out = compute_rms(pcm_out.data(), 240);
    // RMS should be within 50% (Opus is lossy but preserves signal energy)
    REQUIRE(rms_out > rms_in * 0.5f);
    REQUIRE(rms_out < rms_in * 1.5f);
}

TEST_CASE("Opus roundtrip stereo 10ms", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(2, 10)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(2)));

    std::vector<float> pcm_in(480 * 2);
    generate_sine(pcm_in.data(), 480, 2, 440.0f);

    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(480 * 2);

    for (int i = 0; i < 5; ++i) {
        int bytes = enc.encode(pcm_in.data(), 480, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 480);
        REQUIRE(frames == 480);
    }

    float rms_in = compute_rms(pcm_in.data(), 480 * 2);
    float rms_out = compute_rms(pcm_out.data(), 480 * 2);
    REQUIRE(rms_out > rms_in * 0.5f);
    REQUIRE(rms_out < rms_in * 1.5f);
}

TEST_CASE("Opus roundtrip mono 20ms", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 20)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));

    std::vector<float> pcm_in(960);
    generate_sine(pcm_in.data(), 960, 1, 440.0f);

    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(960);

    for (int i = 0; i < 5; ++i) {
        int bytes = enc.encode(pcm_in.data(), 960, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 960);
        REQUIRE(frames == 960);
    }

    float rms_in = compute_rms(pcm_in.data(), 960);
    float rms_out = compute_rms(pcm_out.data(), 960);
    REQUIRE(rms_out > rms_in * 0.5f);
    REQUIRE(rms_out < rms_in * 1.5f);
}

TEST_CASE("Opus roundtrip FOA 4ch 5ms", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(4, 5, 128000)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(4)));

    std::vector<float> pcm_in(240 * 4);
    generate_sine(pcm_in.data(), 240, 4, 440.0f);

    std::vector<uint8_t> encoded(8000);
    std::vector<float> pcm_out(240 * 4);

    for (int i = 0; i < 5; ++i) {
        int bytes = enc.encode(pcm_in.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 240);
        REQUIRE(frames == 240);
    }

    // Verify all 4 channels have output
    for (uint32_t ch = 0; ch < 4; ++ch) {
        float ch_rms = 0.0f;
        for (uint32_t i = 0; i < 240; ++i) {
            float v = pcm_out[i * 4 + ch];
            ch_rms += v * v;
        }
        ch_rms = std::sqrt(ch_rms / 240.0f);
        REQUIRE(ch_rms > 0.01f);
    }
}

TEST_CASE("Opus roundtrip SOA 9ch 5ms", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(9, 5, 256000)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(9)));

    std::vector<float> pcm_in(240 * 9);
    generate_sine(pcm_in.data(), 240, 9, 440.0f);

    std::vector<uint8_t> encoded(16000);
    std::vector<float> pcm_out(240 * 9);

    for (int i = 0; i < 5; ++i) {
        int bytes = enc.encode(pcm_in.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 240);
        REQUIRE(frames == 240);
    }

    float rms_out = compute_rms(pcm_out.data(), 240 * 9);
    REQUIRE(rms_out > 0.01f);
}

// =============================================================================
// DTX roundtrip
// =============================================================================

TEST_CASE("Opus DTX silence roundtrip", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5, 64000, true)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));

    std::vector<float> silence(240, 0.0f);
    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(240);

    // Encode many silence frames to trigger DTX
    for (int i = 0; i < 50; ++i) {
        int bytes = enc.encode(silence.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);

        int frames = dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                                pcm_out.data(), 240);
        REQUIRE(frames == 240);
    }

    // Decoded silence should have low RMS
    float rms = compute_rms(pcm_out.data(), 240);
    REQUIRE(rms < 0.1f);
}

// =============================================================================
// PLC
// =============================================================================

TEST_CASE("Opus PLC produces non-silent output after priming", "[opus_codec]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));

    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));

    std::vector<float> pcm_in(240);
    generate_sine(pcm_in.data(), 240, 1, 440.0f);
    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(240);

    // Prime the decoder with real data
    for (int i = 0; i < 10; ++i) {
        int bytes = enc.encode(pcm_in.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        REQUIRE(bytes > 0);
        dec.decode(encoded.data(), static_cast<uint32_t>(bytes), pcm_out.data(), 240);
    }

    // Now PLC — should produce non-silent output
    int frames = dec.decode_plc(pcm_out.data(), 240);
    REQUIRE(frames == 240);

    float rms = compute_rms(pcm_out.data(), 240);
    REQUIRE(rms > 0.001f);
}

// =============================================================================
// Various bitrates
// =============================================================================

TEST_CASE("Opus encode at various bitrates", "[opus_encoder]") {
    uint32_t bitrates[] = {32000, 64000, 128000};

    for (auto br : bitrates) {
        SECTION("bitrate " + std::to_string(br)) {
            OpusEncoderWrapper enc;
            REQUIRE(enc.init(enc_config(1, 5, br)));

            std::vector<float> pcm(240);
            generate_sine(pcm.data(), 240, 1);

            std::vector<uint8_t> out(4000);
            int bytes = enc.encode(pcm.data(), 240, out.data(),
                                   static_cast<uint32_t>(out.size()));
            REQUIRE(bytes > 0);
        }
    }
}

// =============================================================================
// Move semantics
// =============================================================================

TEST_CASE("OpusEncoder move semantics", "[opus_encoder]") {
    OpusEncoderWrapper enc;
    REQUIRE(enc.init(enc_config(1, 5)));
    REQUIRE(enc.is_initialized());

    OpusEncoderWrapper enc2 = std::move(enc);
    REQUIRE(enc2.is_initialized());
    REQUIRE_FALSE(enc.is_initialized());  // NOLINT — testing moved-from state

    // Moved-to should work
    std::vector<float> pcm(240);
    generate_sine(pcm.data(), 240, 1);
    std::vector<uint8_t> out(4000);
    REQUIRE(enc2.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size())) > 0);
}

TEST_CASE("OpusDecoder move semantics", "[opus_decoder]") {
    OpusDecoderWrapper dec;
    REQUIRE(dec.init(dec_config(1)));
    REQUIRE(dec.is_initialized());

    OpusDecoderWrapper dec2 = std::move(dec);
    REQUIRE(dec2.is_initialized());
    REQUIRE_FALSE(dec.is_initialized());  // NOLINT — testing moved-from state
}

// =============================================================================
// PCM framing
// =============================================================================

TEST_CASE("PCM frame/unframe roundtrip mono", "[pcm_framing]") {
    std::vector<float> pcm_in(240);
    generate_sine(pcm_in.data(), 240, 1);

    std::vector<uint8_t> framed(240 * sizeof(float));
    uint32_t bytes = pcm_frame(pcm_in.data(), 240, 1, framed.data());
    REQUIRE(bytes == 240 * sizeof(float));

    std::vector<float> pcm_out(240);
    uint32_t frames = pcm_unframe(framed.data(), bytes, pcm_out.data(), 1);
    REQUIRE(frames == 240);

    for (uint32_t i = 0; i < 240; ++i) {
        REQUIRE(pcm_in[i] == pcm_out[i]);
    }
}

TEST_CASE("PCM frame/unframe roundtrip stereo", "[pcm_framing]") {
    std::vector<float> pcm_in(480);
    generate_sine(pcm_in.data(), 240, 2);

    std::vector<uint8_t> framed(480 * sizeof(float));
    uint32_t bytes = pcm_frame(pcm_in.data(), 240, 2, framed.data());
    REQUIRE(bytes == 480 * sizeof(float));

    std::vector<float> pcm_out(480);
    uint32_t frames = pcm_unframe(framed.data(), bytes, pcm_out.data(), 2);
    REQUIRE(frames == 240);

    for (uint32_t i = 0; i < 480; ++i) {
        REQUIRE(pcm_in[i] == pcm_out[i]);
    }
}

TEST_CASE("PCM frame/unframe roundtrip 4ch", "[pcm_framing]") {
    std::vector<float> pcm_in(240 * 4);
    generate_sine(pcm_in.data(), 240, 4);

    std::vector<uint8_t> framed(240 * 4 * sizeof(float));
    uint32_t bytes = pcm_frame(pcm_in.data(), 240, 4, framed.data());
    REQUIRE(bytes == 240 * 4 * sizeof(float));

    std::vector<float> pcm_out(240 * 4);
    uint32_t frames = pcm_unframe(framed.data(), bytes, pcm_out.data(), 4);
    REQUIRE(frames == 240);

    for (uint32_t i = 0; i < 240 * 4; ++i) {
        REQUIRE(pcm_in[i] == pcm_out[i]);
    }
}

TEST_CASE("PCM unframe rejects misaligned data", "[pcm_framing]") {
    std::vector<uint8_t> data(241);  // Not aligned to float*channels
    std::vector<float> pcm_out(240);

    // Mono: 241 bytes not divisible by 4 (sizeof float)
    REQUIRE(pcm_unframe(data.data(), 241, pcm_out.data(), 1) == 0);

    // Stereo: 241 bytes not divisible by 8 (2 * sizeof float)
    REQUIRE(pcm_unframe(data.data(), 241, pcm_out.data(), 2) == 0);
}
