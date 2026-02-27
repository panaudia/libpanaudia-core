#include <catch2/catch_test_macros.hpp>
#include <panaudia/moq_protocol.h>
#include <cstring>

using namespace panaudia::moq;

// ============================================================================
// Varint [moq_varint]
// ============================================================================

TEST_CASE("Varint encode 0", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(0, buf) == 1);
    REQUIRE(buf[0] == 0x00);
}

TEST_CASE("Varint encode 63 (1-byte max)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(63, buf) == 1);
    REQUIRE(buf[0] == 63);
}

TEST_CASE("Varint encode 64 (2-byte min)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(64, buf) == 2);
    REQUIRE(buf[0] == 0x40);
    REQUIRE(buf[1] == 0x40);
}

TEST_CASE("Varint encode 16383 (2-byte max)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(16383, buf) == 2);
    REQUIRE(buf[0] == 0x7F);
    REQUIRE(buf[1] == 0xFF);
}

TEST_CASE("Varint encode 16384 (4-byte min)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(16384, buf) == 4);
    REQUIRE(buf[0] == 0x80);
    REQUIRE(buf[1] == 0x00);
    REQUIRE(buf[2] == 0x40);
    REQUIRE(buf[3] == 0x00);
}

TEST_CASE("Varint encode 1073741823 (4-byte max)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(1073741823, buf) == 4);
    REQUIRE(buf[0] == 0xBF);
    REQUIRE(buf[1] == 0xFF);
    REQUIRE(buf[2] == 0xFF);
    REQUIRE(buf[3] == 0xFF);
}

TEST_CASE("Varint encode 1073741824 (8-byte min)", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(1073741824, buf) == 8);
    REQUIRE(buf[0] == 0xC0);
    REQUIRE(buf[1] == 0x00);
    REQUIRE(buf[2] == 0x00);
    REQUIRE(buf[3] == 0x00);
}

TEST_CASE("Varint encode MOQ version 0xff00000b", "[moq_varint]") {
    uint8_t buf[8];
    REQUIRE(encode_varint(0xff00000b, buf) == 8);
    // 0xC0 | (0xff00000b >> 56) = 0xC0 | 0 = 0xC0
    REQUIRE(buf[0] == 0xC0);
    REQUIRE(buf[1] == 0x00);
    REQUIRE(buf[2] == 0x00);
    REQUIRE(buf[3] == 0x00);
    REQUIRE(buf[4] == 0xFF);
    REQUIRE(buf[5] == 0x00);
    REQUIRE(buf[6] == 0x00);
    REQUIRE(buf[7] == 0x0B);
}

TEST_CASE("Varint roundtrip all boundary values", "[moq_varint]") {
    uint64_t values[] = {0, 63, 64, 16383, 16384, 1073741823, 1073741824, 0xff00000b};
    for (auto v : values) {
        uint8_t buf[8];
        int32_t written = encode_varint(v, buf);
        int32_t br = 0;
        uint64_t decoded = decode_varint(buf, written, br);
        REQUIRE(br == written);
        REQUIRE(decoded == v);
    }
}

TEST_CASE("Varint decode insufficient buffer", "[moq_varint]") {
    // Empty buffer
    int32_t br = 0;
    decode_varint(nullptr, 0, br);
    REQUIRE(br == 0);

    // 2-byte varint but only 1 byte provided
    uint8_t buf[8];
    encode_varint(64, buf);  // 2 bytes
    decode_varint(buf, 1, br);
    REQUIRE(br == 0);

    // 4-byte varint but only 3 bytes provided
    encode_varint(16384, buf);  // 4 bytes
    decode_varint(buf, 3, br);
    REQUIRE(br == 0);

    // 8-byte varint but only 7 bytes provided
    encode_varint(0xff00000b, buf);  // 8 bytes
    decode_varint(buf, 7, br);
    REQUIRE(br == 0);
}

TEST_CASE("varint_size matches encode_varint output length", "[moq_varint]") {
    uint64_t values[] = {0, 1, 63, 64, 16383, 16384, 1073741823, 1073741824, 0xff00000b};
    for (auto v : values) {
        uint8_t buf[8];
        int32_t written = encode_varint(v, buf);
        REQUIRE(varint_size(v) == written);
    }
}

// ============================================================================
// Namespace Tuple [moq_namespace]
// ============================================================================

