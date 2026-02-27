# Phase 2: Opus Codec

**Status: COMPLETE** — CI green on all 3 jobs (build, test, TSan). Commit `159c6dd`.

**Goal:** A standalone Opus encoder/decoder that supports per-track configuration (channels, bitrate, frame size). No UE dependencies.

**Depends on:** Phase 0 (project compiles, libopus available via FetchContent)
**Blocks:** Phase 4 (send/recv workers need codec)

**Source:** Extract from UE plugin `PanaudiaOpusEncoder.cpp` and decode logic in `PanaudiaConnectionManager.cpp`

## Tasks

### Examine UE Plugin Code
- [x] Read `PanaudiaOpusEncoder.cpp` — understand encode API, buffer management, frame sizing
- [x] Read decode path in `PanaudiaConnectionManager.cpp` — find where Opus decode happens, extract logic
- [x] Identify all UE-specific types/APIs used (FString, TArray, UE_LOG, etc.)
- [x] Note the current frame size (5ms), sample rate (48kHz), channel count (mono), bitrate

### OpusCodec Class
- [x] `opus_codec.h` / `opus_codec.cpp`
- [x] Config struct:
  - `sample_rate` (default 48000)
  - `channels` (1=mono, 2=stereo, up to 16 for ambisonics)
  - `bitrate` (default 64000)
  - `frame_size_ms` (default 5, valid: 5, 10, 20, 40, 60)
- [x] Encoder:
  - [x] `OpusEncoderWrapper` — create/destroy opus encoder
  - [x] `int encode(const float* pcm, uint32_t frame_count, uint8_t* output, uint32_t max_output_size)`
  - [x] Returns encoded byte count, or negative on error
  - [x] Frame size validation: input frame_count must match configured frame size
- [x] Decoder:
  - [x] `OpusDecoderWrapper` — create/destroy opus decoder
  - [x] `int decode(const uint8_t* data, uint32_t data_len, float* pcm, uint32_t max_frame_count)`
  - [x] Returns decoded frame count, or negative on error
  - [x] Packet loss concealment: `decode_plc(pcm, frame_count)` generates PLC output
- [x] No per-frame allocation — caller provides input/output buffers
- [x] DTX (discontinuous transmission) handling:
  - [x] Silence frames from Opus can be as small as 3 bytes — must not be rejected
  - [x] Verified: DTX produces packets ≤ 6 bytes after ~50 silence frames

### Multi-Channel Support
- [x] Verify libopus multi-channel API works for:
  - [x] Mono (1ch) — simple API `opus_encode_float`/`opus_decode_float`
  - [x] Stereo (2ch) — simple API
  - [x] First-order ambisonics (4ch) — multistream API with `mapping_family=2`
  - [x] Higher-order ambisonics (9ch SOA, 16ch TOA) — multistream API
- [x] UE plugin uses `opus_encode_float` (single-stream, mono only)
- [x] Implemented multistream wrapper: `opus_multistream_surround_encoder_create` for 3+ch, identity mapping decoder

### PCM Framing (No-Codec Path)
- [x] For `AudioCodec::PCM` tracks — free functions, not a class
- [x] `pcm_frame(pcm, frame_count, channels, output)` — memcpy floats to bytes
- [x] `pcm_unframe(data, data_len, pcm, channels)` — memcpy bytes to floats, rejects misaligned data
- [x] Byte order: little-endian (both target platforms are LE)

### Tests (30 test cases, 4772 assertions total with Phase 1)
- [x] Encode/decode roundtrip — mono 5ms, stereo 10ms, mono 20ms
- [x] Encode/decode roundtrip — FOA 4ch, SOA 9ch
- [x] Various frame sizes — 5ms, 10ms, 20ms at 48kHz
- [x] Various bitrates — 32k, 64k, 128k
- [x] DTX silence frames — encode silence, verify small packet, verify decodes
- [x] Packet loss concealment — prime decoder, decode_plc, verify non-silent output
- [x] PCM framing roundtrip — mono, stereo, 4ch exact match
- [x] PCM unframe rejects misaligned data
- [x] Error cases — wrong frame size, encode without init
- [x] Move semantics — encoder and decoder

### Benchmarks (macOS Apple Silicon, Release)
- [x] Opus encode mono 5ms: ~13us
- [x] Opus decode mono 5ms: ~3.6us
- [x] Opus encode+decode cycle: ~19us
- [x] Opus encode FOA 5ms: ~48us
- [x] PCM frame+unframe: ~25ns

## Done When
- [x] Opus encode + decode works for mono and stereo at configurable frame sizes and bitrates
- [x] PCM framing works for all channel counts
- [x] No UE dependencies remain
- [x] All tests pass
- [x] No per-frame memory allocation (verified by inspection — caller provides all buffers)
