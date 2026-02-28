# Phase 3: MOQ/QUIC Transport

**Goal:** A standalone MOQ transport layer over msquic that can connect to the Panaudia Go server, complete the MOQ handshake, subscribe to tracks, publish datagrams, and receive datagrams. No UE dependencies.

**Depends on:** Phase 0 (project compiles, msquic available via FetchContent)
**Blocks:** Phase 4 (session manager uses transport for track subscribe/publish)

**Source:** Extract from UE plugin `PanaudiaConnectionManager.cpp`

**Reference:** CLAUDE.md moqtransport compatibility notes (wire format, message types, TrackAlias flow)

## Tasks

### Examine UE Plugin Code
- [x] Read `PanaudiaConnectionManager.cpp` thoroughly — map out:
  - [x] msquic initialisation (MsQuicOpen2, configuration, credentials)
  - [x] QUIC connection lifecycle (ConnectionOpen, ConnectionStart, callbacks)
  - [x] Control stream setup (bidirectional stream for MOQ messages)
  - [x] MOQ handshake: CLIENT_SETUP / SERVER_SETUP exchange
  - [x] MOQ ANNOUNCE / ANNOUNCE_OK
  - [x] MOQ SUBSCRIBE / SUBSCRIBE_OK (note: TrackAlias is in SUBSCRIBE_OK, not SUBSCRIBE)
  - [x] MOQ SUBSCRIBE_ANNOUNCES / SUBSCRIBE_ANNOUNCES_OK
  - [x] Datagram send path (`SendDatagramDirect`)
  - [x] Datagram receive path (QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED)
  - [x] Object Datagram format: [Type][TrackAlias][GroupID][ObjectID][Priority][Payload]
  - [x] JWT passed as AuthorizationToken parameter (KVP key 0x03)
- [x] Identify all UE-specific code to replace

### msquic Integration
- [x] `moq_transport.h` / `moq_transport.cpp`
- [x] msquic lifecycle:
  - [x] `MsQuicOpen2` / `MsQuicClose` — library init/shutdown
  - [x] Registration, configuration with TLS (QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION for dev, proper TLS for production)
  - [x] ALPN: `"moq-00"` (matching Go server)
- [x] QUIC connection:
  - [x] `ConnectionOpen` with callback handler
  - [x] `ConnectionStart` to server URL
  - [x] Handle connection events: CONNECTED, SHUTDOWN_INITIATED_BY_TRANSPORT, SHUTDOWN_INITIATED_BY_PEER, SHUTDOWN_COMPLETE, DATAGRAM_RECEIVED, DATAGRAM_SEND_STATE_CHANGED
- [x] Bidirectional control stream:
  - [x] Open on connection established
  - [x] Handle STREAM_EVENT_RECEIVE for incoming MOQ messages
  - [x] Handle STREAM_EVENT_SEND_COMPLETE

### MOQ Protocol Messages
- [x] Message framing: `[Type varint][Length 2-bytes big-endian][Content]`
- [x] Varint encoding/decoding (QUIC variable-length integer format)
- [x] KVP parameter encoding/decoding
- [x] Namespace tuple encoding/decoding
- [x] Message types to implement:
  - [x] CLIENT_SETUP (0x20) — send
  - [x] SERVER_SETUP (0x21) — receive
  - [x] ANNOUNCE (0x06) — send and receive (with RequestID)
  - [x] ANNOUNCE_OK (0x07) — send and receive (just RequestID, not namespace)
  - [x] SUBSCRIBE (0x03) — send and receive (NO TrackAlias — differs from some drafts)
  - [x] SUBSCRIBE_OK (0x04) — send and receive (HAS TrackAlias — assigned by publisher)
  - [x] SUBSCRIBE_ERROR (0x05) — receive
  - [x] SUBSCRIBE_ANNOUNCES (0x11) — receive from server
  - [x] SUBSCRIBE_ANNOUNCES_OK (0x12) — send
- [ ] Request ID management: client uses odd IDs, server uses even (or vice versa — check Go server) — **Phase 4**

