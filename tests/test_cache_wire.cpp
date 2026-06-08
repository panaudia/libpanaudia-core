#include <catch2/catch_test_macros.hpp>
#include <panaudia/cache_wire.h>

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

using namespace panaudia;

namespace {

// Convert an ASCII hex string to bytes.
std::vector<uint8_t> hex_to_bytes(std::string_view hex) {
    std::vector<uint8_t> out;
    out.reserve(hex.size() / 2);
    auto nybble = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i + 1 < hex.size(); i += 2) {
        const int hi = nybble(hex[i]);
        const int lo = nybble(hex[i + 1]);
        REQUIRE(hi >= 0);
        REQUIRE(lo >= 0);
        out.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }
    return out;
}

// Mirrors tests/testdata/wire_vectors.json (the cross-language test
// vectors shared by Go, TypeScript, and now C++).
struct Vector {
    const char* name;
    const char* topic;
    const char* key;
    const char* value_hex;
    uint64_t    op_id;
    uint32_t    node_id;
    bool        tombstone;
    const char* encoded_hex;
};

const Vector kVectors[] = {
    {
        "normal_set", "attributes", "abc-123",
        "7b226e616d65223a22616c696365227d",
        65536005, 42, false,
        "ca000000000003e800050000002a"
        "0a6174747269627574657300076162632d313233"
        "000000107b226e616d65223a22616c696365227d",
    },
    {
        "tombstone", "attributes", "abc-123",
        "",
        131072000, 42, true,
        "ca010000000007d000000000002a"
        "0a6174747269627574657300076162632d31323300000000",
    },
    {
        "empty_value", "state", "node-1",
        "",
        32768001, 1, false,
        "ca000000000001f4000100000001"
        "05737461746500066e6f64652d3100000000",
    },
    {
        "max_key", "attributes",
        // 255 'K' bytes — the largest key the 2-byte length field… well,
        // 65535 in principle, but the Go vector uses 255 and that's
        // already enough to exercise length boundaries. Actually 0xFF is
        // a 1-byte topic length value; the key length below is encoded
        // in 2 bytes as 0x00FF.
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK"
        "KKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKKK",
        "76",
        655359999, 4294967295u, false,
        "ca0000000000270fffffffffffff"
        "0a6174747269627574657300ff"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b4b"
        "0000000176",
    },
    {
        "all_zero_value", "t", "k",
        "0000000000000000",
        65536, 0, false,
        "ca0000000000000100000000000001"
        "7400016b000000080000000000000000",
    },
    {
        "all_ff_value", "t", "k",
        "ffffffffffffffff",
        65536, 0, false,
        "ca0000000000000100000000000001"
        "7400016b00000008ffffffffffffffff",
    },
};

}  // namespace

// ============================================================================
// Cross-language vector tests [cache_wire][vectors]
// ============================================================================

TEST_CASE("Vector round-trip — encoded bytes match expected", "[cache_wire][vectors]") {
    for (const auto& v : kVectors) {
        SECTION(v.name) {
            CacheOp op;
            op.topic     = v.topic;
            op.key       = v.key;
            const auto value_bytes = hex_to_bytes(v.value_hex);
            op.value.assign(reinterpret_cast<const char*>(value_bytes.data()),
                            value_bytes.size());
            op.op_id     = v.op_id;
            op.node_id   = v.node_id;
            op.tombstone = v.tombstone;

            const auto encoded = encode_cache_op(op);
            const auto expected = hex_to_bytes(v.encoded_hex);

            REQUIRE(encoded.size() == expected.size());
            REQUIRE(std::memcmp(encoded.data(), expected.data(),
                                encoded.size()) == 0);
        }
    }
}

TEST_CASE("Vector decode — fields match", "[cache_wire][vectors]") {
    for (const auto& v : kVectors) {
        SECTION(v.name) {
            const auto wire = hex_to_bytes(v.encoded_hex);
            auto decoded = decode_cache_op(wire.data(), wire.size());
            REQUIRE(decoded.has_value());

            const auto value_bytes = hex_to_bytes(v.value_hex);
            const std::string expected_value(
                reinterpret_cast<const char*>(value_bytes.data()),
                value_bytes.size());

            REQUIRE(decoded->topic == v.topic);
            REQUIRE(decoded->key == v.key);
            REQUIRE(decoded->value == expected_value);
            REQUIRE(decoded->op_id == v.op_id);
            REQUIRE(decoded->node_id == v.node_id);
            REQUIRE(decoded->tombstone == v.tombstone);
        }
    }
}

TEST_CASE("Round-trip — encode then decode yields original", "[cache_wire][roundtrip]") {
    CacheOp orig;
    orig.topic     = "attributes";
    orig.key       = "abc-123";
    orig.value     = std::string("\x00\x01\x02\xFF\xFE", 5);
    orig.op_id     = 0xDEADBEEFCAFEBABEull;
    orig.node_id   = 0x12345678;
    orig.tombstone = false;

    const auto wire = encode_cache_op(orig);
    auto back = decode_cache_op(wire.data(), wire.size());
    REQUIRE(back.has_value());
    REQUIRE(back->topic == orig.topic);
    REQUIRE(back->key == orig.key);
    REQUIRE(back->value == orig.value);
    REQUIRE(back->op_id == orig.op_id);
    REQUIRE(back->node_id == orig.node_id);
    REQUIRE(back->tombstone == orig.tombstone);
}