TEST_CASE("Namespace empty", "[moq_namespace]") {
    auto encoded = encode_namespace({});
    REQUIRE(encoded.size() == 1);  // count=0 as 1-byte varint
    REQUIRE(encoded[0] == 0x00);

    int32_t offset = 0;
    std::vector<std::string> parts;
    REQUIRE(decode_namespace(encoded.data(), static_cast<int32_t>(encoded.size()), offset, parts));
    REQUIRE(parts.empty());
}

TEST_CASE("Namespace single part", "[moq_namespace]") {
    auto encoded = encode_namespace({"audio"});
    int32_t offset = 0;
    std::vector<std::string> parts;
    REQUIRE(decode_namespace(encoded.data(), static_cast<int32_t>(encoded.size()), offset, parts));
    REQUIRE(parts.size() == 1);
    REQUIRE(parts[0] == "audio");
}

TEST_CASE("Namespace multi-part", "[moq_namespace]") {
    std::vector<std::string> original = {"out", "audio", "opus-stereo", "abc123"};
    auto encoded = encode_namespace(original);
    int32_t offset = 0;
    std::vector<std::string> parts;
    REQUIRE(decode_namespace(encoded.data(), static_cast<int32_t>(encoded.size()), offset, parts));
    REQUIRE(parts == original);
}

TEST_CASE("Namespace with empty string part", "[moq_namespace]") {
    std::vector<std::string> original = {"out", "", "audio"};
    auto encoded = encode_namespace(original);
    int32_t offset = 0;
    std::vector<std::string> parts;
    REQUIRE(decode_namespace(encoded.data(), static_cast<int32_t>(encoded.size()), offset, parts));
    REQUIRE(parts == original);
}

TEST_CASE("Namespace with UTF-8", "[moq_namespace]") {
    std::vector<std::string> original = {"ns", u8"\xC3\xA9\xC3\xA0\xC3\xBC"};  // éàü
    auto encoded = encode_namespace(original);
    int32_t offset = 0;
    std::vector<std::string> parts;
    REQUIRE(decode_namespace(encoded.data(), static_cast<int32_t>(encoded.size()), offset, parts));
    REQUIRE(parts == original);
}

// ============================================================================
// KVP Parameters [moq_kvp]
// ============================================================================

TEST_CASE("KVP empty params", "[moq_kvp]") {
    auto encoded = encode_params({});
    REQUIRE(encoded.size() == 1);  // count=0
    REQUIRE(encoded[0] == 0x00);

    int32_t offset = 0;
    std::vector<KvpParam> params;
    REQUIRE(decode_params(encoded.data(), static_cast<int32_t>(encoded.size()), offset, params));
    REQUIRE(params.empty());
}

TEST_CASE("KVP even key (bare varint)", "[moq_kvp]") {
    KvpParam p;
    p.key = 0x00;     // Role
    p.int_value = 0x03;  // PubSub
    auto encoded = encode_params({p});

    int32_t offset = 0;
    std::vector<KvpParam> decoded;
    REQUIRE(decode_params(encoded.data(), static_cast<int32_t>(encoded.size()), offset, decoded));
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].key == 0x00);
    REQUIRE(decoded[0].int_value == 0x03);
}

TEST_CASE("KVP odd key (length-prefixed bytes)", "[moq_kvp]") {
    KvpParam p;
    p.key = 0x01;     // Path
    std::string path = "/";
    p.bytes_value.assign(path.begin(), path.end());
    auto encoded = encode_params({p});

    int32_t offset = 0;
    std::vector<KvpParam> decoded;
    REQUIRE(decode_params(encoded.data(), static_cast<int32_t>(encoded.size()), offset, decoded));
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].key == 0x01);
    REQUIRE(decoded[0].bytes_value.size() == 1);
    REQUIRE(decoded[0].bytes_value[0] == '/');
}

