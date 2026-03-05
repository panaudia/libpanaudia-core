# MOQ Protocol Compatibility

libpanaudia-core implements the MOQ (Media over QUIC) protocol for real-time media transport. This document describes the specific protocol version we target and how it relates to the server-side Go library.

## Target: moqtransport Go Library

The Panaudia server uses the [moqtransport](https://github.com/mengelbart/moqtransport) Go library by Mathis Engelbart. libpanaudia-core's MOQ wire format is pinned to be byte-compatible with a specific commit of this library.

| Property | Value |
|---|---|
| Go module | `github.com/mengelbart/moqtransport` |
| Pinned commit | `3b0932de5aeb` |
| Pseudo-version | `v0.5.1-0.20251006143843-3b0932de5aeb` |
| MOQ version number | `0xff00000b` (0xff000000 + 11) |
| ALPN | `moq-00` |

The server actually uses a fork (`github.com/paulharter/moqtransport`, branch `fix/subscribe-ok-track-alias`) which fixes a bug where `SUBSCRIBE_OK` always sent TrackAlias=0. Upstream PR: [#266](https://github.com/mengelbart/moqtransport/pull/266). Once merged, the fork can be dropped.

## Draft Status

The MOQ specification is a moving target as its still in draft (currently 17 I think). 
We aim to track the moqtransport library as closely as possible at the moment. It is a little behind the latest drafts, 
but offers a good stable implementation of MOQ.

This commit sits between MOQ draft versions. It has partial draft-13 wire format updates (TrackAlias moved from SUBSCRIBE to SUBSCRIBE_OK) but uses a draft-11 era version number. It is not fully compliant with any single published draft.

## Wire Format Summary

### Control Message Framing

All control messages on the bidirectional control stream use:

```
[Type varint][Length 2-byte big-endian][Content]
```

### SUBSCRIBE (No TrackAlias)

Unlike some MOQ drafts, SUBSCRIBE does **not** carry a TrackAlias. The publisher assigns one and returns it in SUBSCRIBE_OK.

```
[RequestID varint]
[Namespace Tuple]
[TrackName: Len varint + UTF-8 bytes]
[SubscriberPriority 1 byte]
[GroupOrder 1 byte]
[Forward 1 byte]
[FilterType varint]
[Params KVP]
```

### SUBSCRIBE_OK (Has TrackAlias)

```
[RequestID varint]
[TrackAlias varint]
[Expires varint (ms, 0=never)]
[GroupOrder 1 byte]
[ContentExists 1 byte]
[LargestGroupID varint]      # only if ContentExists=1
[LargestObjectID varint]     # only if ContentExists=1
[Params KVP]
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

### KVP Parameters

Key-value pairs use a parity rule for value encoding:

- **Even keys** (0x00, 0x02, 0x04): bare varint value
- **Odd keys** (0x01, 0x03): length-prefixed bytes

| Key | Name | Used in |
|-----|------|---------|
| 0x00 | Role | CLIENT_SETUP |
| 0x01 | Path | CLIENT_SETUP |
| 0x02 | MaxSubscribeId | CLIENT_SETUP |
| 0x03 | AuthorizationToken | SUBSCRIBE |

### ANNOUNCE / ANNOUNCE_OK

ANNOUNCE carries `[RequestID varint][Namespace Tuple][Params KVP]`.

ANNOUNCE_OK is just `[RequestID varint]` -- no namespace tuple.

### Request ID Allocation

Client and server use separate ID sequences (even/odd) to avoid collisions. libpanaudia-core uses even IDs (0, 2, 4, ...).

## JWT Authentication

The JWT token is attached only to the **first** outbound SUBSCRIBE as a KVP parameter with key `0x03` (AuthorizationToken). The server authenticates the connection from this single token, then marks the session as authenticated for all subsequent messages.

In Panaudia's own server auth the JWT itself is Ed25519-signed. libpanaudia-core treats it as an opaque string -- no parsing or verification happens in the library.

## CLIENT_SETUP

libpanaudia-core sends:

```
Version count: 1
Version: 0xff00000b
Parameters:
  Role = PubSub (0x03)
  Path = "/"
  MaxSubscribeId = 100
```

The Path parameter is required for raw QUIC connections (moqtransport validates it). For WebTransport connections, the path comes from the HTTP URL instead.

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

- [Wire format byte-level reference](../plan/moq_protocol_wire_format.md) -- includes byte-level encoding examples
- [moqtransport Go library](https://github.com/mengelbart/moqtransport)
- [Panaudia fork with TrackAlias fix](https://github.com/paulharter/moqtransport/tree/fix/subscribe-ok-track-alias)
