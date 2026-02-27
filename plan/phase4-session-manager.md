# Phase 4: Session Manager & PanaudiaCore

**Goal:** Wire all components together into the public `PanaudiaCore` API. A complete, functional core library that can stream multitrack audio and data to/from the Panaudia server.

**Depends on:** Phase 1 (buffers), Phase 2 (codec), Phase 3 (transport)
**Blocks:** Phase 5 (integration testing), Phase 6 (platform integration)

## Tasks

### TrackHandle Implementation
- [ ] Define `TrackHandle` as an opaque struct containing:
  - `TrackConfig` (copy of the config from SessionConfig)
  - `TrackType` / `TrackDirection` (for dispatch)
  - MOQ track alias (assigned by server via SUBSCRIBE_OK, or by us for published tracks)
  - For outbound audio: pointer to `RingBuffer`
  - For inbound audio: pointer to `JitterBuffer`
  - For data tracks: no buffer (direct send/callback)
- [ ] Name → handle lookup: `std::unordered_map<std::string, TrackHandle>`
- [ ] TrackAlias → handle lookup: `std::unordered_map<uint64_t, TrackHandle*>` (for incoming datagram dispatch)

### Session Manager
- [ ] `session_manager.h` / `session_manager.cpp`
- [ ] `configure(const SessionConfig& config)`:
  - [ ] Create TrackHandle for each track in config
  - [ ] For outbound audio tracks: create `RingBuffer`
  - [ ] For inbound audio tracks: create `JitterBuffer` (configured from session jitter settings)
  - [ ] For data tracks: no buffer creation
  - [ ] Build name→handle map
  - [ ] Store callbacks (DataRecvCallback, StatusCallback, LogCallback)
- [ ] `connect()`:
  - [ ] Call `MoqTransport::connect(url, jwt)`
  - [ ] For each outbound track: ANNOUNCE namespace, then wait for server SUBSCRIBE
  - [ ] For each inbound track: SUBSCRIBE to namespace
  - [ ] Build TrackAlias→handle map as SUBSCRIBE_OKs arrive
  - [ ] Start send/recv worker threads
  - [ ] Report `ConnectionState::Connected` via StatusCallback
- [ ] `disconnect()`:
  - [ ] Stop send/recv worker threads
  - [ ] Call `MoqTransport::disconnect()`
  - [ ] Report `ConnectionState::Disconnected` via StatusCallback
- [ ] `get_track(name)`: lookup in name→handle map, return pointer (or nullptr)
- [ ] `update_jwt(jwt)`: store for next reconnect, pass to transport if it supports hot update

### Send Workers (Outbound Audio)
- [ ] One thread per outbound audio track
- [ ] `send_worker.h` / `send_worker.cpp`
- [ ] Loop:
  - [ ] Read from track's `RingBuffer` (blocks briefly or spins if empty — needs design)
  - [ ] If Opus: encode frame → build datagram → `MoqTransport::send_datagram()`
  - [ ] If PCM: frame raw samples → build datagram → `MoqTransport::send_datagram()`
  - [ ] Frame size determines read chunk size (e.g., 5ms × 48kHz × channels)
- [ ] Pre-allocated encode buffer + datagram buffer (no per-frame allocation)
- [ ] Thread exit: signalled by disconnect, clean shutdown
- [ ] On reconnect: flush ring buffer (discard stale audio), restart thread

### Recv Workers (Inbound Audio)
- [ ] One thread per inbound audio track (or: datagrams dispatched by alias from a single recv callback)
- [ ] `recv_worker.h` / `recv_worker.cpp`
- [ ] Actually — msquic delivers datagrams via callback. The pattern is:
  - [ ] msquic callback receives datagram → parse header → lookup TrackAlias → dispatch
  - [ ] For audio tracks: decode (Opus or PCM unframe) → write to track's `JitterBuffer`
  - [ ] For data tracks: invoke `DataRecvCallback` with raw payload
- [ ] Decide: decode on the msquic callback thread, or queue to a separate decode thread?
  - Option A: Decode on msquic callback — simpler, but blocks msquic if decode is slow
  - Option B: Queue raw packets, decode on separate thread — adds latency but isolates msquic
  - Recommendation: Option A for V1, Opus decode is fast (~0.5ms for a 5ms frame)
- [ ] Pre-allocated decode buffer (no per-frame allocation)

### Data Track Pass-Through
- [ ] Outbound data: `send_data(handle, bytes, len)`:
  - [ ] Build MOQ datagram with track's alias
  - [ ] `MoqTransport::send_datagram()`
  - [ ] Called from host control thread — not RT, can allocate if needed
- [ ] Inbound data: when datagram arrives for a data track handle:
  - [ ] Invoke `DataRecvCallback(handle, payload, len, ctx)`
  - [ ] Called from msquic callback thread — host must not block

### Auto-Reconnection
- [ ] On connection drop (detected via msquic callback or transport state change):
  - [ ] Report `ConnectionState::Reconnecting` via StatusCallback
  - [ ] Stop send workers
  - [ ] Exponential backoff: 100ms, 200ms, 400ms, ..., cap at 10s
  - [ ] On successful reconnect:
    - [ ] Re-announce, re-subscribe all tracks from original SessionConfig
    - [ ] Rebuild TrackAlias→handle map
    - [ ] Flush outbound ring buffers (stale audio)
    - [ ] Inbound jitter buffers are already in FILLING — they re-accumulate naturally
    - [ ] Restart send workers
    - [ ] Report `ConnectionState::Connected` via StatusCallback
  - [ ] On max retries or fatal error: report `ConnectionState::Failed`

### Status & Stats
- [ ] `get_connection_state()` — atomic read of current state
- [ ] `get_buffer_status(handle)` — query the track's ring/jitter buffer stats
- [ ] `get_stats()` — aggregate: bytes sent/received, packets, loss, RTT (from msquic)

### LogCallback Routing
- [ ] Internal log macro/function that routes to `LogCallback` if set
- [ ] Log level filtering (only call callback if level ≥ configured minimum)
- [ ] Called from any thread — callback must be thread-safe (host's responsibility)
- [ ] Key log points: connect/disconnect, subscribe/announce, datagram errors, buffer state changes

### PanaudiaCore Facade
- [ ] `panaudia_core.cpp` — thin wrapper delegating to SessionManager
- [ ] Thread safety: control methods (configure, connect, disconnect, send_data) must not be called concurrently — document as host's responsibility
- [ ] RT methods (write_audio, read_audio) are lock-free by design

## Done When
- `PanaudiaCore` API is fully functional
- Can configure a session with multiple audio + data tracks
- Can connect to Panaudia Go server, stream audio bidirectionally, exchange data
- Auto-reconnect works on connection drop
- StatusCallback reports state transitions
- DataRecvCallback delivers inbound data track payloads
- No per-frame memory allocation in the audio path