TEST_CASE("KVP mixed even/odd keys", "[moq_kvp]") {
    std::vector<KvpParam> params;

    KvpParam role;
    role.key = 0x00;
    role.int_value = 0x03;
    params.push_back(role);

    KvpParam path;
    path.key = 0x01;
    std::string p = "/test";
    path.bytes_value.assign(p.begin(), p.end());
    params.push_back(path);

    KvpParam max_sub;
    max_sub.key = 0x02;
    max_sub.int_value = 100;
    params.push_back(max_sub);

    auto encoded = encode_params(params);

    int32_t offset = 0;
    std::vector<KvpParam> decoded;
    REQUIRE(decode_params(encoded.data(), static_cast<int32_t>(encoded.size()), offset, decoded));
    REQUIRE(decoded.size() == 3);
    REQUIRE(decoded[0].key == 0x00);
    REQUIRE(decoded[0].int_value == 0x03);
    REQUIRE(decoded[1].key == 0x01);
    REQUIRE(std::string(decoded[1].bytes_value.begin(), decoded[1].bytes_value.end()) == "/test");
    REQUIRE(decoded[2].key == 0x02);
    REQUIRE(decoded[2].int_value == 100);
}

TEST_CASE("KVP auth token", "[moq_kvp]") {
    KvpParam auth;
    auth.key = kParamKeyAuthToken;  // 0x03, odd
    std::string token = "eyJhbGciOiJFZERTQSIsInR5cCI6IkpXVCJ9.test";
    auth.bytes_value.assign(token.begin(), token.end());
    auto encoded = encode_params({auth});

    int32_t offset = 0;
    std::vector<KvpParam> decoded;
    REQUIRE(decode_params(encoded.data(), static_cast<int32_t>(encoded.size()), offset, decoded));
    REQUIRE(decoded.size() == 1);
    REQUIRE(decoded[0].key == kParamKeyAuthToken);
    std::string got(decoded[0].bytes_value.begin(), decoded[0].bytes_value.end());
    REQUIRE(got == token);
}

// ============================================================================
// Framing [moq_framing]
// ============================================================================

TEST_CASE("Framing build+parse roundtrip", "[moq_framing]") {
    std::vector<uint8_t> content = {0x01, 0x02, 0x03, 0x04};
    auto msg = build_control_message(MessageType::Subscribe, content);

    MessageType type;
    const uint8_t* parsed_content = nullptr;
    int32_t parsed_content_len = 0;
    int32_t consumed = 0;

    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, parsed_content, parsed_content_len, consumed));
    REQUIRE(type == MessageType::Subscribe);
    REQUIRE(parsed_content_len == 4);
    REQUIRE(std::memcmp(parsed_content, content.data(), 4) == 0);
    REQUIRE(consumed == static_cast<int32_t>(msg.size()));
}

TEST_CASE("Framing incomplete message returns false", "[moq_framing]") {
    std::vector<uint8_t> content = {0x01, 0x02};
    auto msg = build_control_message(MessageType::Announce, content);

    // Truncate
    MessageType type;
    const uint8_t* parsed_content = nullptr;
    int32_t parsed_content_len = 0;
    int32_t consumed = 0;

    REQUIRE_FALSE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()) - 1,
                                        type, parsed_content, parsed_content_len, consumed));
}

TEST_CASE("Framing two sequential messages", "[moq_framing]") {
    std::vector<uint8_t> c1 = {0xAA};
    std::vector<uint8_t> c2 = {0xBB, 0xCC};
    auto m1 = build_control_message(MessageType::AnnounceOk, c1);
    auto m2 = build_control_message(MessageType::SubscribeOk, c2);

    std::vector<uint8_t> combined;
    combined.insert(combined.end(), m1.begin(), m1.end());
    combined.insert(combined.end(), m2.begin(), m2.end());

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;

    // Parse first
    REQUIRE(parse_control_message(combined.data(), static_cast<int32_t>(combined.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::AnnounceOk);
    REQUIRE(content_len == 1);
    REQUIRE(content[0] == 0xAA);

    // Parse second
    int32_t remaining = static_cast<int32_t>(combined.size()) - consumed;
    REQUIRE(parse_control_message(combined.data() + consumed, remaining,
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::SubscribeOk);
    REQUIRE(content_len == 2);
    REQUIRE(content[0] == 0xBB);
    REQUIRE(content[1] == 0xCC);
}

TEST_CASE("Framing empty content", "[moq_framing]") {
    std::vector<uint8_t> content;
    auto msg = build_control_message(MessageType::ServerSetup, content);

    MessageType type;
    const uint8_t* parsed_content = nullptr;
    int32_t parsed_content_len = 0;
    int32_t consumed = 0;

    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, parsed_content, parsed_content_len, consumed));
    REQUIRE(type == MessageType::ServerSetup);
    REQUIRE(parsed_content_len == 0);
}

// ============================================================================
// Datagram [moq_datagram]
// ============================================================================

TEST_CASE("Datagram build+parse roundtrip", "[moq_datagram]") {
    uint8_t payload[] = {0xDE, 0xAD, 0xBE, 0xEF};
    auto dg = build_object_datagram(1, 42, 7, 128, payload, 4);

    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.track_alias == 1);
    REQUIRE(out.group_id == 42);
    REQUIRE(out.object_id == 7);
    REQUIRE(out.priority == 128);
    REQUIRE(out.payload_len == 4);
    REQUIRE(std::memcmp(out.payload, payload, 4) == 0);
}

