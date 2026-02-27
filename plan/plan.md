# libpanaudia-core — Architecture & Implementation Plan

## Purpose

A cross-platform C++ library that provides multitrack bidirectional streaming over MOQ/QUIC. Handles audio tracks (with optional Opus codec, ring buffers, and jitter buffering) and data tracks (opaque byte transport). Exposes a C++ API for integration with platform-specific hosts.

The core is deliberately minimal — it has no knowledge of application semantics (mute, position, state formats, namespace conventions), platform audio APIs (CoreAudio, PortAudio), IPC mechanisms (gRPC, libASPL), or UI. These concerns belong to the host.

## Working Practices

**Roles.** The agent (Claude) does most of the coding. The human reviews, tests on real hardware, and makes architectural calls.

**Tests come first, and failing tests are fine.** Write tests alongside or before implementation. A failing test that exposes a real issue is valuable — leave it failing and flag it. Do not weaken tests to make them pass. Do not skip tests because the implementation isn't ready. Red tests are a signal, not a problem.

**Stop and talk when things get complicated.** If the implementation is becoming more complex than expected, or heading in an unforeseen direction, stop coding and discuss. Do not push through complexity hoping it will resolve — surface it early.

**No surprise dependencies.** The approved dependency list is: msquic, libopus, and a test framework (Catch2 or Google Test). Do not add any other dependency without asking first. This includes header-only libraries, build tools, or "small" utility libraries.

**One phase file in context at a time.** When working on a phase, load that phase's markdown file. Tick off checkboxes as tasks complete. If a task turns out to need rethinking, update the checkbox description rather than silently diverging.

## Context

This library is the shared core described in the [Multitrack Native Client plan](../../spatial-mixer/plan/multitrack/plan.md). Detailed interface design is in [interfaces.md](../../spatial-mixer/plan/multitrack/interfaces.md).

```
Layer 4: UI / App Shell              (platform-specific)
Layer 3: Platform Audio + IPC        (platform-specific)
Layer 2: Track Management            <-- libpanaudia-core
Layer 1: MOQ/QUIC Transport          <-- libpanaudia-core
```

### Hosting Model (Option 4)

On macOS, the core runs **inside the HAL driver** (coreaudiod process), alongside libASPL. Audio never crosses process boundaries — all ring buffers are in-process SPSC. A gRPC server in the driver exposes control to a separate Swift app.

On Linux, the core runs **inside the app process** with PortAudio callbacks feeding the ring buffers directly. No IPC needed.

The core's internal architecture is identical on both platforms. Only the hosting context differs.

## Lineage — Unreal Plugin

The starting point is the working MOQ/QUIC implementation in the Unreal Engine plugin (`../panaudia-client/sdks/unreal/`). Key source files to extract from:

| UE Plugin File | Functionality | Core Equivalent |
|----------------|---------------|-----------------|
| `PanaudiaConnectionManager.cpp` | QUIC connection, MOQ handshake, track subscribe/publish | `moq_session.cpp` — MOQ session lifecycle |
| `PanaudiaOpusEncoder.cpp` | Thread-safe Opus encoding | `opus_codec.cpp` — encode + decode |
| `PanaudiaProceduralSound.cpp` | Jitter buffer reads | `jitter_buffer.cpp` |
| `PanaudiaAudioComponent.cpp` | Audio capture/playback routing | (not extracted — platform-specific) |

The extraction involves:
- Removing all UE dependencies (FString, TArray, UE logging, UE threading)
- Replacing with standard C++ (std::string, std::vector, std::thread)
- Generalising from single-track to multitrack with per-track configuration
- Exposing a C++ class API (not C function API)

---

## Architecture

### Component Overview

