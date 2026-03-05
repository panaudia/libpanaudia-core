# Panaudia Guide

## Intended Consumers

The existing Panaudia Unreal plugin uses libpanaudia-core, we are planning on two other native clients that will use it too (macOS and Linux).

| Client | Platform | Audio Layer | Integration |
|---|---|---|---|
| Panaudia Unreal plugin | macOS/Win | Unreal AudioCapture | Direct C++ linking |
| Panaudia macOS app | macOS | HAL driver (libASPL) + IPC | Swift via C API |
| Panaudia Linux client | Linux | PortAudio | Direct C++ linking |

## Dependencies

| Library | Version | Purpose |
|---|---|---|
| [msquic](https://github.com/microsoft/msquic) | `ed14762f5d55` (~v2.6.0) | QUIC transport (built as static library) |
| [libopus](https://opus-codec.org/) | v1.5.2 | Audio codec |
| [Catch2](https://github.com/catchorg/Catch2) | v3.7.1 | Test framework (dev only) |

All dependencies are fetched automatically via CMake FetchContent.

## Building

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

Build targets:

| Target | Description |
|---|---|
| `panaudia-core` | Static library (`libpanaudia-core.a`) |
| `panaudia-core-tests` | Unit tests (167 tests, 5211 assertions) |
| `panaudia-bench` | Buffer and codec benchmarks |
| `panaudia-test-harness` | Standalone RT callback simulation |

Install layout: `lib/libpanaudia-core.a` + `include/panaudia/*.h`.

For the Unreal Engine plugin, there is a convenience script:

```bash
cd panaudia-client/sdks/unreal/panaudia/Source/ThirdParty && ./build_panaudia_core.sh
```

## Public API

All public types are in the `panaudia` namespace, declared in headers under `include/panaudia/`.

### Configuration

```cpp
#include <panaudia/core.h>

panaudia::SessionConfig config;
config.server_url = "server.example.com:4433";
config.jwt = "<ed25519-signed-jwt>";

// Add an outbound mono audio track
panaudia::TrackConfig audio_out;
audio_out.name = "mic";
audio_out.moq_namespace = {"out", "audio", "opus-mono", "node-abc"};
audio_out.direction = panaudia::TrackDirection::Outbound;
audio_out.type = panaudia::TrackType::Audio;
audio_out.channels = 1;
audio_out.codec = panaudia::AudioCodec::Opus;
audio_out.opus_bitrate = 64000;
audio_out.opus_frame_size_ms = 5;
config.tracks.push_back(audio_out);

// Add an inbound audio track
panaudia::TrackConfig audio_in;
audio_in.name = "speaker";
audio_in.moq_namespace = {"in", "audio", "opus-mono", "node-abc"};
audio_in.direction = panaudia::TrackDirection::Inbound;
audio_in.type = panaudia::TrackType::Audio;
audio_in.channels = 1;
audio_in.codec = panaudia::AudioCodec::Opus;
config.tracks.push_back(audio_in);

// Add a data track for position updates
panaudia::TrackConfig state_out;
state_out.name = "state";
state_out.moq_namespace = {"out", "state", "node-abc"};
state_out.direction = panaudia::TrackDirection::Outbound;
state_out.type = panaudia::TrackType::Data;
config.tracks.push_back(state_out);

// Callbacks
config.data_recv_callback = my_data_handler;
config.status_callback = my_status_handler;
config.log_callback = my_log_handler;
config.log_level = panaudia::LogLevel::Info;
```

### Lifecycle

```cpp
panaudia::PanaudiaCore core;
core.configure(config);   // Allocates buffers, codecs, track handles
core.connect();           // Spawns session thread, starts QUIC + MOQ handshake

// ... use the session ...

core.disconnect();        // Graceful shutdown, joins session thread
```

### Track Handles

After `configure()`, look up tracks by name to get stable opaque pointers:

```cpp
auto* mic     = core.get_track("mic");      // returns TrackHandle*
auto* speaker = core.get_track("speaker");
auto* state   = core.get_track("state");
// Returns nullptr if name not found. Handles valid until next configure() or destruction.
```

### Real-Time Audio (RT-Safe)

These two functions are the only ones guaranteed lock-free -- safe to call from audio render/capture threads:

```cpp
// Write captured audio into outbound ring buffer (interleaved float PCM)
core.write_audio(mic, samples, frame_count, host_time);

// Read decoded audio from inbound jitter buffer
// Returns frames actually read; caller fills remainder with silence
uint32_t got = core.read_audio(speaker, buffer, frame_count, host_time);
```

### Data Tracks

```cpp
// Send opaque bytes (allocating, not RT-safe)
core.send_data(state, position_bytes, position_len);

// Receive via callback (set in SessionConfig::data_recv_callback)
void my_data_handler(panaudia::TrackHandle* track,
                     const uint8_t* data, uint32_t len, void* ctx) {
    // Called from session/msquic thread. Don't block. Payload valid for callback duration only.
}
```

### Status and Monitoring

```cpp
panaudia::ConnectionState state = core.get_connection_state();  // thread-safe atomic read
panaudia::BufferStatus buf = core.get_buffer_status(speaker);   // fill ratio, underruns, overruns
panaudia::SessionStats stats = core.get_stats();                // bytes/packets/RTT/reconnects
```

### JWT Refresh

```cpp
core.update_jwt(new_jwt);  // Takes effect on next reconnect
```

### Enums

```cpp
enum class TrackType       { Audio, Data };
enum class TrackDirection  { Outbound, Inbound };
enum class AudioCodec      { Opus, PCM };
enum class ConnectionState { Disconnected, Connecting, Connected, Reconnecting, Failed };
enum class LogLevel        { Trace, Debug, Info, Warn, Error };
```

### Callbacks

```cpp
using DataRecvCallback = void(*)(TrackHandle* track, const uint8_t* data, uint32_t data_len, void* ctx);
using StatusCallback   = void(*)(ConnectionState state, const char* message, void* ctx);
using LogCallback      = void(*)(LogLevel level, const char* message, void* ctx);
```

### Reconnection

Automatic reconnection is enabled by default. On unexpected disconnection, the library retries with exponential backoff (configurable via `SessionConfig`). Calling `disconnect()` explicitly suppresses reconnection. On reconnect, all MOQ state is reset and outbound ring buffers are flushed.

```cpp
config.max_reconnect_attempts = 10;     // 0 disables reconnection
config.reconnect_base_delay_ms = 100;
config.reconnect_max_delay_ms = 10000;
```

### Jitter Buffer

Inbound audio tracks use an adaptive jitter buffer with configurable latency window:

```cpp
config.jitter_buffer_min_ms = 10;       // Below this triggers underrun → refill
config.jitter_buffer_max_ms = 200;      // Above this snaps read position forward
config.jitter_buffer_initial_ms = 60;   // Target latency centre
```

The buffer has two states: **Filling** (outputs silence until target reached) and **Playing** (outputs audio with periodic 1-sample drift corrections).