TEST_CASE("Datagram small alias", "[moq_datagram]") {
    auto dg = build_object_datagram(0, 0, 0, 0, nullptr, 0);
    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.track_alias == 0);
    REQUIRE(out.group_id == 0);
    REQUIRE(out.object_id == 0);
    REQUIRE(out.priority == 0);
    REQUIRE(out.payload_len == 0);
}

TEST_CASE("Datagram large alias", "[moq_datagram]") {
    uint8_t payload[] = {0x42};
    auto dg = build_object_datagram(999999, 100000, 50000, 200, payload, 1);
    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.track_alias == 999999);
    REQUIRE(out.group_id == 100000);
    REQUIRE(out.object_id == 50000);
    REQUIRE(out.priority == 200);
    REQUIRE(out.payload_len == 1);
    REQUIRE(out.payload[0] == 0x42);
}

TEST_CASE("Datagram empty payload", "[moq_datagram]") {
    auto dg = build_object_datagram(5, 10, 15, 64, nullptr, 0);
    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.track_alias == 5);
    REQUIRE(out.payload_len == 0);
}

TEST_CASE("Datagram max priority 255", "[moq_datagram]") {
    uint8_t payload[] = {0x01};
    auto dg = build_object_datagram(1, 1, 1, 255, payload, 1);
    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.priority == 255);
}

TEST_CASE("Datagram extensions type (0x01) parsed by skipping extensions", "[moq_datagram]") {
    // Manually build a type=0x01 datagram with 1 extension
    std::vector<uint8_t> dg;
    uint8_t buf[8];
    int32_t n;

    // Type = 0x01 (datagram + extensions)
    n = encode_varint(0x01, buf);
    dg.insert(dg.end(), buf, buf + n);
    // TrackAlias = 3
    n = encode_varint(3, buf);
    dg.insert(dg.end(), buf, buf + n);
    // GroupID = 10
    n = encode_varint(10, buf);
    dg.insert(dg.end(), buf, buf + n);
    // ObjectID = 20
    n = encode_varint(20, buf);
    dg.insert(dg.end(), buf, buf + n);
    // Priority = 100
    dg.push_back(100);
    // Extensions: count=1, key=0x42, val_len=2, val=[0xAA, 0xBB]
    n = encode_varint(1, buf);  // ext count
    dg.insert(dg.end(), buf, buf + n);
    n = encode_varint(0x42, buf);  // key
    dg.insert(dg.end(), buf, buf + n);
    n = encode_varint(2, buf);  // val len
    dg.insert(dg.end(), buf, buf + n);
    dg.push_back(0xAA);
    dg.push_back(0xBB);
    // Payload
    dg.push_back(0xFF);

    ObjectDatagram out;
    REQUIRE(parse_object_datagram(dg.data(), static_cast<int32_t>(dg.size()), out));
    REQUIRE(out.track_alias == 3);
    REQUIRE(out.group_id == 10);
    REQUIRE(out.object_id == 20);
    REQUIRE(out.priority == 100);
    REQUIRE(out.payload_len == 1);
    REQUIRE(out.payload[0] == 0xFF);
}

TEST_CASE("Datagram truncated returns false", "[moq_datagram]") {
    uint8_t payload[] = {0x01};
    auto dg = build_object_datagram(1, 1, 1, 128, payload, 1);

    ObjectDatagram out;
    // Just the type byte — not enough for the rest
    REQUIRE_FALSE(parse_object_datagram(dg.data(), 1, out));
    // Two bytes — still not enough
    REQUIRE_FALSE(parse_object_datagram(dg.data(), 2, out));
}

