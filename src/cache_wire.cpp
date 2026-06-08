#include "panaudia/cache_wire.h"

#include <cstring>

namespace panaudia {

namespace {

inline uint16_t read_u16_be(const uint8_t* p) noexcept {
    return static_cast<uint16_t>((static_cast<uint16_t>(p[0]) << 8) |
                                  static_cast<uint16_t>(p[1]));
}

inline uint32_t read_u32_be(const uint8_t* p) noexcept {
    return (static_cast<uint32_t>(p[0]) << 24) |
           (static_cast<uint32_t>(p[1]) << 16) |
           (static_cast<uint32_t>(p[2]) << 8)  |
            static_cast<uint32_t>(p[3]);
}

inline uint64_t read_u64_be(const uint8_t* p) noexcept {
    return (static_cast<uint64_t>(p[0]) << 56) |
           (static_cast<uint64_t>(p[1]) << 48) |
           (static_cast<uint64_t>(p[2]) << 40) |
           (static_cast<uint64_t>(p[3]) << 32) |
           (static_cast<uint64_t>(p[4]) << 24) |
           (static_cast<uint64_t>(p[5]) << 16) |
           (static_cast<uint64_t>(p[6]) << 8)  |
            static_cast<uint64_t>(p[7]);
}

inline void write_u16_be(uint8_t* p, uint16_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 8);
    p[1] = static_cast<uint8_t>(v);
}

inline void write_u32_be(uint8_t* p, uint32_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 24);
    p[1] = static_cast<uint8_t>(v >> 16);
    p[2] = static_cast<uint8_t>(v >> 8);
    p[3] = static_cast<uint8_t>(v);
}

inline void write_u64_be(uint8_t* p, uint64_t v) noexcept {
    p[0] = static_cast<uint8_t>(v >> 56);
    p[1] = static_cast<uint8_t>(v >> 48);
    p[2] = static_cast<uint8_t>(v >> 40);
    p[3] = static_cast<uint8_t>(v >> 32);
    p[4] = static_cast<uint8_t>(v >> 24);
    p[5] = static_cast<uint8_t>(v >> 16);
    p[6] = static_cast<uint8_t>(v >> 8);
    p[7] = static_cast<uint8_t>(v);
}

}  // namespace

bool is_cache_envelope(const uint8_t* data, size_t len) noexcept {
    return len > 0 && data != nullptr && data[0] == kCacheWireVersion;
}

std::optional<CacheOp> decode_cache_op(const uint8_t* data, size_t len) {
    if (data == nullptr || len < kCacheWireMinLen) return std::nullopt;
    if (data[0] != kCacheWireVersion) return std::nullopt;

    size_t off = 0;
    // Version
    off += 1;

    // Flags
    const uint8_t flags = data[off];
    off += 1;

    // OpID
    const uint64_t op_id = read_u64_be(data + off);
    off += 8;

    // NodeID
    const uint32_t node_id = read_u32_be(data + off);
    off += 4;

    // Topic
    const uint8_t topic_len = data[off];
    off += 1;
    if (off + topic_len > len) return std::nullopt;
    std::string topic(reinterpret_cast<const char*>(data + off), topic_len);
    off += topic_len;

    // Key
    if (off + 2 > len) return std::nullopt;
    const uint16_t key_len = read_u16_be(data + off);
    off += 2;
    if (off + key_len > len) return std::nullopt;
    std::string key(reinterpret_cast<const char*>(data + off), key_len);
    off += key_len;

    // Value
    if (off + 4 > len) return std::nullopt;
    const uint32_t value_len = read_u32_be(data + off);
    off += 4;
    if (off + value_len > len) return std::nullopt;
    std::string value(reinterpret_cast<const char*>(data + off), value_len);
    off += value_len;

    CacheOp op;
    op.topic     = std::move(topic);
    op.key       = std::move(key);
    op.value     = std::move(value);
    op.op_id     = op_id;
    op.node_id   = node_id;
    op.tombstone = (flags & kCacheFlagTombstone) != 0;
    return op;
}

std::vector<uint8_t> encode_cache_op(const CacheOp& op) {
    // Truncate over-long fields silently — wire layout has hard limits.
    // Callers should not exceed these in practice; topic/key are fixed
    // small strings and the value is bounded by transport MTU.
    const size_t topic_len = op.topic.size() > 0xFF ? 0xFF : op.topic.size();
    const size_t key_len   = op.key.size()   > 0xFFFF ? 0xFFFF : op.key.size();
    const size_t value_len = op.value.size() > 0xFFFFFFFFu ? 0xFFFFFFFFu : op.value.size();

    const size_t total = kCacheWireMinLen + topic_len + key_len + value_len;
    std::vector<uint8_t> buf(total);
    uint8_t* p = buf.data();

    *p++ = kCacheWireVersion;
    *p++ = op.tombstone ? kCacheFlagTombstone : 0;

    write_u64_be(p, op.op_id);   p += 8;
    write_u32_be(p, op.node_id); p += 4;

    *p++ = static_cast<uint8_t>(topic_len);
    if (topic_len > 0) {
        std::memcpy(p, op.topic.data(), topic_len);
        p += topic_len;
    }

    write_u16_be(p, static_cast<uint16_t>(key_len)); p += 2;
    if (key_len > 0) {
        std::memcpy(p, op.key.data(), key_len);
        p += key_len;
    }

    write_u32_be(p, static_cast<uint32_t>(value_len)); p += 4;
    if (value_len > 0) {
        std::memcpy(p, op.value.data(), value_len);
        // p += value_len;  // unused
    }

    return buf;
}

}  // namespace panaudia
