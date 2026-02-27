# Phase 3: MOQ/QUIC Transport

**Goal:** A standalone MOQ transport layer over msquic that can connect to the Panaudia Go server, complete the MOQ handshake, subscribe to tracks, publish datagrams, and receive datagrams. No UE dependencies.

**Depends on:** Phase 0 (project compiles, msquic available via FetchContent)
**Blocks:** Phase 4 (session manager uses transport for track subscribe/publish)

**Source:** Extract from UE plugin `PanaudiaConnectionManager.cpp`

**Reference:** CLAUDE.md moqtransport compatibility notes (wire format, message types, TrackAlias flow)

## Tasks

### Examine UE Plugin Code
- [ ] Read `PanaudiaConnectionManager.cpp` thoroughly — map out:
  - [ ] msquic initialisation (MsQuicOpen2, configuration, credentials)
  - [ ] QUIC connection lifecycle (ConnectionOpen, ConnectionStart, callbacks)
  - [ ] Control stream setup (bidirectional stream for MOQ messages)
  - [ ] MOQ handshake: CLIENT_SETUP / SERVER_SETUP exchange
  - [ ] MOQ ANNOUNCE / ANNOUNCE_OK
  - [ ] MOQ SUBSCRIBE / SUBSCRIBE_OK (note: TrackAlias is in SUBSCRIBE_OK, not SUBSCRIBE)
  - [ ] MOQ SUBSCRIBE_ANNOUNCES / SUBSCRIBE_ANNOUNCES_OK
  - [ ] Datagram send path (`SendDatagramDirect`)
  - [ ] Datagram receive path (QUIC_CONNECTION_EVENT_DATAGRAM_RECEIVED)
  - [ ] Object Datagram format: [Type][TrackAlias][GroupID][ObjectID][Priority][Payload]
  - [ ] JWT passed as AuthorizationToken parameter (KVP key 0x03)
- [ ] Identify all UE-specific code to replace

### msquic Integration
- [ ] `moq_transport.h` / `moq_transport.cpp`
- [ ] msquic lifecycle:
  - [ ] `MsQuicOpen2` / `MsQuicClose` — library init/shutdown
  - [ ] Registration, configuration with TLS (QUIC_CREDENTIAL_FLAG_NO_CERTIFICATE_VALIDATION for dev, proper TLS for production)
  - [ ] ALPN: `"moq-00"` (matching Go server)
- [ ] QUIC connection:
  - [ ] `ConnectionOpen` with callback handler
  - [ ] `ConnectionStart` to server URL
  - [ ] Handle connection events: CONNECTED, SHUTDOWN_INITIATED_BY_TRANSPORT, SHUTDOWN_INITIATED_BY_PEER, SHUTDOWN_COMPLETE, DATAGRAM_RECEIVED, DATAGRAM_SEND_STATE_CHANGED
- [ ] Bidirectional control stream:
  - [ ] Open on connection established
  - [ ] Handle STREAM_EVENT_RECEIVE for incoming MOQ messages
  - [ ] Handle STREAM_EVENT_SEND_COMPLETE

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
- [ ] Request ID management: client uses odd IDs, server uses even (or vice versa — check Go server)

### Datagram Handling
- [x] Object Datagram format:
  - [x] Send: `[Type varint][TrackAlias varint][GroupID varint][ObjectID varint][Priority 1 byte][Payload]`
  - [x] Receive: parse same format, dispatch by TrackAlias
  - [x] Type field: 0x00 = datagram (no extensions), 0x01 = with extensions (parsed by skipping)
  - [x] Priority is a SINGLE BYTE, not varint
  - [x] Zero-copy header builder for pre-allocated send path
- [ ] TrackAlias dispatch map: `std::unordered_map<uint64_t, TrackHandle*>`
  - [ ] Populated when SUBSCRIBE_OK arrives with TrackAlias
  - [ ] Used to route incoming datagrams to the correct track
- [ ] Send path:
  - [ ] Pre-allocate datagram buffer (avoid malloc per send — UE plugin does malloc, we should improve)
  - [ ] `send_datagram(TrackHandle* track, const uint8_t* payload, uint32_t len)`
- [ ] Receive path:
  - [ ] Parse datagram header, lookup TrackAlias, deliver payload to track handler

### JWT Authentication
- [x] Pass JWT as MOQ AuthorizationToken parameter (KVP key 0x03) in SUBSCRIBE
- [ ] `update_jwt()` — store new token for next connection/reconnection
- [ ] No JWT parsing or validation — just store and send the string

### Transport API (Internal)
- [ ] `MoqTransport` class:
  - [ ] `connect(const std::string& url, const std::string& jwt)`
  - [ ] `disconnect()`
  - [ ] `announce(const std::vector<std::string>& namespace_tuple)`
  - [ ] `subscribe(const std::vector<std::string>& namespace_tuple, const std::string& track_name)` → returns when SUBSCRIBE_OK received (with TrackAlias)
  - [ ] `send_datagram(uint64_t track_alias, const uint8_t* payload, uint32_t len)`
  - [ ] Callback: `on_datagram(uint64_t track_alias, const uint8_t* payload, uint32_t len)`
  - [ ] Callback: `on_connection_state_changed(ConnectionState state)`
  - [ ] Callback: `on_subscribe_request(...)` — for server-initiated subscribes to our published tracks

### Tests
- [x] Varint encode/decode — edge cases (0, 1, 63, 64, 16383, 16384, etc.)
- [x] Message serialisation/deserialisation — roundtrip for each message type
- [x] Datagram format — build and parse, verify fields
- [x] Byte-for-byte compat checks against UE plugin output
- [ ] **Integration test against Go server:**
  - [ ] Connect with JWT
  - [ ] Complete MOQ handshake
  - [ ] ANNOUNCE a namespace
  - [ ] SUBSCRIBE to a server track
  - [ ] Send a datagram, verify server receives
  - [ ] Receive a datagram from server
- [ ] Connection drop — verify callback fires, cleanup occurs
- [ ] Invalid server response — verify graceful error handling

## Done When
- Can connect to the Panaudia Go server, complete handshake, and exchange datagrams
- No UE dependencies
- All unit tests pass
- Integration test against Go server passes
- No per-datagram memory allocation in the send path (pre-allocated buffers)
