# MOQ Protocol Compatibility

libpanaudia-core implements the MOQ (Media over QUIC) protocol for real-time media transport. This document describes the specific protocol version we target and how it relates to the server-side Go library.

## Target: Eyevinn/moqtransport Go Library (draft-16)

The Panaudia server uses the [Eyevinn/moqtransport](https://github.com/Eyevinn/moqtransport)
Go library — a fork of mengelbart/moqtransport carried forward to draft-16.
libpanaudia-core's MOQ wire format is byte-compatible with what that library
emits on the wire (verified by golden-vector tests; see below).

| Property | Value |
|---|---|
| Go module | `github.com/Eyevinn/moqtransport` |
| MOQ version number | `0xff000010` (0xff000000 + 0x10 = draft-16) |
| ALPN | `moqt-16` |

> **History:** earlier versions targeted `mengelbart/moqtransport` (draft-11,
> `0xff00000b`, ALPN `moq-00`) plus a `paulharter` fork for a SUBSCRIBE_OK
> TrackAlias fix. The stack migrated to Eyevinn/draft-16 in 2026; the fork and
> the in-band version negotiation are gone.

## Draft Status

The MOQ specification is still in draft. "draft-16" is not a single wire format
in practice — the IETF spec text, the `moqtail` toolkit, and Eyevinn each
implement *different partial* draft-16s. **The authority for this library is what
the Eyevinn server actually puts on the wire**, not the abstract spec. Eyevinn
implements the draft-16 delta-encoded parameters, ALPN-based version negotiation,
and the SUBSCRIBE/SUBSCRIBE_OK parameter restructuring, while keeping the
draft-14-style message-type set (`ANNOUNCE`/`SUBSCRIBE_ANNOUNCES`, per-message
OK/Error) and the older fixed-type OBJECT_DATAGRAM.

**Golden vectors.** `tests/test_moq_protocol.cpp` `[moq_golden]` asserts the
encoder produces bytes identical to the shared fixtures in
`spatial-mixer/plan/moq-draft14/golden/draft16-vectors.json` — the same fixtures
the Go server and TypeScript client are validated against, so all three speak the
same wire.

## Wire Format Summary

### Control Message Framing

All control messages on the bidirectional control stream use:

```
[Type varint][Length 2-byte big-endian][Content]
```

### SUBSCRIBE (No TrackAlias)

SUBSCRIBE does **not** carry a TrackAlias — the publisher assigns one and returns
it in SUBSCRIBE_OK. In draft-16 the priority / group order / forward / filter
fields are no longer inline; they are **parameters** (see KVP below), merged with
the auth token and any host-supplied params, then sorted ascending and
delta-encoded.

```
[RequestID varint]
[Namespace Tuple]
[TrackName: Len varint + UTF-8 bytes]
[Params KVP]   # SubscriberPriority(0x20), GroupOrder(0x22), Forward(0x10),
               # SubscriptionFilter(0x21), AuthorizationToken(0x03), ...
```

### SUBSCRIBE_OK (Has TrackAlias)

In draft-16 Expires / GroupOrder / Largest Object are also parameters; a trailing
Track Extensions block may follow (the server emits none today).

```
[RequestID varint]
[TrackAlias varint]
[Params KVP]   # Expires(0x08), LargestObject(0x09 → "content exists"),
               # GroupOrder(0x22)
[Track Extensions]   # currently empty
```

### TrackAlias Flow

1. Subscriber sends SUBSCRIBE (no alias)
2. Publisher responds with SUBSCRIBE_OK containing a publisher-assigned TrackAlias
3. Publisher tags outbound datagrams with that TrackAlias
4. Subscriber dispatches inbound datagrams by TrackAlias

### Object Datagram

Sent as QUIC datagrams (unreliable, unordered):

```
[Type varint][TrackAlias varint][GroupID varint][ObjectID varint][Priority 1 byte][Payload...]
```

**Priority is a single byte, not a varint.**

Datagram type values: `0x00` = plain, `0x01` = with extensions, `0x02` = status, `0x03` = status with extensions.

> Eyevinn retains this fixed-type datagram format from draft-13/14 (it did **not**
> adopt the draft-16 bit-field datagram type). So the datagram path is
> **wire-identical to the previous draft-11 codec** — no change was needed here.

### KVP Parameters (delta-encoded)

In draft-16 a parameter list is `[count]` followed by pairs whose **type is
delta-encoded**: write the list sorted ascending by key, and each pair stores
`key − previousKey` (starting from 0) as its type. The value encoding still
follows the parity of the absolute key:

- **Even keys**: bare varint value
- **Odd keys**: length-prefixed bytes

| Key | Name | Parity | Used in |
|-----|------|--------|---------|
| 0x01 | Path | odd | CLIENT_SETUP (raw QUIC only) |
| 0x02 | MaxRequestId | even | CLIENT_SETUP |
| 0x03 | AuthorizationToken | odd | SUBSCRIBE (raw JWT bytes) |
| 0x08 | Expires | even | SUBSCRIBE_OK |
| 0x09 | LargestObject | odd | SUBSCRIBE_OK (Location: group, object) |
| 0x10 | Forward | even | SUBSCRIBE |
| 0x20 | SubscriberPriority | even | SUBSCRIBE |
| 0x21 | SubscriptionFilter | odd | SUBSCRIBE ([filterType][start?][endGroup?]) |
| 0x22 | GroupOrder | even | SUBSCRIBE / SUBSCRIBE_OK |
| 0xFF01 | ResumeOpId (Panaudia) | odd | SUBSCRIBE — opaque to the core; supplied by the host via `subscribe_params_callback` (defined in `panaudia-statecache`) |

The draft-11 `Role` (0x00) parameter was removed in draft-16.

### ANNOUNCE / ANNOUNCE_OK

ANNOUNCE carries `[RequestID varint][Namespace Tuple][Params KVP]`.

ANNOUNCE_OK is just `[RequestID varint]` -- no namespace tuple.

### Request ID Allocation

Client and server use separate ID sequences (even/odd) to avoid collisions. libpanaudia-core uses even IDs (0, 2, 4, ...).

## JWT Authentication

The JWT token is attached only to the **first** outbound SUBSCRIBE as a KVP parameter with key `0x03` (AuthorizationToken). The server authenticates the connection from this single token, then marks the session as authenticated for all subsequent messages.

In Panaudia's own server auth the JWT itself is Ed25519-signed. libpanaudia-core treats it as an opaque string -- no parsing or verification happens in the library.

## CLIENT_SETUP

In draft-16 the version is negotiated by the ALPN (`moqt-16`), so CLIENT_SETUP
carries **no** version list and **no** Role parameter — just a delta-encoded
parameter list. libpanaudia-core sends:

```
Parameters (delta-encoded):
  Path = "/"            # key 0x01
  MaxRequestId = 100    # key 0x02
```

The Path parameter is required for raw QUIC connections (the server validates it).
For WebTransport connections the path comes from the HTTP URL instead.

SERVER_SETUP likewise has no version field in draft-16 — just a parameter list.

## Session Orchestration Sequence

1. QUIC connection established (msquic)
2. Client opens bidirectional control stream, sends CLIENT_SETUP
3. Server responds with SERVER_SETUP
4. Client sends SUBSCRIBE for each inbound track (JWT on first only)
5. Client sends ANNOUNCE for each outbound track namespace
6. Server replies with SUBSCRIBE_OK (containing publisher-assigned TrackAlias) for each subscription
7. Server replies with ANNOUNCE_OK for each announcement
8. Server sends its own SUBSCRIBE for each announced outbound track
9. Client replies with SUBSCRIBE_OK (assigning TrackAlias starting at 1)
10. Bidirectional datagram flow begins

## Further Reference

- [Wire format byte-level reference](../plan/moq_protocol_wire_format.md) -- byte-level encoding examples (note: predates draft-16; see the draft-11→16 delta below)
- [draft-11 → draft-16 wire delta](../../spatial-mixer/plan/moq-draft14/wire-delta-16.md) -- the authoritative per-message delta, grounded in Eyevinn's source
- [Eyevinn/moqtransport Go library](https://github.com/Eyevinn/moqtransport)
