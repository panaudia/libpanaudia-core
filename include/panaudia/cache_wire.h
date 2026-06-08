#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace panaudia {

// Cache envelope wire format. Byte-identical to:
//   spatial-mixer/core/statecache/wire.go
//   panaudia-client/sdks/typescript/src/shared/cache-wire.ts
//
// Layout (big-endian):
//   [Version    1 byte]   0xCA — distinguishes envelope from JSON (0x7B)
//                                or binary NodeInfo3 (UUID bytes).
//   [Flags      1 byte]   bit 0 = tombstone, others reserved.
//   [OpID       8 bytes]  monotonic op id assigned by the bouncer.
//   [NodeID     4 bytes]  origin node (informational, not for ordering).
//   [TopicLen   1 byte]
//   [Topic      N bytes]
//   [KeyLen     2 bytes]
//   [Key        N bytes]
//   [ValueLen   4 bytes]
//   [Value      N bytes]  opaque — for attributes today this is JSON
//                         (single op `{...}` or batch `[...]`).

inline constexpr uint8_t kCacheWireVersion   = 0xCA;
inline constexpr uint8_t kCacheFlagTombstone = 0x01;

// version + flags + op_id + node_id + topic_len + key_len + value_len
inline constexpr size_t kCacheWireMinLen = 1 + 1 + 8 + 4 + 1 + 2 + 4;  // 21

struct CacheOp {
    std::string topic;
    std::string key;
    std::string value;        // arbitrary bytes — std::string holds binary fine
    uint64_t    op_id   = 0;
    uint32_t    node_id = 0;
    bool        tombstone = false;
};

// Cheap version check — does not parse the rest of the header.
bool is_cache_envelope(const uint8_t* data, size_t len) noexcept;

// Decode a cache envelope. Returns std::nullopt on any malformed input
// (too short, wrong version, length overruns). Never throws.
std::optional<CacheOp> decode_cache_op(const uint8_t* data, size_t len);

// Encode an op into the wire format. Single allocation.
std::vector<uint8_t> encode_cache_op(const CacheOp& op);

}  // namespace panaudia
