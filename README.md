# libpanaudia-core v0.1.0

A cross-platform C++17 library for multitrack spatial audio streaming over MOQ/QUIC. It provides the shared streaming infrastructure used by Panaudia's native clients -- handling QUIC transport, MOQ protocol, Opus encoding/decoding, multitrack session management, and adaptive jitter buffering, all behind a clean C++ API.

## What It Does

libpanaudia-core connects to a Panaudia server and manages bidirectional streaming of multiple audio and data tracks over a single QUIC connection. Each session supports an arbitrary number of concurrent tracks -- both audio and data, inbound and outbound. Audio tracks are multi-channel (mono, stereo, or ambisonics) with a per-track choice of codec: Opus for compressed low-latency streaming, or uncompressed PCM for applications where encoding latency or lossy compression is unacceptable.

A host application configures the library with a set of named tracks, calls `connect()`, and then:

- **Writes captured audio** into lock-free ring buffers from a real-time audio thread
- **Reads decoded audio** from adaptive jitter buffers on a real-time audio thread
- **Sends and receives opaque data** (position updates, control messages, etc.) on data tracks

The library handles everything between the ring buffers and the network: Opus encoding/decoding, MOQ framing, QUIC transport via msquic, session orchestration (SUBSCRIBE/ANNOUNCE handshakes), JWT authentication, and automatic reconnection with exponential backoff.

## Architecture

```
Host Application
  │
  ├─ RT audio thread ──► write_audio() ──► RingBuffer ──┐
  │                                                      │  Session Thread
  │                                                      ├─► Opus Encode ──► MOQ Datagram ──► msquic ──► Network
  │                                                      │
  ├─ RT audio thread ◄── read_audio() ◄── JitterBuffer ◄┤
  │                                                      ├─◄ Opus Decode ◄── MOQ Datagram ◄── msquic ◄── Network
  │                                                      │
  ├─ Control thread ──► send_data() ────────────────────►├─► MOQ Datagram ──► msquic ──► Network
  │                                                      │
  └─ DataRecvCallback ◄─────────────────────────────────◄┘◄── MOQ Datagram ◄── msquic ◄── Network
```

### Components

| Component | Role |
|---|---|
| **PanaudiaCore** | Public API entry point. Thin wrapper around SessionManager. |
| **SessionManager** | Track lifecycle, MOQ subscribe/announce orchestration, session thread loop, reconnection. |
| **MoqTransport** | Raw QUIC connection (msquic) + MOQ handshake + control stream framing. No track knowledge. |
| **moq_protocol** | Stateless MOQ wire format encoder/decoder functions. |
| **RingBuffer** | SPSC lock-free buffer for outbound audio (RT thread writes, session thread reads). |
| **JitterBuffer** | Adaptive SPSC buffer for inbound audio with drift correction (session thread writes, RT thread reads). |
| **OpusEncoderWrapper / OpusDecoderWrapper** | libopus wrappers supporting mono, stereo, and multistream ambisonics. |

### Threading Model

| Thread | Owned by | Does what | Constraints |
|---|---|---|---|
| Host RT thread | Platform audio layer | Calls `write_audio()` / `read_audio()` | Lock-free only: no alloc, no locks, no syscalls |
| Session thread | SessionManager (spawned by `connect()`) | Polls transport, drains ring buffers, encodes/decodes, sends/receives datagrams | Not RT. May allocate. |
| msquic I/O thread(s) | msquic internals | Fires datagram callbacks, queues control stream data | Must return quickly |

### Key Design Decisions

- **Static subscription model** -- all tracks (audio and data, inbound and outbound) are declared upfront in `SessionConfig`. The client connects with a known expectation of track count and format rather than discovering tracks dynamically. To change tracks: disconnect, reconfigure, reconnect.
- **Unified track model** -- audio and data tracks share the same addressing. The library has no concept of "state", "control", or "mute" -- those are just data tracks with different names. Application semantics are opaque bytes; only the host interprets them.
- **Handle-based addressing** -- tracks are configured by name but operated by opaque `TrackHandle*` pointers. No string lookups on the real-time path.
- **Zero-allocation audio send path** -- each outbound audio track pre-allocates its PCM read buffer, Opus encode buffer, and datagram send buffer at configure time. The session thread's per-frame encode-and-send loop never allocates.
- **Cross-platform hosting model** -- the same core runs inside the macOS CoreAudio HAL driver process (coreaudiod, alongside libASPL) or directly inside the app process on Linux (with PortAudio). Audio never crosses process boundaries -- all ring buffers are in-process SPSC.
- **Opus DTX** -- discontinuous transmission is enabled by default, reducing bandwidth to ~3 bytes/frame during silence.
- **Ambisonics** -- mono and stereo tracks use the standard Opus API; 3+ channel tracks (first-order ambisonics, etc.) automatically use `opus_multistream` with ambisonics mapping family 2.
- **48 kHz audio** -- sample rate is configurable per-track via `TrackConfig.sample_rate` but defaults to 48000 Hz, which is the only rate used in practice across all Panaudia clients.

### What the Library Does NOT Do

The library is deliberately transport-only. These concerns belong to the host application:

- JWT parsing or verification (core stores and sends the JWT string verbatim)
- MOQ namespace construction (host provides full namespace tuples; core uses them as-is)
- Spatial audio, muting, position tracking (host sends these as opaque bytes on data tracks)
- Platform audio I/O (libASPL, PortAudio, Unreal AudioCapture, etc.)
- IPC / gRPC, logging frameworks, JSON, protobuf



## Documentation

- [Guide](docs/guide.md) -- how to use this library
- [MOQ Protocol Compatibility](docs/moq-compatibility.md) -- protocol version, wire format, server library targeting
- [Wire Format Byte-Level Reference](plan/moq_protocol_wire_format.md) -- encoding examples and field layouts
- [Architecture and Implementation Plan](plan/plan.md) -- design decisions and phase history