TEST_CASE("Datagram zero-copy header matches full build", "[moq_datagram]") {
    uint8_t payload[] = {0xDE, 0xAD};
    auto full = build_object_datagram(42, 100, 200, 128, payload, 2);

    uint8_t header_buf[34];
    int32_t header_len = build_object_datagram_header(42, 100, 200, 128, header_buf);

    // Header should match the beginning of full build
    REQUIRE(header_len > 0);
    REQUIRE(header_len == static_cast<int32_t>(full.size()) - 2);  // minus payload
    REQUIRE(std::memcmp(header_buf, full.data(), static_cast<size_t>(header_len)) == 0);

    // Reassemble: header + payload should equal full
    std::vector<uint8_t> reassembled(header_buf, header_buf + header_len);
    reassembled.insert(reassembled.end(), payload, payload + 2);
    REQUIRE(reassembled == full);
}

// ============================================================================
// Setup [moq_setup]
// ============================================================================

TEST_CASE("CLIENT_SETUP default config", "[moq_setup]") {
    auto msg = build_client_setup();

    // Should be a valid framed message
    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;

    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::ClientSetup);
    REQUIRE(content_len > 0);
    REQUIRE(consumed == static_cast<int32_t>(msg.size()));
}

TEST_CASE("CLIENT_SETUP contains version and 3 params", "[moq_setup]") {
    auto msg = build_client_setup();

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;

    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    // Parse content manually: [version_count=1][version][param_count=3][params...]
    int32_t offset = 0;
    int32_t br = 0;

    uint64_t ver_count = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(br > 0);
    offset += br;
    REQUIRE(ver_count == 1);

    uint64_t version = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(br > 0);
    offset += br;
    REQUIRE(version == kMoqVersion);

    uint64_t param_count = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(br > 0);
    offset += br;
    REQUIRE(param_count == 3);
}

TEST_CASE("SERVER_SETUP parse version and params", "[moq_setup]") {
    // Build a fake SERVER_SETUP content: [version][param_count=0]
    std::vector<uint8_t> content;
    uint8_t buf[8];
    int32_t n;
    n = encode_varint(kMoqVersion, buf);
    content.insert(content.end(), buf, buf + n);
    n = encode_varint(0, buf);  // 0 params
    content.insert(content.end(), buf, buf + n);

    ServerSetupResult result;
    REQUIRE(parse_server_setup(content.data(), static_cast<int32_t>(content.size()), result));
    REQUIRE(result.version == kMoqVersion);
    REQUIRE(result.params.empty());
}

TEST_CASE("SERVER_SETUP parse with params", "[moq_setup]") {
    std::vector<uint8_t> content;
    uint8_t buf[8];
    int32_t n;

    // Version
    n = encode_varint(kMoqVersion, buf);
    content.insert(content.end(), buf, buf + n);

    // 1 param: Role=PubSub (key=0x00, even, value=0x03)
    n = encode_varint(1, buf);
    content.insert(content.end(), buf, buf + n);
    n = encode_varint(0x00, buf);
    content.insert(content.end(), buf, buf + n);
    n = encode_varint(0x03, buf);
    content.insert(content.end(), buf, buf + n);

    ServerSetupResult result;
    REQUIRE(parse_server_setup(content.data(), static_cast<int32_t>(content.size()), result));
    REQUIRE(result.version == kMoqVersion);
    REQUIRE(result.params.size() == 1);
    REQUIRE(result.params[0].key == 0x00);
    REQUIRE(result.params[0].int_value == 0x03);
}

// ============================================================================
// Announce [moq_announce]
// ============================================================================

TEST_CASE("ANNOUNCE build+parse roundtrip", "[moq_announce]") {
    AnnounceConfig config;
    config.request_id = 5;
    config.track_namespace = {"out", "audio", "opus-stereo"};
    auto msg = build_announce(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::Announce);

    AnnounceResult result;
    REQUIRE(parse_announce(content, content_len, result));
    REQUIRE(result.request_id == 5);
    REQUIRE(result.track_namespace == std::vector<std::string>{"out", "audio", "opus-stereo"});
    REQUIRE(result.params.empty());
}