```
+----------------------------------------------------------+
|                    libpanaudia-core                       |
|                                                          |
|  +----------------------------------------------------+  |
|  |           PanaudiaCore (C++ class)                  |  |
|  |  configure(), connect(), disconnect()               |  |
|  |  get_track(), write_audio(), read_audio()           |  |
|  |  send_data(), update_jwt()                          |  |
|  |  Callbacks: DataRecvCallback, StatusCallback, Log   |  |
|  +------------------------+---------------------------+  |
|                           |                              |
|  +------------------------v---------------------------+  |
|  |           Session Manager                          |  |
|  |  - Track lifecycle (from SessionConfig)            |  |
|  |  - TrackHandle resolution (name → handle)          |  |
|  |  - MOQ subscribe/publish per track                 |  |
|  +---------+------------------+----------+-----------+  |
|            |                  |          |               |
|  +---------v-------+  +------v---+  +---v----------+   |
|  |  MOQ Transport  |  |  Opus    |  |  Ring Buffer |   |
|  |  - QUIC(msquic) |  |  Codec   |  |  - Per-track |   |
|  |  - MOQ protocol |  |  - Encode|  |  - SPSC      |   |
|  |  - Datagrams    |  |  - Decode|  |  - Lock-free |   |
|  +-----------------+  +----------+  +--------------+   |
|                                                          |
|  +------------------+                                    |
|  |  Jitter Buffer   |                                    |
|  |  - Per inbound   |                                    |
|  |    audio track   |                                    |
|  +------------------+                                    |
+----------------------------------------------------------+
```

### Unified Track Model

The core has a single concept: **tracks**. Every track is a named MOQ channel. Tracks differ only in how the core processes their payload:

| Track type | Core processing | Host interaction |
|------------|----------------|------------------|
| **Audio** | Codec (Opus/PCM), ring buffer, jitter buffer | RT `write_audio` / `read_audio` via handle |
| **Data** | None — opaque bytes | `send_data` / `DataRecvCallback` via handle |

The core does not distinguish between "state", "attributes", "control", "mute", or any other application concept. These are all just data tracks with different names and MOQ namespaces. Application semantics live in the host.

### Track Configuration

Each track is configured with:
- **Name** — local handle name for `get_track()` lookup
- **MOQ namespace** — full namespace tuple, constructed by the host, used verbatim
- **Direction** — outbound (publish) or inbound (subscribe)
- **Type** — Audio or Data
- **Audio parameters** (audio tracks only) — channels, sample rate, codec (Opus/PCM), bitrate, frame size

Three orthogonal per-track audio dimensions:

- **Channels** (within one MOQ track) — sample-aligned, for coherent spatial audio (stereo, ambisonics)
- **Codec** — Opus (compressed) or PCM (uncompressed), per-track choice
- **Track count** — independent MOQ tracks, no timing guarantee between them

### Subscription Model

The client does not discover tracks dynamically. It connects with an **expectation of format and count**: all tracks — audio and data, inbound and outbound — are declared upfront in `SessionConfig`. The core subscribes/publishes based on this configuration.

For V1, track configuration is fixed at session creation. Dynamic add/remove can be added later.

### Threading Model

```
Host RT thread         - write_audio / read_audio (lock-free ring buffer access only)
Host control thread    - configure, connect, disconnect, send_data, get_track
QUIC I/O thread(s)     - msquic callbacks, MOQ message parsing
Send thread(s)         - reads outbound ring buffers, encodes (Opus/PCM framing), MOQ publish
Recv thread(s)         - MOQ subscribe, decodes, writes inbound ring buffers
```

The RT thread (owned by the host — libASPL on macOS, PortAudio on Linux) never encodes, decodes, or touches the network. It only reads/writes lock-free ring buffers. Dedicated send/recv worker threads handle codec and transport.

### Audio Buffers — Two Types

The core has two distinct buffer types for audio tracks. The design is adapted from the Go server's circular buffer (`core/buffers/circular_buffer.go`). Full design details in [jitter_buffer_design.md](../../spatial-mixer/plan/jitter_buffer_design.md).

**Outbound audio → `RingBuffer` (plain SPSC)**

A lock-free single-producer single-consumer ring buffer. No jitter logic — the RT thread writes at a steady OS audio clock rate, the send thread reads and encodes/frames. All outbound audio uses a ring buffer even for PCM (uncompressed) tracks, because msquic's `DatagramSend()` is not RT-safe (it allocates internally and may lock).

