#include "panaudia/opus_codec.h"
#include <opus.h>
#include <opus_multistream.h>
#include <cstring>
#include <algorithm>

namespace panaudia {

// --- OpusEncoderWrapper ---

OpusEncoderWrapper::OpusEncoderWrapper() = default;

OpusEncoderWrapper::~OpusEncoderWrapper() {
    destroy();
}

OpusEncoderWrapper::OpusEncoderWrapper(OpusEncoderWrapper&& other) noexcept
    : encoder_(other.encoder_),
      ms_encoder_(other.ms_encoder_),
      sample_rate_(other.sample_rate_),
      channels_(other.channels_),
      frame_size_samples_(other.frame_size_samples_) {
    other.encoder_ = nullptr;
    other.ms_encoder_ = nullptr;
}

OpusEncoderWrapper& OpusEncoderWrapper::operator=(OpusEncoderWrapper&& other) noexcept {
    if (this != &other) {
        destroy();
        encoder_ = other.encoder_;
        ms_encoder_ = other.ms_encoder_;
        sample_rate_ = other.sample_rate_;
        channels_ = other.channels_;
        frame_size_samples_ = other.frame_size_samples_;
        other.encoder_ = nullptr;
        other.ms_encoder_ = nullptr;
    }
    return *this;
}

bool OpusEncoderWrapper::init(const OpusEncoderConfig& config) {
    destroy();

    // Validate frame size
    switch (config.frame_size_ms) {
        case 5: case 10: case 20: case 40: case 60:
            break;
        default:
            return false;
    }

    if (config.channels == 0) return false;

    sample_rate_ = config.sample_rate;
    channels_ = config.channels;
    frame_size_samples_ = config.sample_rate * config.frame_size_ms / 1000;

    int err = 0;

    if (channels_ <= 2) {
        // Simple API for mono/stereo
        encoder_ = opus_encoder_create(
            static_cast<opus_int32>(sample_rate_),
            static_cast<int>(channels_),
            OPUS_APPLICATION_VOIP,
            &err);
        if (err != OPUS_OK || !encoder_) {
            encoder_ = nullptr;
            return false;
        }

        opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(config.bitrate)));
        opus_encoder_ctl(encoder_, OPUS_SET_VBR(config.vbr ? 1 : 0));
        opus_encoder_ctl(encoder_, OPUS_SET_DTX(config.dtx ? 1 : 0));
        opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(static_cast<opus_int32>(config.complexity)));
        opus_encoder_ctl(encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    } else {
        // Multistream API for 3+ channels (ambisonics)
        int streams = 0;
        int coupled_streams = 0;
        unsigned char mapping[255] = {};

        ms_encoder_ = opus_multistream_surround_encoder_create(
            static_cast<opus_int32>(sample_rate_),
            static_cast<int>(channels_),
            /*mapping_family=*/2,  // ambisonics
            &streams,
            &coupled_streams,
            mapping,
            OPUS_APPLICATION_VOIP,
            &err);
        if (err != OPUS_OK || !ms_encoder_) {
            ms_encoder_ = nullptr;
            return false;
        }

        opus_multistream_encoder_ctl(ms_encoder_, OPUS_SET_BITRATE(static_cast<opus_int32>(config.bitrate)));
        opus_multistream_encoder_ctl(ms_encoder_, OPUS_SET_VBR(config.vbr ? 1 : 0));
        opus_multistream_encoder_ctl(ms_encoder_, OPUS_SET_DTX(config.dtx ? 1 : 0));
        opus_multistream_encoder_ctl(ms_encoder_, OPUS_SET_COMPLEXITY(static_cast<opus_int32>(config.complexity)));
        opus_multistream_encoder_ctl(ms_encoder_, OPUS_SET_SIGNAL(OPUS_SIGNAL_VOICE));
    }

    return true;
}

int OpusEncoderWrapper::encode(const float* pcm, uint32_t frame_count,
                                uint8_t* output, uint32_t max_output_size) {
    if (!is_initialized()) return -1;
    if (frame_count != frame_size_samples_) return -1;

    if (encoder_) {
        return opus_encode_float(
            encoder_, pcm,
            static_cast<int>(frame_count),
            output,
            static_cast<opus_int32>(max_output_size));
    } else {
        return opus_multistream_encode_float(
            ms_encoder_, pcm,
            static_cast<int>(frame_count),
            output,
            static_cast<opus_int32>(max_output_size));
    }
}

bool OpusEncoderWrapper::is_initialized() const {
    return encoder_ != nullptr || ms_encoder_ != nullptr;
}

uint32_t OpusEncoderWrapper::frame_size_samples() const {
    return frame_size_samples_;
}

uint32_t OpusEncoderWrapper::channels() const {
    return channels_;
}