TEST_CASE("ANNOUNCE_OK build+parse roundtrip", "[moq_announce]") {
    auto msg = build_announce_ok(42);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::AnnounceOk);

    AnnounceOkResult result;
    REQUIRE(parse_announce_ok(content, content_len, result));
    REQUIRE(result.request_id == 42);
}

TEST_CASE("ANNOUNCE with request_id 0", "[moq_announce]") {
    AnnounceConfig config;
    config.request_id = 0;
    config.track_namespace = {"test"};
    auto msg = build_announce(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    AnnounceResult result;
    REQUIRE(parse_announce(content, content_len, result));
    REQUIRE(result.request_id == 0);
}

// ============================================================================
// Subscribe [moq_subscribe]
// ============================================================================

TEST_CASE("SUBSCRIBE build without auth (0 params)", "[moq_subscribe]") {
    SubscribeConfig config;
    config.request_id = 1;
    config.track_namespace = {"out", "audio"};
    config.track_name = "opus-mono";
    config.priority = 128;

    auto msg = build_subscribe(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::Subscribe);

    SubscribeResult result;
    REQUIRE(parse_subscribe(content, content_len, result));
    REQUIRE(result.request_id == 1);
    REQUIRE(result.track_namespace == std::vector<std::string>{"out", "audio"});
    REQUIRE(result.track_name == "opus-mono");
    REQUIRE(result.priority == 128);
    REQUIRE(result.group_order == 0);
    REQUIRE(result.forward == 0);
    REQUIRE(result.filter_type == kFilterLatestGroup);
    REQUIRE(result.params.empty());
}

TEST_CASE("SUBSCRIBE build with auth token (KVP 0x03)", "[moq_subscribe]") {
    SubscribeConfig config;
    config.request_id = 2;
    config.track_namespace = {"out", "audio", "opus-stereo", "user-123"};
    config.track_name = "";
    config.priority = 200;
    config.authorization = "eyJ0ZXN0IjoiYXV0aCJ9";

    auto msg = build_subscribe(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    SubscribeResult result;
    REQUIRE(parse_subscribe(content, content_len, result));
    REQUIRE(result.request_id == 2);
    REQUIRE(result.track_namespace.size() == 4);
    REQUIRE(result.track_name.empty());
    REQUIRE(result.priority == 200);
    REQUIRE(result.params.size() == 1);
    REQUIRE(result.params[0].key == kParamKeyAuthToken);
    std::string auth(result.params[0].bytes_value.begin(),
                     result.params[0].bytes_value.end());
    REQUIRE(auth == "eyJ0ZXN0IjoiYXV0aCJ9");
}

TEST_CASE("SUBSCRIBE parse all fields", "[moq_subscribe]") {
    SubscribeConfig config;
    config.request_id = 99;
    config.track_namespace = {"ns1", "ns2"};
    config.track_name = "track-a";
    config.priority = 50;
    config.group_order = 1;
    config.forward = 2;
    config.filter_type = kFilterLatestObject;

    auto msg = build_subscribe(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    SubscribeResult result;
    REQUIRE(parse_subscribe(content, content_len, result));
    REQUIRE(result.request_id == 99);
    REQUIRE(result.track_namespace == std::vector<std::string>{"ns1", "ns2"});
    REQUIRE(result.track_name == "track-a");
    REQUIRE(result.priority == 50);
    REQUIRE(result.group_order == 1);
    REQUIRE(result.forward == 2);
    REQUIRE(result.filter_type == kFilterLatestObject);
}

TEST_CASE("SUBSCRIBE wire bytes contain NO TrackAlias", "[moq_subscribe]") {
    SubscribeConfig config;
    config.request_id = 1;
    config.track_namespace = {"ns"};
    config.track_name = "t";
    config.priority = 128;

    auto msg = build_subscribe(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    // Parse content byte-by-byte to verify NO TrackAlias:
    // [RequestID=1][NamespaceCount=1]["ns" len=2][0x6E,0x73]["t" len=1][0x74]
    // [Priority=128][GroupOrder=0][Forward=0][FilterType=1][ParamCount=0]
    int32_t offset = 0;
    int32_t br = 0;

    // RequestID
    uint64_t req = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(req == 1);
    offset += br;

    // Namespace count
    uint64_t ns_count = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(ns_count == 1);
    offset += br;

    // Namespace part "ns"
    uint64_t ns_len = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(ns_len == 2);
    offset += br;
    REQUIRE(content[offset] == 'n');
    REQUIRE(content[offset + 1] == 's');
    offset += 2;

    // Track name "t"
    uint64_t tn_len = decode_varint(content + offset, content_len - offset, br);
    REQUIRE(tn_len == 1);
    offset += br;
    REQUIRE(content[offset] == 't');
    offset += 1;

    // Priority (1 byte) — next byte after track name, NOT a varint TrackAlias
    REQUIRE(content[offset] == 128);
}

TEST_CASE("SUBSCRIBE_OK build+parse roundtrip", "[moq_subscribe]") {
    SubscribeOkConfig config;
    config.request_id = 7;
    config.track_alias = 42;
    config.expires_ms = 0;
    config.group_order = 0;
    config.content_exists = false;

    auto msg = build_subscribe_ok(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::SubscribeOk);

    SubscribeOkResult result;
    REQUIRE(parse_subscribe_ok(content, content_len, result));
    REQUIRE(result.request_id == 7);
    REQUIRE(result.track_alias == 42);
    REQUIRE(result.expires_ms == 0);
    REQUIRE(result.group_order == 0);
    REQUIRE(result.content_exists == false);
}

TEST_CASE("SUBSCRIBE_OK extract track_alias", "[moq_subscribe]") {
    SubscribeOkConfig config;
    config.request_id = 1;
    config.track_alias = 12345;
    config.content_exists = false;

    auto msg = build_subscribe_ok(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    SubscribeOkResult result;
    REQUIRE(parse_subscribe_ok(content, content_len, result));
    REQUIRE(result.track_alias == 12345);
}

TEST_CASE("SUBSCRIBE_OK with content_exists=true", "[moq_subscribe]") {
    SubscribeOkConfig config;
    config.request_id = 3;
    config.track_alias = 10;
    config.expires_ms = 5000;
    config.group_order = 1;
    config.content_exists = true;
    config.largest_group_id = 100;
    config.largest_object_id = 50;

    auto msg = build_subscribe_ok(config);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));

    SubscribeOkResult result;
    REQUIRE(parse_subscribe_ok(content, content_len, result));
    REQUIRE(result.request_id == 3);
    REQUIRE(result.track_alias == 10);
    REQUIRE(result.expires_ms == 5000);
    REQUIRE(result.group_order == 1);
    REQUIRE(result.content_exists == true);
    REQUIRE(result.largest_group_id == 100);
    REQUIRE(result.largest_object_id == 50);
}

TEST_CASE("SUBSCRIBE_ERROR parse", "[moq_subscribe]") {
    // Build SUBSCRIBE_ERROR content manually:
    // [RequestID=5][ErrorCode=0x01][ReasonPhrase="denied"][TrackAlias=0]
    std::vector<uint8_t> content;
    uint8_t buf[8];
    int32_t n;

    n = encode_varint(5, buf);
    content.insert(content.end(), buf, buf + n);
    n = encode_varint(0x01, buf);
    content.insert(content.end(), buf, buf + n);
    // Reason phrase: length-prefixed string
    std::string reason = "denied";
    n = encode_varint(reason.size(), buf);
    content.insert(content.end(), buf, buf + n);
    content.insert(content.end(), reason.begin(), reason.end());
    // TrackAlias
    n = encode_varint(0, buf);
    content.insert(content.end(), buf, buf + n);

    SubscribeErrorResult result;
    REQUIRE(parse_subscribe_error(content.data(), static_cast<int32_t>(content.size()), result));
    REQUIRE(result.request_id == 5);
    REQUIRE(result.error_code == 0x01);
    REQUIRE(result.reason == "denied");
    REQUIRE(result.track_alias == 0);
}

// ============================================================================
// Subscribe Announces [moq_subscribe_announces]
// ============================================================================

TEST_CASE("SUBSCRIBE_ANNOUNCES parse", "[moq_subscribe_announces]") {
    // Build content: [RequestID=10][Namespace Tuple]["panaudia"][Params: 0]
    std::vector<uint8_t> content;
    uint8_t buf[8];
    int32_t n;

    n = encode_varint(10, buf);
    content.insert(content.end(), buf, buf + n);
    auto ns = encode_namespace({"panaudia"});
    content.insert(content.end(), ns.begin(), ns.end());
    n = encode_varint(0, buf);  // 0 params
    content.insert(content.end(), buf, buf + n);

    SubscribeAnnouncesResult result;
    REQUIRE(parse_subscribe_announces(content.data(), static_cast<int32_t>(content.size()), result));
    REQUIRE(result.request_id == 10);
    REQUIRE(result.track_namespace == std::vector<std::string>{"panaudia"});
    REQUIRE(result.params.empty());
}

TEST_CASE("SUBSCRIBE_ANNOUNCES_OK build", "[moq_subscribe_announces]") {
    auto msg = build_subscribe_announces_ok(10);

    MessageType type;
    const uint8_t* content = nullptr;
    int32_t content_len = 0;
    int32_t consumed = 0;
    REQUIRE(parse_control_message(msg.data(), static_cast<int32_t>(msg.size()),
                                  type, content, content_len, consumed));
    REQUIRE(type == MessageType::SubscribeAnnouncesOk);

    int32_t br = 0;
    uint64_t req_id = decode_varint(content, content_len, br);
    REQUIRE(br > 0);
    REQUIRE(req_id == 10);
}

// ============================================================================
// Cross-compat [moq_compat]
// ============================================================================

TEST_CASE("CLIENT_SETUP byte-for-byte match with UE plugin output", "[moq_compat]") {
    // Expected bytes from UE plugin SendClientSetup() with 3 params:
    // Type=0x20 (1 byte varint)
    // Length=0x0010 (2 byte BE = 16)
    // Content:
    //   01                          # version_count = 1
    //   C0 00 00 00 FF 00 00 0B     # version = 0xff00000b (8-byte varint)
    //   03                          # param_count = 3
    //   00                          # key = Role (0x00)
    //   03                          # value = PubSub (0x03)
    //   01                          # key = Path (0x01)
    //   01                          # length = 1
    //   2F                          # "/" (0x2F)
    //   02                          # key = MaxSubscribeId (0x02)
    //   40 64                       # value = 100 (2-byte varint)
    const uint8_t expected[] = {
        0x20,                                       // Type
        0x00, 0x12,                                 // Length = 18
        0x01,                                       // 1 version
        0xC0, 0x00, 0x00, 0x00, 0xFF, 0x00, 0x00, 0x0B,  // version
        0x03,                                       // 3 params
        0x00, 0x03,                                 // Role = PubSub
        0x01, 0x01, 0x2F,                           // Path = "/"
        0x02, 0x40, 0x64,                           // MaxSubscribeId = 100
    };

    auto msg = build_client_setup();
    REQUIRE(msg.size() == sizeof(expected));
    REQUIRE(std::memcmp(msg.data(), expected, sizeof(expected)) == 0);
}

TEST_CASE("SUBSCRIBE byte-for-byte match with UE plugin output", "[moq_compat]") {
    // UE plugin BuildSubscribe(RequestId=1, Namespace={"ns"}, TrackName="t",
    //                          Priority=128, Authorization="")
    // Type=0x03 (1 byte varint)
    // Content:
    //   01              # RequestID = 1
    //   01              # Namespace count = 1
    //   02 6E 73        # "ns" (len=2)
    //   01 74           # "t" (len=1)
    //   80              # Priority = 128
    //   00              # GroupOrder = 0
    //   00              # Forward = 0
    //   01              # FilterType = LatestGroup
    //   00              # ParamCount = 0
    const uint8_t expected[] = {
        0x03,                   // Type = SUBSCRIBE
        0x00, 0x0C,             // Length = 12
        0x01,                   // RequestID = 1
        0x01,                   // Namespace count = 1
        0x02, 0x6E, 0x73,      // "ns"
        0x01, 0x74,             // "t"
        0x80,                   // Priority = 128
        0x00,                   // GroupOrder
        0x00,                   // Forward
        0x01,                   // FilterType = LatestGroup
        0x00,                   // 0 params
    };

    SubscribeConfig config;
    config.request_id = 1;
    config.track_namespace = {"ns"};
    config.track_name = "t";
    config.priority = 128;

    auto msg = build_subscribe(config);
    REQUIRE(msg.size() == sizeof(expected));
    REQUIRE(std::memcmp(msg.data(), expected, sizeof(expected)) == 0);
}
