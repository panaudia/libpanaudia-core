# MOQ Protocol Wire Format — Pinned Reference

> ⚠️ **HISTORICAL (draft-11).** libpanaudia-core now targets **draft-16**
> (Eyevinn/moqtransport; version `0xff000010`, ALPN `moqt-16`). The byte-level
> examples below are the *old* draft-11 format and are kept only as historical
> reference. For the current wire format see
> [docs/moq-compatibility.md](../docs/moq-compatibility.md) and the
> draft-11→16 delta in
> `spatial-mixer/plan/moq-draft14/wire-delta-16.md`; the authoritative byte
> fixtures live in `tests/test_moq_protocol.cpp` `[moq_golden]`.

This document describes the MOQ wire format originally used by libpanaudia-core.
It was pinned to the `moqtransport` Go library at commit `3b0932de5aeb`
(pseudo-version `v0.5.1-0.20251006143843-3b0932de5aeb`).

**Version number:** `0xff00000b` (0xff000000 + 11, draft-11 era)
**ALPN:** `"moq-00"`

---

## Varint Encoding (RFC 9000 Section 16)

Variable-length integer encoding using the top 2 bits to indicate size:

| Prefix bits | Byte length | Value range       | Mask  |
|-------------|-------------|-------------------|-------|
| `00`        | 1           | 0 – 63            | —     |
| `01`        | 2           | 64 – 16383        | 0x40  |
| `10`        | 4           | 16384 – 2^30 - 1  | 0x80  |
| `11`        | 8           | 2^30 – 2^62 - 1   | 0xC0  |

Encoding: set the top 2 bits of byte 0 to the prefix, store value in remaining bits (big-endian).
Decoding: read prefix from `byte[0] >> 6`, mask off prefix bits (`& 0x3F`), read remaining bytes.

---

## Control Message Framing

All control messages (sent on the bidirectional control stream) use this framing:

```
[Type varint][Length 2-byte big-endian][Content]
```

- **Type**: message type as varint (e.g. 0x20 for CLIENT_SETUP)
- **Length**: content length as 2-byte big-endian uint16
- **Content**: message-specific payload

---

## KVP Parameters

Parameters are encoded as a counted list of key-value pairs:

```
[Count varint][Key1 varint][Value1 ...][Key2 varint][Value2 ...]...
```

Value encoding depends on key parity:
- **Even keys** (0x00, 0x02, 0x04): bare varint value
- **Odd keys** (0x01, 0x03): length-prefixed bytes `[Length varint][Bytes]`

### Known Parameter Keys

| Key  | Name              | Parity | Value format       | Used in        |
|------|-------------------|--------|--------------------|----------------|
| 0x00 | Role              | even   | varint             | CLIENT_SETUP   |
| 0x01 | Path              | odd    | length+bytes       | CLIENT_SETUP   |
| 0x02 | MaxSubscribeId    | even   | varint             | CLIENT_SETUP   |
| 0x03 | AuthorizationToken| odd    | length+bytes       | SUBSCRIBE      |
| 0x04 | MaxCacheDuration  | even   | varint             | —              |

### Role Values

| Value | Meaning |
|-------|---------|
| 0x01  | Publisher |
| 0x02  | Subscriber |
| 0x03  | PubSub (both) |

---

## Namespace Tuple

Namespace is a list of UTF-8 string parts:

```
[Count varint][Len1 varint][UTF-8 bytes1][Len2 varint][UTF-8 bytes2]...
```

Example: `["out", "audio", "opus-stereo", "abc123"]` → count=4 followed by 4 length-prefixed strings.

---

## Control Message Types

| Type | Hex  | Name                  |
|------|------|-----------------------|
| 0x03 | 0x03 | SUBSCRIBE             |
| 0x04 | 0x04 | SUBSCRIBE_OK          |
| 0x05 | 0x05 | SUBSCRIBE_ERROR       |
| 0x06 | 0x06 | ANNOUNCE              |
| 0x07 | 0x07 | ANNOUNCE_OK           |
| 0x08 | 0x08 | ANNOUNCE_ERROR        |
| 0x11 | 0x11 | SUBSCRIBE_ANNOUNCES   |
| 0x12 | 0x12 | SUBSCRIBE_ANNOUNCES_OK|
| 0x20 | 0x20 | CLIENT_SETUP          |
| 0x21 | 0x21 | SERVER_SETUP          |