- RT thread writes (producer), send thread reads (consumer)
- Overflow: writer drops oldest data
- `alignas(64)` on atomic positions prevents false sharing

**Inbound audio → `JitterBuffer` (ring buffer + state machine + drift correction)**

A ring buffer with a FILLING/PLAYING state machine and single-zone drift correction. Required because network packets arrive irregularly while the RT callback demands samples at a strict cadence.

- Recv thread writes after decode (producer), RT thread reads (consumer)
- **FILLING state:** outputs silence until fill reaches TargetLow, then transitions to PLAYING. Correction counter reset on transition.
- **PLAYING state:** normal audio output with ±1 sample drift correction every CorrectionInterval reads when fill is outside the target window.
- **Underrun** (fill < Min): transitions back to FILLING, outputs silence.
- **Overrun** (fill > Max): snaps read position forward to TargetCentre, discarding stale audio.
- `alignas(64)` on atomic positions prevents false sharing

**Thread safety adaptation from Go:** The Go buffer is single-threaded. The C++ version uses atomic operations for the shared `buffered` counter (SPSC pattern). `writePos` is writer-owned, `readPos` is reader-owned. On overflow, the writer sets an atomic flag rather than directly moving `readPos` — the reader checks the flag and performs the snap itself.

**Configuration** (from `SessionConfig`):

| SessionConfig field          | JitterBuffer parameter | Default |
|------------------------------|------------------------|---------|
| `jitter_buffer_initial_ms`   | `TargetLatencyMs`      | 60      |
| `jitter_buffer_min_ms`       | `MinLatencyMs`         | 10      |
| `jitter_buffer_max_ms`       | `MaxLatencyMs`         | 200     |

Additional defaults (not exposed in SessionConfig): TargetWindowMs=20, CapacityMs=1000, CorrectionInterval=16.

---

## C++ API Surface

The complete API is defined in [interfaces.md §8](../../spatial-mixer/plan/multitrack/interfaces.md). Summary:

### PanaudiaCore Class

```cpp
class PanaudiaCore {
public:
    // Lifecycle (host control thread)
    void configure(const SessionConfig& config);
    void connect();
    void disconnect();
    void update_jwt(const std::string& jwt);

    // Track handle lookup (after configure)
    TrackHandle* get_track(const std::string& name);

    // Realtime audio (host RT thread — must be lock-free)
    void write_audio(TrackHandle* track, const float* samples,
                     uint32_t frame_count, uint64_t host_time);
    uint32_t read_audio(TrackHandle* track, float* buffer,
                        uint32_t frame_count, uint64_t host_time);

    // Data send (host control thread)
    void send_data(TrackHandle* track, const uint8_t* data, uint32_t data_len);

    // Status (thread-safe, lock-free reads)
    ConnectionState get_connection_state() const;
    BufferStatus get_buffer_status(TrackHandle* track) const;
    SessionStats get_stats() const;
};
```

### Key Types

```cpp
struct TrackConfig {
    std::string name;                        // local handle name
    std::vector<std::string> moq_namespace;  // full MOQ namespace tuple
    std::string moq_track_name;              // MOQ track name (usually empty)
    TrackDirection direction;
    TrackType type = TrackType::Data;

    // Audio-specific (ignored when type == Data)
    uint32_t channels = 1;
    uint32_t sample_rate = 48000;
    AudioCodec codec = AudioCodec::Opus;
    uint32_t opus_bitrate = 64000;
    uint32_t opus_frame_size_ms = 5;
    uint32_t pcm_frame_size_ms = 5;
};

struct SessionConfig {
    std::string server_url;
    std::string jwt;
    std::vector<TrackConfig> tracks;

    uint32_t jitter_buffer_min_ms = 10;
    uint32_t jitter_buffer_max_ms = 200;
    uint32_t jitter_buffer_initial_ms = 60;

    DataRecvCallback data_recv_callback = nullptr;
    void* data_recv_ctx = nullptr;
    StatusCallback status_callback = nullptr;
    void* status_ctx = nullptr;
    LogCallback log_callback = nullptr;
    void* log_ctx = nullptr;
    LogLevel log_level = LogLevel::Info;
};
```

