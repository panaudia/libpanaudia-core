#include <catch2/catch_test_macros.hpp>
#include <panaudia/cache_map.h>
#include <panaudia/cache_wire.h>
#include <panaudia/topic_merger.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

using namespace panaudia;

namespace {

// Encode an envelope wrapping the given JSON payload.
std::vector<uint8_t> envelope(const std::string& topic,
                              uint64_t op_id,
                              uint32_t node_id,
                              bool tombstone_flag,
                              const std::string& json_payload) {
    CacheOp op;
    op.topic     = topic;
    // The wire envelope's KEY field doesn't carry the per-key key here —
    // when batches arrive the per-op keys live inside the JSON. The Go
    // bouncer uses an empty top-level key for batches; we mirror that.
    op.key       = "";
    op.value     = json_payload;
    op.op_id     = op_id;
    op.node_id   = node_id;
    op.tombstone = tombstone_flag;
    return encode_cache_op(op);
}

// Convenience for tests that only care about a single op envelope.
std::vector<uint8_t> single_set_envelope(uint64_t op_id,
                                         const std::string& key,
                                         const std::string& json_value_literal) {
    const std::string payload =
        std::string("{\"key\":\"") + key + "\",\"value\":" + json_value_literal + "}";
    return envelope("attributes", op_id, /*node_id=*/0, /*tomb=*/false, payload);
}

std::vector<uint8_t> single_tombstone_envelope(uint64_t op_id,
                                               const std::string& key) {
    const std::string payload =
        std::string("{\"key\":\"") + key + "\",\"tombstone\":true}";
    return envelope("attributes", op_id, /*node_id=*/0, /*tomb=*/false, payload);
}

}  // namespace

// ============================================================================
// Single-op envelopes [merger][single]
// ============================================================================

TEST_CASE("Single set op produces one accepted value", "[merger][single]") {
    TopicMerger m;
    const auto wire = single_set_envelope(10, "alice.name", "\"alice\"");

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.size() == 1);
    REQUIRE(r->accepted[0].key == "alice.name");
    REQUIRE(r->accepted[0].value == "\"alice\"");
    REQUIRE(r->tombstoned.empty());

    REQUIRE(m.resume_op_id() == 10);
    auto stored = m.get("alice.name");
    REQUIRE(stored.has_value());
    REQUIRE(stored->value == "\"alice\"");
}

TEST_CASE("Single set op with numeric value", "[merger][single]") {
    TopicMerger m;
    const auto wire = single_set_envelope(20, "alice.score", "42");

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.size() == 1);
    REQUIRE(r->accepted[0].value == "42");
}

TEST_CASE("Single set op with boolean value", "[merger][single]") {
    TopicMerger m;
    const auto wire = single_set_envelope(30, "alice.muted", "true");

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted[0].value == "true");
}

TEST_CASE("Single set op with null value", "[merger][single]") {
    TopicMerger m;
    const auto wire = single_set_envelope(40, "alice.x", "null");

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted[0].value == "null");
}

TEST_CASE("Single tombstone op surfaces in `tombstoned`", "[merger][single]") {
    TopicMerger m;
    // Set first, then tombstone.
    {
        const auto w = single_set_envelope(10, "alice.name", "\"alice\"");
        m.apply_envelope(w.data(), w.size());
    }
    const auto wire = single_tombstone_envelope(20, "alice.name");
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.empty());
    REQUIRE(r->tombstoned == std::vector<std::string>{"alice.name"});
    REQUIRE_FALSE(m.get("alice.name").has_value());
}

// ============================================================================
// Batch envelopes [merger][batch]
// ============================================================================

TEST_CASE("Batch envelope yields accepted values in order", "[merger][batch]") {
    TopicMerger m;
    const std::string payload =
        R"([{"key":"a.name","value":"alice"},)"
        R"({"key":"a.colour","value":"red"},)"
        R"({"key":"a.score","value":42}])";
    const auto wire = envelope("attributes", 100, 0, false, payload);

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.size() == 3);
    REQUIRE(r->accepted[0].key == "a.name");
    REQUIRE(r->accepted[0].value == "\"alice\"");
    REQUIRE(r->accepted[1].key == "a.colour");
    REQUIRE(r->accepted[1].value == "\"red\"");
    REQUIRE(r->accepted[2].key == "a.score");
    REQUIRE(r->accepted[2].value == "42");

    REQUIRE(m.resume_op_id() == 100);
    REQUIRE(m.stats().entry_count == 3);
}

TEST_CASE("Batch with mixed sets and tombstones", "[merger][batch]") {
    TopicMerger m;
    {
        const std::string p =
            R"([{"key":"a.name","value":"alice"},{"key":"a.colour","value":"red"}])";
        const auto w = envelope("attributes", 50, 0, false, p);
        m.apply_envelope(w.data(), w.size());
    }
    const std::string payload =
        R"([{"key":"a.name","value":"ALICE"},)"
        R"({"key":"a.colour","tombstone":true}])";
    const auto wire = envelope("attributes", 100, 0, false, payload);

    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.size() == 1);
    REQUIRE(r->accepted[0].key == "a.name");
    REQUIRE(r->accepted[0].value == "\"ALICE\"");
    REQUIRE(r->tombstoned == std::vector<std::string>{"a.colour"});
}

