# Phase 4: Session Manager & PanaudiaCore

**Goal:** Wire all components together into the public `PanaudiaCore` API. A complete, functional core library that can stream multitrack audio and data to/from the Panaudia server.

**Depends on:** Phase 1 (buffers), Phase 2 (codec), Phase 3 (transport)
**Blocks:** Phase 5 (integration testing), Phase 6 (platform integration)

## Tasks

### TrackHandle Implementation
- [x] Define `TrackHandle` as an opaque struct containing:
  - `TrackConfig` (copy of the config from SessionConfig)
  - `TrackType` / `TrackDirection` (for dispatch)
  - MOQ track alias (assigned by server via SUBSCRIBE_OK, or by us for published tracks)
  - For outbound audio: pointer to `RingBuffer`
  - For inbound audio: pointer to `JitterBuffer`
  - For data tracks: no buffer (direct send/callback)
- [x] Name → handle lookup: `std::unordered_map<std::string, TrackHandle>`
- [x] TrackAlias → handle lookup: `std::unordered_map<uint64_t, TrackHandle*>` (for incoming datagram dispatch)

### Session Manager
- [x] `session_manager.h` / `session_manager.cpp`
- [x] `configure(const SessionConfig& config)`:
  - [x] Create TrackHandle for each track in config
  - [x] For outbound audio tracks: create `RingBuffer`
  - [x] For inbound audio tracks: create `JitterBuffer` (configured from session jitter settings)
  - [x] For data tracks: no buffer creation
  - [x] Build name→handle map
  - [x] Store callbacks (DataRecvCallback, StatusCallback, LogCallback)
- [x] `connect()`:
  - [x] Call `MoqTransport::connect(url, jwt)`
  - [x] For each outbound track: ANNOUNCE namespace, then wait for server SUBSCRIBE
  - [x] For each inbound track: SUBSCRIBE to namespace
  - [x] Build TrackAlias→handle map as SUBSCRIBE_OKs arrive
  - [x] Start send/recv worker threads (session thread polls outbound; datagram callback handles inbound)
  - [x] Report `ConnectionState::Connected` via StatusCallback
- [x] `disconnect()`:
  - [x] Stop send/recv worker threads (session thread stopped on disconnect)
  - [x] Call `MoqTransport::disconnect()`
  - [x] Report `ConnectionState::Disconnected` via StatusCallback
- [x] `get_track(name)`: lookup in name→handle map, return pointer (or nullptr)
- [x] `update_jwt(jwt)`: store for next reconnect, pass to transport if it supports hot update

### Send Workers (Outbound Audio)
- [x] ~~One thread per outbound audio track~~ Session thread polls all outbound tracks (simpler for V1)
- [x] ~~`send_worker.h` / `send_worker.cpp`~~ Integrated into session_manager.cpp (poll_outbound_tracks)
- [x] Loop:
  - [x] Read from track's `RingBuffer` (poll every 5ms in session thread)
  - [x] If Opus: encode frame → build datagram → `MoqTransport::send_datagram()`
  - [x] If PCM: frame raw samples → build datagram → `MoqTransport::send_datagram()`
  - [x] Frame size determines read chunk size (e.g., 5ms × 48kHz × channels)
- [x] Pre-allocated encode buffer + datagram buffer (no per-frame allocation)
- [x] Thread exit: signalled by disconnect, clean shutdown
- [x] On reconnect: flush ring buffer (discard stale audio) — done in Phase 4d

### Recv Workers (Inbound Audio)
- [x] ~~One thread per inbound audio track~~ Datagrams dispatched by alias from msquic callback (Option A)
- [x] ~~`recv_worker.h` / `recv_worker.cpp`~~ Integrated into session_manager.cpp (dispatch_audio_datagram)
- [x] msquic delivers datagrams via callback:
  - [x] msquic callback receives datagram → parse header → lookup TrackAlias → dispatch
  - [x] For audio tracks: decode (Opus or PCM unframe) → write to track's `JitterBuffer`
  - [x] For data tracks: invoke `DataRecvCallback` with raw payload
- [x] Decision: Option A — decode on msquic callback thread (Opus decode is ~3.6µs for 5ms frame)
- [x] Pre-allocated decode buffer (no per-frame allocation)

### Data Track Pass-Through
- [x] Outbound data: `send_data(handle, bytes, len)`:
  - [x] Build MOQ datagram with track's alias
  - [x] `MoqTransport::send_datagram()`
  - [x] Called from host control thread — not RT, can allocate if needed
- [x] Inbound data: when datagram arrives for a data track handle:
  - [x] Invoke `DataRecvCallback(handle, payload, len, ctx)`
  - [x] Called from msquic callback thread — host must not block

### Auto-Reconnection
- [x] On connection drop (detected via msquic callback or transport state change):
  - [x] Report `ConnectionState::Reconnecting` via StatusCallback
  - [x] Stop send workers (session thread pauses outbound polling during reconnect)
  - [x] Exponential backoff: 100ms, 200ms, 400ms, ..., cap at 10s
  - [x] On successful reconnect:
    - [x] Re-announce, re-subscribe all tracks from original SessionConfig
    - [x] Rebuild TrackAlias→handle map
    - [x] Flush outbound ring buffers (discard stale audio)
    - [x] Inbound jitter buffers are already in FILLING — they re-accumulate naturally
    - [x] Restart send workers (session thread resumes outbound polling)
    - [x] Report `ConnectionState::Connected` via StatusCallback
  - [x] On max retries or fatal error: report `ConnectionState::Failed`

### Status & Stats
- [x] `get_connection_state()` — atomic read of current state
- [x] `get_buffer_status(handle)` — query the track's ring/jitter buffer stats
- [x] `get_stats()` — aggregate: bytes sent/received, packets, loss, RTT (from msquic)

### LogCallback Routing
- [x] Internal log macro/function that routes to `LogCallback` if set
- [x] Log level filtering (only call callback if level ≥ configured minimum)
- [x] Called from any thread — callback must be thread-safe (host's responsibility)
- [x] Key log points: connect/disconnect, subscribe/announce, datagram errors, buffer state changes

### PanaudiaCore Facade
- [x] `panaudia_core.cpp` — thin wrapper delegating to SessionManager
- [x] Thread safety: control methods (configure, connect, disconnect, send_data) must not be called concurrently — document as host's responsibility
- [x] RT methods (write_audio, read_audio) are lock-free by design

## Done When
- `PanaudiaCore` API is fully functional
- Can configure a session with multiple audio + data tracks
- Can connect to Panaudia Go server, stream audio bidirectionally, exchange data
- Auto-reconnect works on connection drop
- StatusCallback reports state transitions
- DataRecvCallback delivers inbound data track payloads
- No per-frame memory allocation in the audio path
