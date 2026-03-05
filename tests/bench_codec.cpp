#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include "panaudia/opus_codec.h"

#include <cmath>
#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include <vector>

using namespace panaudia;

static OpusEncoderConfig enc_config(uint32_t channels = 1, uint32_t frame_ms = 5,
                                     uint32_t bitrate = 64000) {
    OpusEncoderConfig c;
    c.channels = channels;
    c.frame_size_ms = frame_ms;
    c.bitrate = bitrate;
    return c;
}

static OpusDecoderConfig dec_config(uint32_t channels = 1) {
    OpusDecoderConfig c;
    c.channels = channels;
    return c;
}

static void fill_sine(float* buf, uint32_t frame_count, uint32_t channels) {
    for (uint32_t i = 0; i < frame_count; ++i) {
        float val = std::sin(2.0f * static_cast<float>(M_PI) * 440.0f *
                             static_cast<float>(i) / 48000.0f);
        for (uint32_t ch = 0; ch < channels; ++ch) {
            buf[i * channels + ch] = val;
        }
    }
}

TEST_CASE("Opus encode mono 5ms", "[bench][opus_codec]") {
    OpusEncoderWrapper enc;
    enc.init(enc_config(1, 5));

    std::vector<float> pcm(240);
    fill_sine(pcm.data(), 240, 1);
    std::vector<uint8_t> out(4000);

    BENCHMARK("encode mono 5ms") {
        return enc.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
    };
}

TEST_CASE("Opus decode mono 5ms", "[bench][opus_codec]") {
    OpusEncoderWrapper enc;
    enc.init(enc_config(1, 5));

    OpusDecoderWrapper dec;
    dec.init(dec_config(1));

    std::vector<float> pcm(240);
    fill_sine(pcm.data(), 240, 1);
    std::vector<uint8_t> encoded(4000);
    int bytes = enc.encode(pcm.data(), 240, encoded.data(),
                           static_cast<uint32_t>(encoded.size()));

    std::vector<float> pcm_out(240);

    BENCHMARK("decode mono 5ms") {
        return dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                          pcm_out.data(), 240);
    };
}

TEST_CASE("Opus encode+decode mono 5ms", "[bench][opus_codec]") {
    OpusEncoderWrapper enc;
    enc.init(enc_config(1, 5));

    OpusDecoderWrapper dec;
    dec.init(dec_config(1));

    std::vector<float> pcm(240);
    fill_sine(pcm.data(), 240, 1);
    std::vector<uint8_t> encoded(4000);
    std::vector<float> pcm_out(240);

    BENCHMARK("encode+decode mono 5ms") {
        int bytes = enc.encode(pcm.data(), 240, encoded.data(),
                               static_cast<uint32_t>(encoded.size()));
        return dec.decode(encoded.data(), static_cast<uint32_t>(bytes),
                          pcm_out.data(), 240);
    };
}

TEST_CASE("Opus encode FOA 5ms", "[bench][opus_codec]") {
    OpusEncoderWrapper enc;
    enc.init(enc_config(4, 5, 128000));

    std::vector<float> pcm(240 * 4);
    fill_sine(pcm.data(), 240, 4);
    std::vector<uint8_t> out(8000);

    BENCHMARK("encode FOA 5ms") {
        return enc.encode(pcm.data(), 240, out.data(), static_cast<uint32_t>(out.size()));
    };
}

TEST_CASE("PCM frame+unframe", "[bench][pcm_framing]") {
    std::vector<float> pcm(240);
    fill_sine(pcm.data(), 240, 1);
    std::vector<uint8_t> framed(240 * sizeof(float));
    std::vector<float> pcm_out(240);

    BENCHMARK("pcm frame+unframe mono 240") {
        uint32_t bytes = pcm_frame(pcm.data(), 240, 1, framed.data());
        return pcm_unframe(framed.data(), bytes, pcm_out.data(), 1);
    };
}