TEST_CASE("Stale batch — none of its ops beat the cache", "[merger][batch]") {
    TopicMerger m;
    {
        const std::string p = R"({"key":"a.name","value":"alice"})";
        const auto w = envelope("attributes", 100, 0, false, p);
        m.apply_envelope(w.data(), w.size());
    }
    // Same op_id (rejected per per-key gate), but for a different key it
    // would still be accepted. Use op_id strictly less for the same key.
    const std::string payload = R"({"key":"a.name","value":"older"})";
    const auto wire = envelope("attributes", 50, 0, false, payload);
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.empty());
    REQUIRE(r->tombstoned.empty());
    // Cache value unchanged
    REQUIRE(m.get("a.name")->value == "\"alice\"");
}

// ============================================================================
// Backward compatibility / error handling [merger][errors]
// ============================================================================

TEST_CASE("Non-envelope payload returns nullopt", "[merger][errors]") {
    TopicMerger m;
    const std::string raw_json = R"({"name":"alice"})";
    auto r = m.apply_envelope(
        reinterpret_cast<const uint8_t*>(raw_json.data()), raw_json.size());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(m.stats().errors_dropped == 1);
}

TEST_CASE("Envelope wrapping invalid JSON returns nullopt", "[merger][errors]") {
    TopicMerger m;
    const auto wire = envelope("attributes", 10, 0, false, "{\"key\":\"k\",unfinished");
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(m.stats().errors_dropped == 1);
}

TEST_CASE("Envelope wrapping JSON that is not object/array — nullopt",
          "[merger][errors]") {
    TopicMerger m;
    const auto wire = envelope("attributes", 10, 0, false, "42");
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE_FALSE(r.has_value());
    REQUIRE(m.stats().errors_dropped == 1);
}

TEST_CASE("Op missing the `key` field is silently dropped", "[merger][errors]") {
    TopicMerger m;
    const auto wire =
        envelope("attributes", 10, 0, false, R"({"value":"orphan"})");
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.empty());
    REQUIRE(r->tombstoned.empty());
}

TEST_CASE("Empty key string is silently dropped", "[merger][errors]") {
    TopicMerger m;
    const auto wire =
        envelope("attributes", 10, 0, false, R"({"key":"","value":"x"})");
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(r->accepted.empty());
}

// ============================================================================
// Resume opID + reconnect simulation [merger][resume]
// ============================================================================

TEST_CASE("resume_op_id tracks the highest envelope opID seen",
          "[merger][resume]") {
    TopicMerger m;
    const auto w1 = single_set_envelope(10, "a", "1");
    const auto w2 = single_set_envelope(30, "b", "2");
    const auto w3 = single_set_envelope(20, "c", "3");
    m.apply_envelope(w1.data(), w1.size());
    m.apply_envelope(w2.data(), w2.size());
    m.apply_envelope(w3.data(), w3.size());
    REQUIRE(m.resume_op_id() == 30);
}

TEST_CASE("Sharing CacheMap across merger lifetimes preserves resume",
          "[merger][resume]") {
    auto shared = std::make_shared<CacheMap>();
    {
        TopicMerger first(shared);
        const auto w = single_set_envelope(100, "k", "\"v\"");
        first.apply_envelope(w.data(), w.size());
        REQUIRE(first.resume_op_id() == 100);
    }
    // first is destroyed; shared CacheMap survives
    TopicMerger reconnect(shared);
    REQUIRE(reconnect.resume_op_id() == 100);
    REQUIRE(reconnect.get("k").has_value());
}

// ============================================================================
// Debug handler [merger][debug]
// ============================================================================

TEST_CASE("Debug handler reports accepted/tombstoned/rejected per envelope",
          "[merger][debug]") {
    TopicMerger m;
    // Seed: a.name = alice (op 100)
    {
        const auto w = single_set_envelope(100, "a.name", "\"alice\"");
        m.apply_envelope(w.data(), w.size());
    }

    MergeDebugInfo captured;
    bool fired = false;
    m.set_debug_handler([&](const MergeDebugInfo& info) {
        captured = info;
        fired = true;
    });

    // Envelope op 200: one accepted (new key), one rejected (older for
    // existing key), one tombstone for an absent key.
    const std::string payload =
        R"([{"key":"a.colour","value":"red"},)"
        R"({"key":"a.name","value":"older"},)"
        R"({"key":"a.gone","tombstone":true}])";
    // For the rejected op, the per-op opID gate uses the envelope opID =
    // 200 vs cached a.name op 100, so the new op (200) actually wins.
    // Use an envelope op_id that's lower than the cached one to force a
    // rejection.
    const auto wire = envelope("attributes", 50, 0, false, payload);
    auto r = m.apply_envelope(wire.data(), wire.size());
    REQUIRE(r.has_value());
    REQUIRE(fired);

    REQUIRE(captured.op_id == 50);
    REQUIRE(captured.op_count == 3);
    REQUIRE(captured.topic == "attributes");

    // a.colour: new key, op 50 — ACCEPTED
    REQUIRE(captured.accepted_keys == std::vector<std::string>{"a.colour"});
    // a.name: cached op 100 > envelope op 50 → REJECTED
    REQUIRE(captured.rejected_keys == std::vector<std::string>{"a.name"});
    // a.gone: tombstone with op 50, no prior entry — surfaces as
    // tombstoned (matches TS behaviour)
    REQUIRE(captured.tombstoned_keys == std::vector<std::string>{"a.gone"});
}
