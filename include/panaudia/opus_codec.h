#pragma once
#include <cstdint>

// Forward-declare libopus types (avoids #include <opus.h> in public header)
struct OpusEncoder;
struct OpusDecoder;
struct OpusMSEncoder;
struct OpusMSDecoder;

namespace panaudia {

struct OpusEncoderConfig {
    uint32_t sample_rate = 48000;
    uint32_t channels = 1;        // 1-2: simple API, 3+: multistream ambisonics
    uint32_t bitrate = 64000;     // bits/sec
    uint32_t frame_size_ms = 5;   // valid: 5, 10, 20, 40, 60
    uint32_t complexity = 5;      // 0-10
    bool dtx = true;
    bool vbr = true;
};

class OpusEncoderWrapper {
public:
    OpusEncoderWrapper();
    ~OpusEncoderWrapper();
    OpusEncoderWrapper(OpusEncoderWrapper&&) noexcept;
    OpusEncoderWrapper& operator=(OpusEncoderWrapper&&) noexcept;
    OpusEncoderWrapper(const OpusEncoderWrapper&) = delete;
    OpusEncoderWrapper& operator=(const OpusEncoderWrapper&) = delete;

    bool init(const OpusEncoderConfig& config);
    int encode(const float* pcm, uint32_t frame_count,
               uint8_t* output, uint32_t max_output_size);

    bool is_initialized() const;
    uint32_t frame_size_samples() const;
    uint32_t channels() const;

private:
    void destroy();
    ::OpusEncoder* encoder_ = nullptr;       // 1-2ch
    ::OpusMSEncoder* ms_encoder_ = nullptr;  // 3+ch
    uint32_t sample_rate_ = 48000;
    uint32_t channels_ = 1;
    uint32_t frame_size_samples_ = 240;
};

struct OpusDecoderConfig {
    uint32_t sample_rate = 48000;
    uint32_t channels = 1;
};

class OpusDecoderWrapper {
public:
    OpusDecoderWrapper();
    ~OpusDecoderWrapper();
    OpusDecoderWrapper(OpusDecoderWrapper&&) noexcept;
    OpusDecoderWrapper& operator=(OpusDecoderWrapper&&) noexcept;
    OpusDecoderWrapper(const OpusDecoderWrapper&) = delete;
    OpusDecoderWrapper& operator=(const OpusDecoderWrapper&) = delete;

    bool init(const OpusDecoderConfig& config);
    int decode(const uint8_t* data, uint32_t data_len,
               float* pcm, uint32_t max_frame_count);
    int decode_plc(float* pcm, uint32_t frame_count);

    bool is_initialized() const;
    uint32_t channels() const;

private:
    void destroy();
    ::OpusDecoder* decoder_ = nullptr;       // 1-2ch
    ::OpusMSDecoder* ms_decoder_ = nullptr;  // 3+ch
    uint32_t sample_rate_ = 48000;
    uint32_t channels_ = 1;
};

// PCM framing (AudioCodec::PCM tracks) — little-endian memcpy
uint32_t pcm_frame(const float* pcm, uint32_t frame_count, uint32_t channels,
                    uint8_t* output);
uint32_t pcm_unframe(const uint8_t* data, uint32_t data_len,
                      float* pcm, uint32_t channels);

}  // namespace panaudia