void OpusEncoderWrapper::destroy() {
    if (encoder_) {
        opus_encoder_destroy(encoder_);
        encoder_ = nullptr;
    }
    if (ms_encoder_) {
        opus_multistream_encoder_destroy(ms_encoder_);
        ms_encoder_ = nullptr;
    }
}

// --- OpusDecoderWrapper ---

OpusDecoderWrapper::OpusDecoderWrapper() = default;

OpusDecoderWrapper::~OpusDecoderWrapper() {
    destroy();
}

OpusDecoderWrapper::OpusDecoderWrapper(OpusDecoderWrapper&& other) noexcept
    : decoder_(other.decoder_),
      ms_decoder_(other.ms_decoder_),
      sample_rate_(other.sample_rate_),
      channels_(other.channels_) {
    other.decoder_ = nullptr;
    other.ms_decoder_ = nullptr;
}

OpusDecoderWrapper& OpusDecoderWrapper::operator=(OpusDecoderWrapper&& other) noexcept {
    if (this != &other) {
        destroy();
        decoder_ = other.decoder_;
        ms_decoder_ = other.ms_decoder_;
        sample_rate_ = other.sample_rate_;
        channels_ = other.channels_;
        other.decoder_ = nullptr;
        other.ms_decoder_ = nullptr;
    }
    return *this;
}

bool OpusDecoderWrapper::init(const OpusDecoderConfig& config) {
    destroy();

    if (config.channels == 0) return false;

    sample_rate_ = config.sample_rate;
    channels_ = config.channels;

    int err = 0;

    if (channels_ <= 2) {
        decoder_ = opus_decoder_create(
            static_cast<opus_int32>(sample_rate_),
            static_cast<int>(channels_),
            &err);
        if (err != OPUS_OK || !decoder_) {
            decoder_ = nullptr;
            return false;
        }
    } else {
        // Multistream decoder for ambisonics (mapping_family=2):
        // All streams are uncoupled mono with identity mapping.
        int streams = static_cast<int>(channels_);
        int coupled_streams = 0;
        unsigned char mapping[255] = {};
        for (int i = 0; i < streams; ++i) {
            mapping[i] = static_cast<unsigned char>(i);
        }

        ms_decoder_ = opus_multistream_decoder_create(
            static_cast<opus_int32>(sample_rate_),
            static_cast<int>(channels_),
            streams,
            coupled_streams,
            mapping,
            &err);
        if (err != OPUS_OK || !ms_decoder_) {
            ms_decoder_ = nullptr;
            return false;
        }
    }

    return true;
}

int OpusDecoderWrapper::decode(const uint8_t* data, uint32_t data_len,
                                float* pcm, uint32_t max_frame_count) {
    if (!is_initialized()) return -1;

    if (decoder_) {
        return opus_decode_float(
            decoder_,
            data,
            static_cast<opus_int32>(data_len),
            pcm,
            static_cast<int>(max_frame_count),
            /*decode_fec=*/0);
    } else {
        return opus_multistream_decode_float(
            ms_decoder_,
            data,
            static_cast<opus_int32>(data_len),
            pcm,
            static_cast<int>(max_frame_count),
            /*decode_fec=*/0);
    }
}

int OpusDecoderWrapper::decode_plc(float* pcm, uint32_t frame_count) {
    if (!is_initialized()) return -1;

    if (decoder_) {
        return opus_decode_float(
            decoder_,
            nullptr, 0,
            pcm,
            static_cast<int>(frame_count),
            /*decode_fec=*/0);
    } else {
        return opus_multistream_decode_float(
            ms_decoder_,
            nullptr, 0,
            pcm,
            static_cast<int>(frame_count),
            /*decode_fec=*/0);
    }
}

bool OpusDecoderWrapper::is_initialized() const {
    return decoder_ != nullptr || ms_decoder_ != nullptr;
}

uint32_t OpusDecoderWrapper::channels() const {
    return channels_;
}

void OpusDecoderWrapper::destroy() {
    if (decoder_) {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    if (ms_decoder_) {
        opus_multistream_decoder_destroy(ms_decoder_);
        ms_decoder_ = nullptr;
    }
}

// --- PCM framing ---

uint32_t pcm_frame(const float* pcm, uint32_t frame_count, uint32_t channels,
                    uint8_t* output) {
    uint32_t total_samples = frame_count * channels;
    uint32_t byte_count = total_samples * sizeof(float);
    std::memcpy(output, pcm, byte_count);
    return byte_count;
}

uint32_t pcm_unframe(const uint8_t* data, uint32_t data_len,
                      float* pcm, uint32_t channels) {
    if (channels == 0) return 0;
    uint32_t sample_bytes = channels * sizeof(float);
    // Reject misaligned data
    if (data_len % sample_bytes != 0) return 0;

    uint32_t total_samples = data_len / sizeof(float);
    std::memcpy(pcm, data, data_len);
    return total_samples / channels;  // frame count
}

}  // namespace panaudia