TEST_CASE("Round-trip — tombstone with empty value", "[cache_wire][roundtrip]") {
    CacheOp orig;
    orig.topic     = "attributes";
    orig.key       = "node-uuid.field";
    orig.value     = "";
    orig.op_id     = 99;
    orig.node_id   = 7;
    orig.tombstone = true;

    const auto wire = encode_cache_op(orig);
    auto back = decode_cache_op(wire.data(), wire.size());
    REQUIRE(back.has_value());
    REQUIRE(back->tombstone == true);
    REQUIRE(back->value.empty());
}

TEST_CASE("Round-trip — large value (64 KB)", "[cache_wire][roundtrip]") {
    CacheOp orig;
    orig.topic = "t";
    orig.key   = "k";
    orig.value.resize(64 * 1024, '\xAB');
    orig.op_id = 1;

    const auto wire = encode_cache_op(orig);
    auto back = decode_cache_op(wire.data(), wire.size());
    REQUIRE(back.has_value());
    REQUIRE(back->value.size() == orig.value.size());
    REQUIRE(back->value == orig.value);
}

// ============================================================================
// Error handling [cache_wire][errors]
// ============================================================================

TEST_CASE("Decode rejects null/empty input", "[cache_wire][errors]") {
    REQUIRE_FALSE(decode_cache_op(nullptr, 0).has_value());
    REQUIRE_FALSE(decode_cache_op(nullptr, 100).has_value());
    uint8_t buf[1] = {0};
    REQUIRE_FALSE(decode_cache_op(buf, 0).has_value());
}

TEST_CASE("Decode rejects truncated input", "[cache_wire][errors]") {
    // 20 bytes — one short of the minimum 21
    uint8_t buf[20] = {0xCA};
    REQUIRE_FALSE(decode_cache_op(buf, sizeof(buf)).has_value());
}

TEST_CASE("Decode rejects wrong version byte", "[cache_wire][errors]") {
    std::vector<uint8_t> buf(kCacheWireMinLen, 0);
    buf[0] = 0x7B;  // JSON '{'
    REQUIRE_FALSE(decode_cache_op(buf.data(), buf.size()).has_value());
}

TEST_CASE("Decode rejects topic_len overrun", "[cache_wire][errors]") {
    // Header claims a 200-byte topic but only 21 bytes total.
    std::vector<uint8_t> buf(kCacheWireMinLen, 0);
    buf[0] = kCacheWireVersion;
    buf[14] = 200;  // topic_len
    REQUIRE_FALSE(decode_cache_op(buf.data(), buf.size()).has_value());
}

TEST_CASE("Decode rejects key_len overrun", "[cache_wire][errors]") {
    CacheOp op;
    op.topic = "t";
    op.key   = "k";
    auto wire = encode_cache_op(op);
    // Mangle key_len bytes (after version+flags+op_id+node_id+topic_len(1)+topic(1) = 16)
    REQUIRE(wire.size() > 18);
    wire[16] = 0xFF;
    wire[17] = 0xFF;
    REQUIRE_FALSE(decode_cache_op(wire.data(), wire.size()).has_value());
}

TEST_CASE("Decode rejects value_len overrun", "[cache_wire][errors]") {
    CacheOp op;
    op.topic = "t";
    op.key   = "k";
    auto wire = encode_cache_op(op);
    // value_len starts at: 1+1+8+4+1+1+2+1 = 19
    REQUIRE(wire.size() >= 23);
    wire[19] = 0xFF;
    wire[20] = 0xFF;
    wire[21] = 0xFF;
    wire[22] = 0xFF;
    REQUIRE_FALSE(decode_cache_op(wire.data(), wire.size()).has_value());
}

// ============================================================================
// is_cache_envelope [cache_wire][isenvelope]
// ============================================================================

TEST_CASE("is_cache_envelope detects envelope", "[cache_wire][isenvelope]") {
    uint8_t env[1] = {kCacheWireVersion};
    REQUIRE(is_cache_envelope(env, 1));
}

TEST_CASE("is_cache_envelope rejects JSON", "[cache_wire][isenvelope]") {
    uint8_t json[1] = {'{'};
    REQUIRE_FALSE(is_cache_envelope(json, 1));
}

TEST_CASE("is_cache_envelope rejects empty/null", "[cache_wire][isenvelope]") {
    REQUIRE_FALSE(is_cache_envelope(nullptr, 0));
    REQUIRE_FALSE(is_cache_envelope(nullptr, 100));
    uint8_t buf[1] = {0};
    REQUIRE_FALSE(is_cache_envelope(buf, 0));
}
