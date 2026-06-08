#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace panaudia {
namespace moq {

// ---------------------------------------------------------------------------
// Constants — IETF MOQ draft-16, as emitted by Eyevinn/moqtransport (our
// server). See spatial-mixer/plan/moq-draft14/wire-delta-16.md for the
// draft-11 -> draft-16 delta and golden/draft16-vectors.json for byte-exact
// fixtures. Key draft-16 changes: KVP params are delta-encoded; SUBSCRIBE /
// SUBSCRIBE_OK move several fields into parameters; CLIENT_SETUP drops the
// version list + ROLE; SERVER_SETUP has no version field. The auth token
// stays raw JWT bytes (Eyevinn does not use the spec Token struct). The
// OBJECT_DATAGRAM and ANNOUNCE wire formats are unchanged.
// ---------------------------------------------------------------------------

constexpr uint64_t kMoqVersion = 0xff000010;  // draft-16
constexpr const char* kMoqAlpn = "moqt-16";

// Control message types (sent on bidirectional control stream)
enum class MessageType : uint64_t {
    Subscribe            = 0x03,
    SubscribeOk          = 0x04,
    SubscribeError       = 0x05,
    Announce             = 0x06,
    AnnounceOk           = 0x07,
    AnnounceError        = 0x08,
    SubscribeAnnounces   = 0x11,
    SubscribeAnnouncesOk = 0x12,
    ClientSetup          = 0x20,
    ServerSetup          = 0x21,
};

// Datagram type field (separate from control MessageType — different QUIC context)
constexpr uint64_t kDatagramTypePlain      = 0x00;
constexpr uint64_t kDatagramTypeExtensions = 0x01;

// Setup parameter keys
constexpr uint64_t kParamKeyPath           = 0x01;  // odd  → length-prefixed bytes (QUIC only)
constexpr uint64_t kParamKeyMaxSubscribeId = 0x02;  // even → bare varint
constexpr uint64_t kParamKeyAuthToken      = 0x03;  // odd  → length-prefixed bytes

// draft-16 SUBSCRIBE parameter keys (fields moved out of the message body)
constexpr uint64_t kParamKeyForward            = 0x10;  // even → varint (bool)
constexpr uint64_t kParamKeySubscriberPriority = 0x20;  // even → varint
constexpr uint64_t kParamKeySubscriptionFilter = 0x21;  // odd  → bytes [filterType][start?][endGroup?]
constexpr uint64_t kParamKeyGroupOrder         = 0x22;  // even → varint

// draft-16 SUBSCRIBE_OK parameter keys
constexpr uint64_t kParamKeyExpires       = 0x08;  // even → varint (ms)
constexpr uint64_t kParamKeyLargestObject = 0x09;  // odd  → bytes (Location: group, object)

// Application-specific SUBSCRIBE parameters (e.g. the cache-resume key
// 0xFF01) are NOT defined here — the core treats them as opaque KvpParams
// the host supplies via SubscribeParam. See panaudia-statecache.

// Role values
constexpr uint64_t kRolePublisher  = 0x01;
constexpr uint64_t kRoleSubscriber = 0x02;
constexpr uint64_t kRolePubSub     = 0x03;

// Filter types
constexpr uint64_t kFilterLatestGroup  = 0x01;
constexpr uint64_t kFilterLatestObject = 0x02;

// ---------------------------------------------------------------------------
// Varint — RFC 9000 Section 16
// ---------------------------------------------------------------------------

// Encode value into buffer. Returns bytes written (1, 2, 4, or 8).
// Buffer must have at least 8 bytes available.
int32_t encode_varint(uint64_t value, uint8_t* buffer);

// Decode varint from buffer. Sets bytes_read to number of bytes consumed,
// or 0 on error (insufficient data). Returns decoded value.
uint64_t decode_varint(const uint8_t* buffer, int32_t buffer_len,
                       int32_t& bytes_read);

// Returns encoding size for value (1, 2, 4, or 8).
int32_t varint_size(uint64_t value);

// ---------------------------------------------------------------------------
// Namespace Tuple — [Count varint][Len varint, UTF-8 bytes]...
// ---------------------------------------------------------------------------

std::vector<uint8_t> encode_namespace(const std::vector<std::string>& parts);

// Decode namespace from buffer at offset. Advances offset past consumed bytes.
// Returns false on parse error.
bool decode_namespace(const uint8_t* buffer, int32_t buffer_len,
                      int32_t& offset, std::vector<std::string>& parts);

// ---------------------------------------------------------------------------
// KVP Parameters — even keys: bare varint, odd keys: length-prefixed bytes
// ---------------------------------------------------------------------------

struct KvpParam {
    uint64_t key = 0;
    uint64_t int_value = 0;                // used for even keys
    std::vector<uint8_t> bytes_value;      // used for odd keys
};

std::vector<uint8_t> encode_params(const std::vector<KvpParam>& params);

// Decode params from buffer at offset. Advances offset. Returns false on error.
bool decode_params(const uint8_t* buffer, int32_t buffer_len,
                   int32_t& offset, std::vector<KvpParam>& params);

// ---------------------------------------------------------------------------
// Control Message Framing — [Type varint][Length 2-byte BE][Content]
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_control_message(MessageType type,
                                            const std::vector<uint8_t>& content);

// Parse one framed control message from buffer.
// On success: sets type, content pointer (into buffer), content_len, consumed.
// Returns false if buffer is incomplete or malformed.
bool parse_control_message(const uint8_t* buffer, int32_t buffer_len,
                           MessageType& type, const uint8_t*& content,
                           int32_t& content_len, int32_t& consumed);

// ---------------------------------------------------------------------------
// Object Datagram
// [Type 0x00 varint][TrackAlias varint][GroupID varint][ObjectID varint]
// [Priority 1 BYTE][Payload...]
// ---------------------------------------------------------------------------

struct ObjectDatagram {
    uint64_t track_alias = 0;
    uint64_t group_id = 0;
    uint64_t object_id = 0;
    uint8_t priority = 0;
    const uint8_t* payload = nullptr;   // points into original buffer (zero-copy)
    int32_t payload_len = 0;
};

// Build complete datagram (allocates).
std::vector<uint8_t> build_object_datagram(
    uint64_t track_alias, uint64_t group_id, uint64_t object_id,
    uint8_t priority, const uint8_t* payload, int32_t payload_len);

// Zero-copy: writes header only into pre-allocated buffer, returns header length.
// Buffer must have at least 34 bytes. Caller appends payload separately.
int32_t build_object_datagram_header(
    uint64_t track_alias, uint64_t group_id, uint64_t object_id,
    uint8_t priority, uint8_t* header_buffer);

// Parse datagram. Payload pointer is zero-copy into original data buffer.
bool parse_object_datagram(const uint8_t* data, int32_t len, ObjectDatagram& out);

// ---------------------------------------------------------------------------
// CLIENT_SETUP builder
// ---------------------------------------------------------------------------

struct ClientSetupConfig {
    uint64_t version = kMoqVersion;
    std::string path = "/";
    uint64_t max_subscribe_id = 100;
};

std::vector<uint8_t> build_client_setup(const ClientSetupConfig& config = {});

// ---------------------------------------------------------------------------
// SERVER_SETUP parser
// ---------------------------------------------------------------------------

struct ServerSetupResult {
    uint64_t version = 0;
    std::vector<KvpParam> params;
};

bool parse_server_setup(const uint8_t* content, int32_t content_len,
                        ServerSetupResult& result);

// ---------------------------------------------------------------------------
// SUBSCRIBE builder & parser
// ---------------------------------------------------------------------------

struct SubscribeConfig {
    uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::string track_name;
    uint8_t priority = 128;
    uint8_t group_order = 0;
    uint8_t forward = 0;
    uint64_t filter_type = kFilterLatestGroup;
    std::string authorization;          // empty = no auth param
    std::vector<KvpParam> extra_params; // appended after the auth param
};

std::vector<uint8_t> build_subscribe(const SubscribeConfig& config);

struct SubscribeResult {
    uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::string track_name;
    uint8_t priority = 0;
    uint8_t group_order = 0;
    uint8_t forward = 0;
    uint64_t filter_type = 0;
    std::vector<KvpParam> params;
};

bool parse_subscribe(const uint8_t* content, int32_t content_len,
                     SubscribeResult& result);

// ---------------------------------------------------------------------------
// SUBSCRIBE_OK builder & parser
// ---------------------------------------------------------------------------

struct SubscribeOkConfig {
    uint64_t request_id = 0;
    uint64_t track_alias = 0;
    uint64_t expires_ms = 0;       // 0 = never
    uint8_t group_order = 0;
    bool content_exists = false;
    uint64_t largest_group_id = 0;   // only if content_exists
    uint64_t largest_object_id = 0;  // only if content_exists
};

std::vector<uint8_t> build_subscribe_ok(const SubscribeOkConfig& config);

struct SubscribeOkResult {
    uint64_t request_id = 0;
    uint64_t track_alias = 0;
    uint64_t expires_ms = 0;
    uint8_t group_order = 0;
    bool content_exists = false;
    uint64_t largest_group_id = 0;
    uint64_t largest_object_id = 0;
    std::vector<KvpParam> params;
};

bool parse_subscribe_ok(const uint8_t* content, int32_t content_len,
                        SubscribeOkResult& result);

// ---------------------------------------------------------------------------
// SUBSCRIBE_ERROR parser
// ---------------------------------------------------------------------------

struct SubscribeErrorResult {
    uint64_t request_id = 0;
    uint64_t error_code = 0;
    std::string reason;
    uint64_t track_alias = 0;
};

bool parse_subscribe_error(const uint8_t* content, int32_t content_len,
                           SubscribeErrorResult& result);

// ---------------------------------------------------------------------------
// ANNOUNCE builder & parser
// ---------------------------------------------------------------------------

struct AnnounceConfig {
    uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
};

std::vector<uint8_t> build_announce(const AnnounceConfig& config);

struct AnnounceResult {
    uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::vector<KvpParam> params;
};

bool parse_announce(const uint8_t* content, int32_t content_len,
                    AnnounceResult& result);

// ---------------------------------------------------------------------------
// ANNOUNCE_OK builder & parser
// ---------------------------------------------------------------------------

std::vector<uint8_t> build_announce_ok(uint64_t request_id);

struct AnnounceOkResult {
    uint64_t request_id = 0;
};

bool parse_announce_ok(const uint8_t* content, int32_t content_len,
                       AnnounceOkResult& result);

// ---------------------------------------------------------------------------
// SUBSCRIBE_ANNOUNCES parser & OK builder
// ---------------------------------------------------------------------------

struct SubscribeAnnouncesResult {
    uint64_t request_id = 0;
    std::vector<std::string> track_namespace;
    std::vector<KvpParam> params;
};

bool parse_subscribe_announces(const uint8_t* content, int32_t content_len,
                               SubscribeAnnouncesResult& result);

std::vector<uint8_t> build_subscribe_announces_ok(uint64_t request_id);

}  // namespace moq
}  // namespace panaudia