### Datagram Handling
- [x] Object Datagram format:
  - [x] Send: `[Type varint][TrackAlias varint][GroupID varint][ObjectID varint][Priority 1 byte][Payload]`
  - [x] Receive: parse same format, dispatch by TrackAlias
  - [x] Type field: 0x00 = datagram (no extensions), 0x01 = with extensions (parsed by skipping)
  - [x] Priority is a SINGLE BYTE, not varint
  - [x] Zero-copy header builder for pre-allocated send path
- [ ] TrackAlias dispatch map: `std::unordered_map<uint64_t, TrackHandle*>` — **Phase 4**
  - [ ] Populated when SUBSCRIBE_OK arrives with TrackAlias
  - [ ] Used to route incoming datagrams to the correct track
- [x] Send path:
  - [x] Single-block datagram allocation: `[QUIC_BUFFER][payload]` in one malloc, freed in DATAGRAM_SEND_STATE_CHANGED callback (same pattern as UE plugin)
  - [x] `send_datagram(const uint8_t* data, uint32_t len)` — pre-built datagram
- [x] Receive path:
  - [x] Parse datagram header inline on msquic thread (zero-copy), dispatch via on_datagram callback

### JWT Authentication
- [x] Pass JWT as MOQ AuthorizationToken parameter (KVP key 0x03) in SUBSCRIBE
- [ ] `update_jwt()` — store new token for next connection/reconnection — **Phase 4**
- [ ] No JWT parsing or validation — just store and send the string — **Phase 4**

### Transport API (Internal)
- [x] `MoqTransport` class (Pimpl pattern — hides msquic.h from consumers):
  - [x] `connect(const TransportConfig& config, const TransportCallbacks& callbacks)`
  - [x] `disconnect()`
  - [x] `send_control(const uint8_t* data, uint32_t len)` — pre-built framed message
  - [x] `send_datagram(const uint8_t* data, uint32_t len)` — pre-built datagram
  - [x] `process_incoming()` — drain queues, parse control messages, fire callbacks
  - [x] `state()` / `max_datagram_size()` — thread-safe status
  - [x] Callback: `on_datagram(track_alias, group_id, object_id, priority, payload, len)` — msquic thread
  - [x] Callback: `on_control_message(message_type, content, content_len)` — caller thread
  - [x] Callback: `on_state_changed(state, message)` — any thread
  - [ ] `announce()` / `subscribe()` orchestration — **Phase 4 SessionManager drives via send_control()**

### Tests
- [x] Varint encode/decode — edge cases (0, 1, 63, 64, 16383, 16384, etc.)
- [x] Message serialisation/deserialisation — roundtrip for each message type
- [x] Datagram format — build and parse, verify fields
- [x] Byte-for-byte compat checks against UE plugin output
- [x] Create/destroy without connecting
- [x] Initial state is Disconnected
- [x] send_control/send_datagram return false when disconnected
- [x] Double disconnect is safe
- [x] process_incoming safe when disconnected
- [x] Connect to nonexistent host transitions to Failed
- [ ] **Integration test against Go server** (tagged `[integration]`):
  - [x] Connect, complete MOQ handshake, verify Ready state — *written, needs running server*
  - [ ] ANNOUNCE a namespace — **Phase 4**
  - [ ] SUBSCRIBE to a server track — **Phase 4**
  - [ ] Send a datagram, verify server receives — **Phase 4**
  - [ ] Receive a datagram from server — **Phase 4**
- [ ] Connection drop — verify callback fires, cleanup occurs
- [ ] Invalid server response — verify graceful error handling

## Done When
- [x] Can connect to the Panaudia Go server, complete handshake — transport layer done (Phase 3b)
- [ ] Exchange datagrams with track management — **Phase 4**
- [x] No UE dependencies
- [x] All unit tests pass (117 tests, 5047 assertions, macOS + Docker/Linux)
- [ ] Integration test against Go server passes — written, needs running server
- [x] Single-block datagram allocation (same proven pattern as UE plugin)
