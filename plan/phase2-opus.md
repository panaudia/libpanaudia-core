# Phase 2: Opus Codec

**Goal:** A standalone Opus encoder/decoder that supports per-track configuration (channels, bitrate, frame size). No UE dependencies.

**Depends on:** Phase 0 (project compiles, libopus available via FetchContent)
**Blocks:** Phase 4 (send/recv workers need codec)

**Source:** Extract from UE plugin `PanaudiaOpusEncoder.cpp` and decode logic in `PanaudiaConnectionManager.cpp`

## Tasks

### Examine UE Plugin Code
- [ ] Read `PanaudiaOpusEncoder.cpp` — understand encode API, buffer management, frame sizing
- [ ] Read decode path in `PanaudiaConnectionManager.cpp` — find where Opus decode happens, extract logic
- [ ] Identify all UE-specific types/APIs used (FString, TArray, UE_LOG, etc.)
- [ ] Note the current frame size (5ms), sample rate (48kHz), channel count (mono), bitrate

### OpusCodec Class
- [ ] `opus_codec.h` / `opus_codec.cpp`
- [ ] Config struct:
  - `sample_rate` (default 48000)
  - `channels` (1=mono, 2=stereo, up to 8 for ambisonics)
  - `bitrate` (default 64000)
  - `frame_size_ms` (default 5, valid: 2.5, 5, 10, 20, 40, 60)
- [ ] Encoder:
  - [ ] `OpusEncoder` wrapper — create/destroy opus encoder
  - [ ] `int encode(const float* pcm, uint32_t frame_count, uint8_t* output, uint32_t max_output_size)`
  - [ ] Returns encoded byte count, or negative on error
  - [ ] Frame size validation: input frame_count must match configured frame size
- [ ] Decoder:
  - [ ] `OpusDecoder` wrapper — create/destroy opus decoder
  - [ ] `int decode(const uint8_t* data, uint32_t data_len, float* pcm, uint32_t max_frame_count)`
  - [ ] Returns decoded frame count, or negative on error
  - [ ] Packet loss concealment: `decode(nullptr, 0, ...)` generates PLC output
- [ ] Pre-allocated encode/decode buffers (no allocation per frame)
- [ ] DTX (discontinuous transmission) handling:
  - [ ] Silence frames from Opus can be as small as 3 bytes — must not be rejected
  - [ ] Reference: this was a bug we fixed in the Go server (see MEMORY.md)

### Multi-Channel Support
- [ ] Verify libopus multi-channel API works for:
  - [ ] Mono (1ch)
  - [ ] Stereo (2ch)
  - [ ] First-order ambisonics (4ch) — may need `opus_multistream_encoder`
  - [ ] Higher-order ambisonics (9ch, 16ch) — `opus_multistream_encoder` required
- [ ] Determine: does the UE plugin use `opus_encode_float` (single-stream) or `opus_multistream_encode_float`?
- [ ] If FOA/HOA needed for V1, implement multistream wrapper. If not, mono+stereo may suffice initially.

### PCM Framing (No-Codec Path)
- [ ] For `AudioCodec::PCM` tracks, no Opus encode/decode — just frame raw PCM into datagram-sized chunks
- [ ] `PcmFramer` class (or simple functions):
  - [ ] `frame(const float* pcm, uint32_t frame_count, uint32_t channels, uint8_t* output)` — pack floats into network byte order
  - [ ] `unframe(const uint8_t* data, uint32_t data_len, float* pcm, uint32_t channels)` — unpack
- [ ] Decide byte order: little-endian (native on x86/ARM) or network byte order (big-endian)?
  - Recommendation: little-endian (both platforms are LE, avoids conversion cost)

### Tests
- [ ] Encode/decode roundtrip — mono, verify PCM out ≈ PCM in (within Opus lossy tolerance)
- [ ] Encode/decode roundtrip — stereo
- [ ] Various frame sizes — 5ms, 10ms, 20ms at 48kHz
- [ ] Various bitrates — 32k, 64k, 128k
- [ ] DTX silence frames — encode silence, verify small packet produced, verify it decodes
- [ ] Packet loss concealment — decode with nullptr input, verify output is plausible (not silence, not garbage)
- [ ] PCM framing roundtrip — frame and unframe, verify exact match
- [ ] Multi-channel if implemented — FOA encode/decode roundtrip
- [ ] Error cases — invalid frame size, zero-length input, oversized input

## Done When
- Opus encode + decode works for mono and stereo at configurable frame sizes and bitrates
- PCM framing works for all channel counts
- No UE dependencies remain
- All tests pass
- No per-frame memory allocation (verified by inspection or allocator instrumentation)
