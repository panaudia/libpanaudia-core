#include <panaudia/moq_protocol.h>
#include <cstring>

namespace panaudia {
namespace moq {

// ---------------------------------------------------------------------------
// Internal helpers
// ---------------------------------------------------------------------------

static void append_varint(std::vector<uint8_t>& vec, uint64_t value) {
    uint8_t buf[8];
    int32_t len = encode_varint(value, buf);
    vec.insert(vec.end(), buf, buf + len);
}

static void append_string_bytes(std::vector<uint8_t>& vec, const std::string& str) {
    append_varint(vec, static_cast<uint64_t>(str.size()));
    vec.insert(vec.end(),
               reinterpret_cast<const uint8_t*>(str.data()),
               reinterpret_cast<const uint8_t*>(str.data()) + str.size());
}

static bool read_string(const uint8_t* buffer, int32_t buffer_len,
                        int32_t& offset, std::string& out) {
    int32_t br = 0;
    uint64_t len = decode_varint(buffer + offset, buffer_len - offset, br);
    if (br == 0) return false;
    offset += br;

    if (offset + static_cast<int32_t>(len) > buffer_len) return false;
    out.assign(reinterpret_cast<const char*>(buffer + offset),
               static_cast<size_t>(len));
    offset += static_cast<int32_t>(len);
    return true;
}

static bool read_varint(const uint8_t* buffer, int32_t buffer_len,
                        int32_t& offset, uint64_t& out) {
    int32_t br = 0;
    out = decode_varint(buffer + offset, buffer_len - offset, br);
    if (br == 0) return false;
    offset += br;
    return true;
}

static bool read_byte(const uint8_t* buffer, int32_t buffer_len,
                      int32_t& offset, uint8_t& out) {
    if (offset >= buffer_len) return false;
    out = buffer[offset];
    offset += 1;
    return true;
}

// ---------------------------------------------------------------------------
// Varint — RFC 9000 Section 16
// ---------------------------------------------------------------------------

int32_t encode_varint(uint64_t value, uint8_t* buffer) {
    if (value <= 63) {
        buffer[0] = static_cast<uint8_t>(value);
        return 1;
    }
    if (value <= 16383) {
        buffer[0] = static_cast<uint8_t>((value >> 8) | 0x40);
        buffer[1] = static_cast<uint8_t>(value & 0xFF);
        return 2;
    }
    if (value <= 1073741823) {
        buffer[0] = static_cast<uint8_t>((value >> 24) | 0x80);
        buffer[1] = static_cast<uint8_t>((value >> 16) & 0xFF);
        buffer[2] = static_cast<uint8_t>((value >> 8) & 0xFF);
        buffer[3] = static_cast<uint8_t>(value & 0xFF);
        return 4;
    }
    buffer[0] = static_cast<uint8_t>((value >> 56) | 0xC0);
    buffer[1] = static_cast<uint8_t>((value >> 48) & 0xFF);
    buffer[2] = static_cast<uint8_t>((value >> 40) & 0xFF);
    buffer[3] = static_cast<uint8_t>((value >> 32) & 0xFF);
    buffer[4] = static_cast<uint8_t>((value >> 24) & 0xFF);
    buffer[5] = static_cast<uint8_t>((value >> 16) & 0xFF);
    buffer[6] = static_cast<uint8_t>((value >> 8) & 0xFF);
    buffer[7] = static_cast<uint8_t>(value & 0xFF);
    return 8;
}

uint64_t decode_varint(const uint8_t* buffer, int32_t buffer_len,
                       int32_t& bytes_read) {
    if (buffer_len < 1) {
        bytes_read = 0;
        return 0;
    }

    uint8_t prefix = buffer[0] >> 6;

    switch (prefix) {
    case 0:
        bytes_read = 1;
        return buffer[0] & 0x3F;

    case 1:
        if (buffer_len < 2) { bytes_read = 0; return 0; }
        bytes_read = 2;
        return (static_cast<uint64_t>(buffer[0] & 0x3F) << 8) |
               static_cast<uint64_t>(buffer[1]);

    case 2:
        if (buffer_len < 4) { bytes_read = 0; return 0; }
        bytes_read = 4;
        return (static_cast<uint64_t>(buffer[0] & 0x3F) << 24) |
               (static_cast<uint64_t>(buffer[1]) << 16) |
               (static_cast<uint64_t>(buffer[2]) << 8) |
               static_cast<uint64_t>(buffer[3]);

    case 3:
        if (buffer_len < 8) { bytes_read = 0; return 0; }
        bytes_read = 8;
        return (static_cast<uint64_t>(buffer[0] & 0x3F) << 56) |
               (static_cast<uint64_t>(buffer[1]) << 48) |
               (static_cast<uint64_t>(buffer[2]) << 40) |
               (static_cast<uint64_t>(buffer[3]) << 32) |
               (static_cast<uint64_t>(buffer[4]) << 24) |
               (static_cast<uint64_t>(buffer[5]) << 16) |
               (static_cast<uint64_t>(buffer[6]) << 8) |
               static_cast<uint64_t>(buffer[7]);

    default:
        bytes_read = 0;
        return 0;
    }
}

int32_t varint_size(uint64_t value) {
    if (value <= 63) return 1;
    if (value <= 16383) return 2;
    if (value <= 1073741823) return 4;
    return 8;
}

// ---------------------------------------------------------------------------
// Namespace Tuple
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_namespace(const std::vector<std::string>& parts) {
    std::vector<uint8_t> result;
    append_varint(result, static_cast<uint64_t>(parts.size()));
    for (const auto& part : parts) {
        append_string_bytes(result, part);
    }
    return result;
}

bool decode_namespace(const uint8_t* buffer, int32_t buffer_len,
                      int32_t& offset, std::vector<std::string>& parts) {
    uint64_t count = 0;
    if (!read_varint(buffer, buffer_len, offset, count)) return false;

    parts.clear();
    parts.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        std::string part;
        if (!read_string(buffer, buffer_len, offset, part)) return false;
        parts.push_back(std::move(part));
    }
    return true;
}

// ---------------------------------------------------------------------------
// KVP Parameters
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_params(const std::vector<KvpParam>& params) {
    std::vector<uint8_t> result;
    append_varint(result, static_cast<uint64_t>(params.size()));
    for (const auto& p : params) {
        append_varint(result, p.key);
        if (p.key & 1) {
            // Odd key → length-prefixed bytes
            append_varint(result, static_cast<uint64_t>(p.bytes_value.size()));
            result.insert(result.end(), p.bytes_value.begin(), p.bytes_value.end());
        } else {
            // Even key → bare varint
            append_varint(result, p.int_value);
        }
    }
    return result;
}

bool decode_params(const uint8_t* buffer, int32_t buffer_len,
                   int32_t& offset, std::vector<KvpParam>& params) {
    uint64_t count = 0;
    if (!read_varint(buffer, buffer_len, offset, count)) return false;

    params.clear();
    params.reserve(static_cast<size_t>(count));
    for (uint64_t i = 0; i < count; ++i) {
        KvpParam p;
        if (!read_varint(buffer, buffer_len, offset, p.key)) return false;

        if (p.key & 1) {
            // Odd key → length-prefixed bytes
            uint64_t len = 0;
            if (!read_varint(buffer, buffer_len, offset, len)) return false;
            if (offset + static_cast<int32_t>(len) > buffer_len) return false;
            p.bytes_value.assign(buffer + offset, buffer + offset + len);
            offset += static_cast<int32_t>(len);
        } else {
            // Even key → bare varint
            if (!read_varint(buffer, buffer_len, offset, p.int_value)) return false;
        }
        params.push_back(std::move(p));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Control Message Framing
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_control_message(MessageType type,
                                            const std::vector<uint8_t>& content) {
    std::vector<uint8_t> result;
    append_varint(result, static_cast<uint64_t>(type));
    uint16_t len = static_cast<uint16_t>(content.size());
    result.push_back(static_cast<uint8_t>(len >> 8));
    result.push_back(static_cast<uint8_t>(len & 0xFF));
    result.insert(result.end(), content.begin(), content.end());
    return result;
}

bool parse_control_message(const uint8_t* buffer, int32_t buffer_len,
                           MessageType& type, const uint8_t*& content,
                           int32_t& content_len, int32_t& consumed) {
    if (buffer_len < 3) return false;

    int32_t br = 0;
    uint64_t raw_type = decode_varint(buffer, buffer_len, br);
    if (br == 0) return false;

    int32_t header_size = br + 2;
    if (header_size > buffer_len) return false;

    uint16_t clen = static_cast<uint16_t>(
        (static_cast<uint16_t>(buffer[br]) << 8) |
        static_cast<uint16_t>(buffer[br + 1]));

    int32_t total = header_size + static_cast<int32_t>(clen);
    if (total > buffer_len) return false;

    type = static_cast<MessageType>(raw_type);
    content = buffer + header_size;
    content_len = static_cast<int32_t>(clen);
    consumed = total;
    return true;
}

// ---------------------------------------------------------------------------
// Object Datagram
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_object_datagram(
    uint64_t track_alias, uint64_t group_id, uint64_t object_id,
    uint8_t priority, const uint8_t* payload, int32_t payload_len) {
    std::vector<uint8_t> result;
    append_varint(result, kDatagramTypePlain);  // Type = 0x00
    append_varint(result, track_alias);
    append_varint(result, group_id);
    append_varint(result, object_id);
    result.push_back(priority);
    if (payload && payload_len > 0) {
        result.insert(result.end(), payload, payload + payload_len);
    }
    return result;
}

int32_t build_object_datagram_header(
    uint64_t track_alias, uint64_t group_id, uint64_t object_id,
    uint8_t priority, uint8_t* header_buffer) {
    int32_t offset = 0;
    offset += encode_varint(kDatagramTypePlain, header_buffer + offset);
    offset += encode_varint(track_alias, header_buffer + offset);
    offset += encode_varint(group_id, header_buffer + offset);
    offset += encode_varint(object_id, header_buffer + offset);
    header_buffer[offset] = priority;
    offset += 1;
    return offset;
}

bool parse_object_datagram(const uint8_t* data, int32_t len, ObjectDatagram& out) {
    int32_t offset = 0;
    int32_t br = 0;

    // Type: 0x00 = datagram, 0x01 = datagram+extensions
    uint64_t type = decode_varint(data + offset, len - offset, br);
    if (br == 0 || (type != 0x00 && type != 0x01)) return false;
    offset += br;

    // TrackAlias
    out.track_alias = decode_varint(data + offset, len - offset, br);
    if (br == 0) return false;
    offset += br;

    // GroupID
    out.group_id = decode_varint(data + offset, len - offset, br);
    if (br == 0) return false;
    offset += br;

    // ObjectID
    out.object_id = decode_varint(data + offset, len - offset, br);
    if (br == 0) return false;
    offset += br;

    // Priority (1 byte, NOT varint)
    if (offset >= len) return false;
    out.priority = data[offset];
    offset += 1;

    // If type == 0x01, skip extensions
    if (type == 0x01) {
        uint64_t ext_count = decode_varint(data + offset, len - offset, br);
        if (br == 0) return false;
        offset += br;
        for (uint64_t i = 0; i < ext_count; ++i) {
            // Key varint
            decode_varint(data + offset, len - offset, br);
            if (br == 0) return false;
            offset += br;
            // Value length varint + value bytes
            uint64_t val_len = decode_varint(data + offset, len - offset, br);
            if (br == 0) return false;
            offset += br;
            offset += static_cast<int32_t>(val_len);
            if (offset > len) return false;
        }
    }

    // Payload is the remainder
    out.payload = data + offset;
    out.payload_len = len - offset;
    return true;
}

// ---------------------------------------------------------------------------
// CLIENT_SETUP
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_client_setup(const ClientSetupConfig& config) {
    std::vector<uint8_t> content;

    // 1 supported version
    append_varint(content, 1);
    append_varint(content, config.version);

    // 3 parameters: Role, Path, MaxSubscribeId
    append_varint(content, 3);

    // Role = PubSub (0x03). Key 0x00 is even → value is bare varint
    append_varint(content, kParamKeyRole);
    append_varint(content, kRolePubSub);

    // Path. Key 0x01 is odd → length-prefixed bytes
    append_varint(content, kParamKeyPath);
    append_varint(content, static_cast<uint64_t>(config.path.size()));
    content.insert(content.end(),
                   reinterpret_cast<const uint8_t*>(config.path.data()),
                   reinterpret_cast<const uint8_t*>(config.path.data()) + config.path.size());

    // MaxSubscribeId. Key 0x02 is even → value is bare varint
    append_varint(content, kParamKeyMaxSubscribeId);
    append_varint(content, config.max_subscribe_id);

    return build_control_message(MessageType::ClientSetup, content);
}

// ---------------------------------------------------------------------------
// SERVER_SETUP
// ---------------------------------------------------------------------------

bool parse_server_setup(const uint8_t* content, int32_t content_len,
                        ServerSetupResult& result) {
    int32_t offset = 0;
    if (!read_varint(content, content_len, offset, result.version)) return false;
    if (!decode_params(content, content_len, offset, result.params)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// SUBSCRIBE
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_subscribe(const SubscribeConfig& config) {
    std::vector<uint8_t> content;

    append_varint(content, config.request_id);
    // NOTE: NO TrackAlias in SUBSCRIBE per draft-11 / moqtransport
    auto ns = encode_namespace(config.track_namespace);
    content.insert(content.end(), ns.begin(), ns.end());
    append_string_bytes(content, config.track_name);

    // SubscriberPriority (1 byte)
    content.push_back(config.priority);
    // GroupOrder (1 byte)
    content.push_back(config.group_order);
    // Forward (1 byte)
    content.push_back(config.forward);
    // FilterType (varint)
    append_varint(content, config.filter_type);

    // Parameters: auth (if present) followed by any extra_params.
    std::vector<KvpParam> params;
    params.reserve(1 + config.extra_params.size());
    if (!config.authorization.empty()) {
        KvpParam auth;
        auth.key = kParamKeyAuthToken;
        auth.bytes_value.assign(config.authorization.begin(),
                                config.authorization.end());
        params.push_back(std::move(auth));
    }
    for (const auto& p : config.extra_params) {
        params.push_back(p);
    }
    auto encoded_params = encode_params(params);
    content.insert(content.end(), encoded_params.begin(), encoded_params.end());

    return build_control_message(MessageType::Subscribe, content);
}

// ---------------------------------------------------------------------------
// Resume opID parameter helper
// ---------------------------------------------------------------------------

KvpParam make_resume_op_id_param(uint64_t op_id) {
    KvpParam p;
    p.key = kParamKeyResumeOpId;
    p.bytes_value.resize(8);
    for (int i = 7; i >= 0; --i) {
        p.bytes_value[i] = static_cast<uint8_t>(op_id & 0xFF);
        op_id >>= 8;
    }
    return p;
}

bool parse_subscribe(const uint8_t* content, int32_t content_len,
                     SubscribeResult& result) {
    int32_t offset = 0;

    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    // NO TrackAlias
    if (!decode_namespace(content, content_len, offset, result.track_namespace)) return false;
    if (!read_string(content, content_len, offset, result.track_name)) return false;
    if (!read_byte(content, content_len, offset, result.priority)) return false;
    if (!read_byte(content, content_len, offset, result.group_order)) return false;
    if (!read_byte(content, content_len, offset, result.forward)) return false;
    if (!read_varint(content, content_len, offset, result.filter_type)) return false;
    if (!decode_params(content, content_len, offset, result.params)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// SUBSCRIBE_OK
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_subscribe_ok(const SubscribeOkConfig& config) {
    std::vector<uint8_t> content;

    append_varint(content, config.request_id);
    append_varint(content, config.track_alias);
    append_varint(content, config.expires_ms);
    content.push_back(config.group_order);
    content.push_back(config.content_exists ? 1 : 0);

    if (config.content_exists) {
        append_varint(content, config.largest_group_id);
        append_varint(content, config.largest_object_id);
    }

    // 0 parameters
    append_varint(content, 0);

    return build_control_message(MessageType::SubscribeOk, content);
}

bool parse_subscribe_ok(const uint8_t* content, int32_t content_len,
                        SubscribeOkResult& result) {
    int32_t offset = 0;

    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    if (!read_varint(content, content_len, offset, result.track_alias)) return false;
    if (!read_varint(content, content_len, offset, result.expires_ms)) return false;
    if (!read_byte(content, content_len, offset, result.group_order)) return false;

    uint8_t ce = 0;
    if (!read_byte(content, content_len, offset, ce)) return false;
    result.content_exists = (ce != 0);

    if (result.content_exists) {
        if (!read_varint(content, content_len, offset, result.largest_group_id)) return false;
        if (!read_varint(content, content_len, offset, result.largest_object_id)) return false;
    }

    if (!decode_params(content, content_len, offset, result.params)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// SUBSCRIBE_ERROR
// ---------------------------------------------------------------------------

bool parse_subscribe_error(const uint8_t* content, int32_t content_len,
                           SubscribeErrorResult& result) {
    int32_t offset = 0;

    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    if (!read_varint(content, content_len, offset, result.error_code)) return false;
    if (!read_string(content, content_len, offset, result.reason)) return false;
    if (!read_varint(content, content_len, offset, result.track_alias)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// ANNOUNCE
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_announce(const AnnounceConfig& config) {
    std::vector<uint8_t> content;
    append_varint(content, config.request_id);
    auto ns = encode_namespace(config.track_namespace);
    content.insert(content.end(), ns.begin(), ns.end());
    append_varint(content, 0);  // 0 parameters
    return build_control_message(MessageType::Announce, content);
}

bool parse_announce(const uint8_t* content, int32_t content_len,
                    AnnounceResult& result) {
    int32_t offset = 0;
    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    if (!decode_namespace(content, content_len, offset, result.track_namespace)) return false;
    if (!decode_params(content, content_len, offset, result.params)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// ANNOUNCE_OK
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_announce_ok(uint64_t request_id) {
    std::vector<uint8_t> content;
    append_varint(content, request_id);
    return build_control_message(MessageType::AnnounceOk, content);
}

bool parse_announce_ok(const uint8_t* content, int32_t content_len,
                       AnnounceOkResult& result) {
    int32_t offset = 0;
    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    return true;
}

// ---------------------------------------------------------------------------
// SUBSCRIBE_ANNOUNCES
// ---------------------------------------------------------------------------

bool parse_subscribe_announces(const uint8_t* content, int32_t content_len,
                               SubscribeAnnouncesResult& result) {
    int32_t offset = 0;
    if (!read_varint(content, content_len, offset, result.request_id)) return false;
    if (!decode_namespace(content, content_len, offset, result.track_namespace)) return false;
    if (!decode_params(content, content_len, offset, result.params)) return false;
    return true;
}

std::vector<uint8_t> build_subscribe_announces_ok(uint64_t request_id) {
    std::vector<uint8_t> content;
    append_varint(content, request_id);
    return build_control_message(MessageType::SubscribeAnnouncesOk, content);
}

}  // namespace moq
}  // namespace panaudia