---

## CLIENT_SETUP (0x20)

Sent by client after opening control stream.

```
[VersionCount varint=1][Version varint=0xff00000b]
[ParamCount varint=3]
  [Key=0x00 varint][Value=0x03 varint]            # Role = PubSub
  [Key=0x01 varint][Len=1 varint][0x2F]           # Path = "/"
  [Key=0x02 varint][Value=100 varint]              # MaxSubscribeId = 100
```

Path is REQUIRED for raw QUIC connections (moqtransport validates it).
For WebTransport the path comes from the HTTP URL.

---

## SERVER_SETUP (0x21)

Sent by server in response to CLIENT_SETUP.

```
[Version varint][ParamCount varint][Params KVP...]
```

---

## SUBSCRIBE (0x03)

**IMPORTANT: NO TrackAlias in SUBSCRIBE.** This differs from some other MOQ drafts.
The publisher assigns a TrackAlias and returns it in SUBSCRIBE_OK.

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

- **FilterType**: 0x01 = LatestGroup (most common)
- **Auth**: passed as KVP parameter with key 0x03 (AuthorizationToken)

---

## SUBSCRIBE_OK (0x04)

**HAS TrackAlias** — assigned by the publisher.

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
2. Publisher responds with SUBSCRIBE_OK containing TrackAlias
3. Publisher sends datagrams tagged with that TrackAlias
4. Subscriber dispatches datagrams by TrackAlias to the correct handler

---

## SUBSCRIBE_ERROR (0x05)

```
[RequestID varint]
[ErrorCode varint]
[ReasonPhrase: Len varint + UTF-8 bytes]
[TrackAlias varint]
```

---

## ANNOUNCE (0x06)

```
[RequestID varint]
[Namespace Tuple]
[Params KVP]
```

---

## ANNOUNCE_OK (0x07)

```
[RequestID varint]
```

Just the request ID, no namespace tuple.

---

## ANNOUNCE_ERROR (0x08)

```
[RequestID varint]
[ErrorCode varint]
[ReasonPhrase: Len varint + UTF-8 bytes]
```

---

## SUBSCRIBE_ANNOUNCES (0x11)

```
[RequestID varint]
[Namespace Tuple]
[Params KVP]
```

---

## SUBSCRIBE_ANNOUNCES_OK (0x12)

```
[RequestID varint]
```

---

## Object Datagram

Sent as QUIC datagrams (unreliable, unordered). Separate from control stream.

```
[Type varint][TrackAlias varint][GroupID varint][ObjectID varint][Priority 1 byte][Payload...]
```

### Datagram Type Field

| Type | Meaning              |
|------|----------------------|
| 0x00 | Object Datagram      |
| 0x01 | Object Datagram + Extensions |
| 0x02 | Status               |
| 0x03 | Status + Extensions  |

**Priority is a SINGLE BYTE, not varint.**

### Extensions (Type 0x01)

If type = 0x01, after Priority there is an extensions block:

```
[ExtCount varint][Key1 varint][ValLen1 varint][Val1 bytes]...
```

Skip all extensions to reach the payload.

---

## Byte-Level Examples

### Varint: 0xff00000b (MOQ version)

```
Prefix = 11 (8 bytes), value stored in 62 bits:
C0 00 00 00 FF 00 00 0B
```

### CLIENT_SETUP with 3 params (Role=PubSub, Path="/", MaxSubscribeId=100)

```
20                    # Type = CLIENT_SETUP (0x20, 1-byte varint)
00 12                 # Length = 18 bytes (big-endian uint16)
01                    # 1 supported version
C0 00 00 00 FF 00 00 0B  # Version = 0xff00000b (8-byte varint)
03                    # 3 parameters
00                    # Key = Role (0x00)
03                    # Value = PubSub (0x03)
01                    # Key = Path (0x01)
01                    # Length = 1
2F                    # "/" (0x2F)
02                    # Key = MaxSubscribeId (0x02)
40 64                 # Value = 100 (2-byte varint: 0x40 | (100>>8), 100&0xFF)
```

---

## Request ID Allocation

Server and client use different ID sequences (even/odd) to avoid collisions.
The specific parity convention depends on the implementation.