### Three Callbacks

```cpp
using DataRecvCallback = void(*)(TrackHandle* track, const uint8_t* data,
                                  uint32_t data_len, void* ctx);
using StatusCallback = void(*)(ConnectionState state, const char* message, void* ctx);
using LogCallback = void(*)(LogLevel level, const char* message, void* ctx);
```

---

## What the Core Does NOT Do

The core is deliberately ignorant of:

| Not in core | Where it lives |
|-------------|---------------|
| OSC server | Host (Swift app on macOS, Linux app) |
| JWT parsing / Ed25519 verification | Host — core just stores and sends the token string |
| Mute logic | Host sends bytes on a control data track |
| Position data interpretation | Host packs/unpacks state bytes |
| MOQ namespace conventions | Host constructs full tuples, core uses verbatim |
| gRPC / IPC | macOS driver — not a core concern |
| libASPL / VAD management | macOS driver |
| PortAudio | Linux app |
| UI | Platform app |
| Logging framework | Core produces via LogCallback — no spdlog or similar |

---

## Dependencies

| Library | Purpose | License |
|---------|---------|---------|
| msquic | QUIC transport | MIT |
| libopus | Audio codec (Opus tracks only) | BSD-3 |

That's it. No OSC library, no logging library, no JSON library, no JWT library. No Unreal Engine, no Apple, no Linux-specific dependencies.

---

## Build System

CMake, targeting:
- macOS (arm64, x86_64) — consumed by HAL driver via direct C++ linking
- Linux (x86_64, aarch64) — consumed by C++ client app

Dependencies via CMake `FetchContent`:
- msquic (pinned version)
- libopus (pinned version)

Output artifacts:
- `libpanaudia-core.a` (static library — preferred, avoids dylib versioning issues)
- `panaudia_core.h` (C++ API header)
- Optional: `libpanaudia-core.dylib` / `.so` for dynamic linking

### Build Targets

```
libpanaudia-core       — static library
panaudia-core-tests    — unit tests (ring buffer, jitter buffer, codec, session)
panaudia-test-harness  — standalone executable simulating RT callback pattern
```

---

## Internal Source Structure (Proposed)

```
libpanaudia-core/
├── CMakeLists.txt
├── include/
│   └── panaudia/
│       └── core.h                 # Public C++ API (PanaudiaCore, TrackConfig, etc.)
├── src/
│   ├── panaudia_core.cpp          # PanaudiaCore implementation
│   ├── session_manager.cpp        # Track lifecycle, MOQ subscribe/publish orchestration
│   ├── moq_transport.cpp          # MOQ protocol over msquic (extracted from UE plugin)
│   ├── opus_codec.cpp             # Opus encode/decode (extracted from UE plugin)
│   ├── ring_buffer.cpp            # SPSC lock-free ring buffer
│   ├── jitter_buffer.cpp          # Per-track adaptive jitter buffer
│   ├── send_worker.cpp            # Outbound: ring buf → codec → MOQ publish
│   └── recv_worker.cpp            # Inbound: MOQ subscribe → codec → ring buf
├── tests/
│   ├── test_ring_buffer.cpp
│   ├── test_jitter_buffer.cpp
│   ├── test_opus_codec.cpp
│   ├── test_session.cpp
│   └── test_harness.cpp           # Simulates RT callbacks at 48kHz timing
└── plan/
    └── plan.md                    # This document
```

---

## Key Design Decisions

### Resolved

1. **C++ API, not C API** — The core exposes a C++ class (`PanaudiaCore`). Both the macOS driver (C++) and the Linux app (C++) consume it directly. No C wrapper needed — there is no Swift FFI boundary at the core level (Swift talks to the driver via gRPC, not to the core directly).

2. **No OSC in core** — OSC is the host's concern. The host receives OSC messages, packs them into binary state data, and sends via `send_data()` on a data track. This keeps the core protocol-agnostic.

3. **No JWT parsing** — The core stores the JWT string and sends it as the MOQ authorization parameter. No Ed25519, no token parsing, no expiry checking.

4. **Unified track model** — Audio and data tracks are both "tracks" configured in `SessionConfig`. No separate concepts for mute, position, state, control. All application semantics are opaque bytes on named data tracks.

5. **Handle-based addressing** — Configure tracks by name, operate by opaque `TrackHandle*` pointer. No string lookups on the RT path.

6. **In-process ring buffers** — No shared memory. The core runs in the same process as the audio RT thread (inside coreaudiod on macOS, inside the app on Linux).

7. **Host constructs MOQ namespaces** — The core uses namespace tuples from `TrackConfig` verbatim. Panaudia's namespace conventions (`/in/audio/opus-mono/{nodeId}/0`) can evolve without changing the core.

8. **Minimal dependencies** — msquic + libopus only. No logging framework (LogCallback), no OSC library, no JSON library.

9. **Clock drift** — Handled by the jitter buffer's single-zone drift correction (±1 sample every CorrectionInterval reads). At CorrectionInterval=16 with 5ms reads, this provides ~12.5 corrections/sec — far exceeding typical crystal drift (50 ppm ≈ 2.4 samples/sec at 48kHz). See [jitter_buffer_design.md](../../spatial-mixer/plan/jitter_buffer_design.md).

10. **Error recovery** — Automatic reconnection with exponential backoff (100ms, 200ms, 400ms, ..., capped at 10s). On reconnect, re-subscribe/re-publish all tracks from the immutable `SessionConfig`. Inbound jitter buffers handle the gap naturally (transition to FILLING → silence → re-accumulate → PLAYING). Outbound ring buffers flushed on reconnect to avoid sending stale audio. `StatusCallback` reports state transitions: Connected → Reconnecting → Connected (or Failed). Host can also force manual recovery via `disconnect()` + `connect()`.

11. **Track count changes** — V1: fixed at session creation. To change tracks: `disconnect()` → new `SessionConfig` → `configure()` → `connect()`. Dynamic add/remove deferred.

12. **MOQ protocol implementation** — Port the UE plugin's raw msquic/MOQ C++ code. No CGo, no Go runtime. msquic is MIT-licensed, well-maintained by Microsoft, and already proven in the UE plugin. Maintaining a separate C++ MOQ implementation is acceptable — the protocol surface we use (datagrams, subscribe/publish, handshake) is small.

13. **Opus frame size** — Per-track configurable via `TrackConfig.opus_frame_size_ms`, default 5ms (matching UE plugin). Hosts can set larger frames (10-20ms) for tracks where CPU matters more than latency.

14. **Thread pool vs per-track threads** — One send thread and one recv thread per audio track for V1. Simpler, debuggable, and sufficient for expected track counts (8-16). Thread pooling can be added later if needed.

---

## Phased Implementation Plan

Each phase has a detailed task breakdown with checkboxes. See individual files:

| Phase | Focus | Detail |
|-------|-------|--------|
| 0 | Repository & Build Setup | [phase0-repo-setup.md](phase0-repo-setup.md) |
| 1 | Ring Buffer & Jitter Buffer | [phase1-buffers.md](phase1-buffers.md) |
| 2 | Opus Codec | [phase2-opus.md](phase2-opus.md) |
| 3 | MOQ/QUIC Transport | [phase3-moq-transport.md](phase3-moq-transport.md) |
| 4 | Session Manager & PanaudiaCore | [phase4-session-manager.md](phase4-session-manager.md) |
| 5 | Test Harness & Integration | [phase5-testing.md](phase5-testing.md) |
| 6 | Platform Integration Prep | [phase6-platform-integration.md](phase6-platform-integration.md) |

**Dependencies:**
```
Phase 0 ──► Phase 1 (buffers) ──┐
         ──► Phase 2 (codec)  ──┼──► Phase 4 (session) ──► Phase 5 (testing) ──► Phase 6 (platform)
         ──► Phase 3 (transport)┘
```

Phases 1, 2, and 3 can proceed in parallel once Phase 0 is complete.
