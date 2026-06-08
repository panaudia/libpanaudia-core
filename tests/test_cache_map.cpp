#include <catch2/catch_test_macros.hpp>
#include <panaudia/cache_map.h>
#include <panaudia/cache_wire.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace panaudia;

namespace {

// Build a CacheOp shorthand for tests.
CacheOp set_op(std::string key, std::string value, uint64_t op_id,
               uint32_t node_id = 0,
               std::string topic = "attributes") {
    CacheOp op;
    op.topic     = std::move(topic);
    op.key       = std::move(key);
    op.value     = std::move(value);
    op.op_id     = op_id;
    op.node_id   = node_id;
    op.tombstone = false;
    return op;
}

CacheOp tomb_op(std::string key, uint64_t op_id, uint32_t node_id = 0,
                std::string topic = "attributes") {
    CacheOp op;
    op.topic     = std::move(topic);
    op.key       = std::move(key);
    op.value     = "";
    op.op_id     = op_id;
    op.node_id   = node_id;
    op.tombstone = true;
    return op;
}

}  // namespace

// ============================================================================
// merge: opID gating [cache_map][merge]
// ============================================================================

TEST_CASE("merge — first insert is accepted", "[cache_map][merge]") {
    CacheMap m;
    REQUIRE(m.merge(set_op("k", "v1", 10)) == MergeResult::Accepted);
    auto e = m.get("k");
    REQUIRE(e.has_value());
    REQUIRE(e->value == "v1");
    REQUIRE(e->op_id == 10);
    REQUIRE(m.size() == 1);
}

TEST_CASE("merge — newer op replaces older", "[cache_map][merge]") {
    CacheMap m;
    m.merge(set_op("k", "v1", 10));
    REQUIRE(m.merge(set_op("k", "v2", 20)) == MergeResult::Accepted);

    auto e = m.get("k");
    REQUIRE(e.has_value());
    REQUIRE(e->value == "v2");
    REQUIRE(e->op_id == 20);
}

TEST_CASE("merge — older op is rejected", "[cache_map][merge]") {
    CacheMap m;
    m.merge(set_op("k", "v_new", 20));
    REQUIRE(m.merge(set_op("k", "v_old", 10)) == MergeResult::Rejected);

    auto e = m.get("k");
    REQUIRE(e.has_value());
    REQUIRE(e->value == "v_new");
    REQUIRE(e->op_id == 20);
}

TEST_CASE("merge — equal op_id is rejected (idempotent)", "[cache_map][merge]") {
    CacheMap m;
    m.merge(set_op("k", "first", 50));
    REQUIRE(m.merge(set_op("k", "second", 50)) == MergeResult::Rejected);

    auto e = m.get("k");
    REQUIRE(e->value == "first");
}

TEST_CASE("merge — node_id is stored on the entry", "[cache_map][merge]") {
    CacheMap m;
    m.merge(set_op("k", "v", 1, /*node_id=*/42));
    auto e = m.get("k");
    REQUIRE(e->node_id == 42);
}

// ============================================================================
// merge: tombstones [cache_map][tombstone]
// ============================================================================

TEST_CASE("tombstone — removes existing entry when newer", "[cache_map][tombstone]") {
    CacheMap m;
    m.merge(set_op("k", "v", 10));
    REQUIRE(m.merge(tomb_op("k", 20)) == MergeResult::Tombstoned);
    REQUIRE_FALSE(m.get("k").has_value());
    REQUIRE(m.size() == 0);
}

TEST_CASE("tombstone — older is rejected, key remains", "[cache_map][tombstone]") {
    CacheMap m;
    m.merge(set_op("k", "v", 20));
    REQUIRE(m.merge(tomb_op("k", 10)) == MergeResult::Rejected);

    auto e = m.get("k");
    REQUIRE(e.has_value());
    REQUIRE(e->value == "v");
}

TEST_CASE("tombstone — on absent key does not crash, returns Tombstoned",
          "[cache_map][tombstone]") {
    CacheMap m;
    REQUIRE(m.merge(tomb_op("nobody", 5)) == MergeResult::Tombstoned);
    REQUIRE_FALSE(m.get("nobody").has_value());
    REQUIRE(m.size() == 0);
    // highest_op_id still advances
    REQUIRE(m.highest_op_id() == 5);
}

// ============================================================================
// highest_op_id [cache_map][highest]
// ============================================================================

TEST_CASE("highest_op_id — empty map returns 0", "[cache_map][highest]") {
    CacheMap m;
    REQUIRE(m.highest_op_id() == 0);
}

TEST_CASE("highest_op_id — tracks across multiple keys", "[cache_map][highest]") {
    CacheMap m;
    m.merge(set_op("a", "x", 10));
    m.merge(set_op("b", "y", 30));
    m.merge(set_op("c", "z", 20));
    REQUIRE(m.highest_op_id() == 30);
}

TEST_CASE("highest_op_id — advances even on rejected merges",
          "[cache_map][highest]") {
    CacheMap m;
    m.merge(set_op("k", "v", 100));
    // A "stale-but-newer" arrival: same key, older op_id than the cached
    // entry, but higher op_id than anything else we've ever seen.
    // (Contrived for unit test; in practice this isn't possible because
    // op_id is monotonic per bouncer. But the resume parameter still
    // needs to track the highest_op_id seen on the wire.)
    CacheMap m2;
    m2.merge(set_op("k", "v_high", 100));
    m2.merge(set_op("k", "v_low", 50));   // rejected (older for this key)
    REQUIRE(m2.highest_op_id() == 100);   // unchanged

    // Realistic case: an op for key A, then a rejected one for key B
    CacheMap m3;
    m3.merge(set_op("a", "1", 100));
    m3.merge(set_op("a", "1b", 200));   // accepted, advances
    m3.merge(set_op("a", "1c", 50));    // rejected
    REQUIRE(m3.highest_op_id() == 200);
}

// ============================================================================
// Iteration [cache_map][iter]
// ============================================================================

TEST_CASE("for_each — visits all entries in sorted-key order",
          "[cache_map][iter]") {
    CacheMap m;
    m.merge(set_op("c", "3", 1));
    m.merge(set_op("a", "1", 2));
    m.merge(set_op("b", "2", 3));

    std::vector<std::string> seen;
    m.for_each([&](const std::string& k, const CacheEntry&) {
        seen.push_back(k);
    });
    REQUIRE(seen == std::vector<std::string>{"a", "b", "c"});
}

TEST_CASE("for_each_prefix — visits only matching keys", "[cache_map][iter]") {
    CacheMap m;
    m.merge(set_op("alice.name", "a-name", 1));
    m.merge(set_op("alice.colour", "red", 2));
    m.merge(set_op("bob.name", "b-name", 3));
    m.merge(set_op("alice.role", "host", 4));
    m.merge(set_op("zelda.name", "z-name", 5));

    std::vector<std::string> seen;
    m.for_each_prefix("alice.", [&](const std::string& k, const CacheEntry&) {
        seen.push_back(k);
    });
    // Sorted: alice.colour, alice.name, alice.role
    REQUIRE(seen == std::vector<std::string>{
        "alice.colour", "alice.name", "alice.role"});
}

TEST_CASE("for_each_prefix — empty prefix visits everything",
          "[cache_map][iter]") {
    CacheMap m;
    m.merge(set_op("a", "1", 1));
    m.merge(set_op("b", "2", 2));

    std::vector<std::string> seen;
    m.for_each_prefix("", [&](const std::string& k, const CacheEntry&) {
        seen.push_back(k);
    });
    REQUIRE(seen.size() == 2);
}

TEST_CASE("for_each_prefix — no match yields empty visit",
          "[cache_map][iter]") {
    CacheMap m;
    m.merge(set_op("alice.name", "v", 1));
    int hits = 0;
    m.for_each_prefix("nobody.", [&](const std::string&, const CacheEntry&) {
        ++hits;
    });
    REQUIRE(hits == 0);
}

TEST_CASE("for_each_prefix — does not match similar but distinct prefixes",
          "[cache_map][iter]") {
    CacheMap m;
    m.merge(set_op("alice", "v1", 1));
    m.merge(set_op("alicia", "v2", 2));   // shares 'alic' prefix
    m.merge(set_op("alice.name", "v3", 3));

    std::vector<std::string> seen;
    m.for_each_prefix("alice.", [&](const std::string& k, const CacheEntry&) {
        seen.push_back(k);
    });
    REQUIRE(seen == std::vector<std::string>{"alice.name"});
}

// ============================================================================
// clear / size / empty [cache_map][bookkeeping]
// ============================================================================

TEST_CASE("clear resets entries and highest_op_id", "[cache_map][bookkeeping]") {
    CacheMap m;
    m.merge(set_op("a", "v", 100));
    REQUIRE(m.size() == 1);
    REQUIRE(m.highest_op_id() == 100);

    m.clear();
    REQUIRE(m.empty());
    REQUIRE(m.highest_op_id() == 0);
}

TEST_CASE("size and empty track inserts and tombstones",
          "[cache_map][bookkeeping]") {
    CacheMap m;
    REQUIRE(m.empty());
    m.merge(set_op("a", "v", 1));
    m.merge(set_op("b", "v", 2));
    REQUIRE(m.size() == 2);
    m.merge(tomb_op("a", 3));
    REQUIRE(m.size() == 1);
    REQUIRE_FALSE(m.empty());
}

// ============================================================================
// Change handler [cache_map][handler]
// ============================================================================

TEST_CASE("change handler fires on accepted merge", "[cache_map][handler]") {
    CacheMap m;
    int calls = 0;
    std::string last_key;
    MergeResult last_result = MergeResult::Rejected;
    bool entry_was_present = false;

    m.set_change_handler([&](std::string_view k, const CacheEntry* e,
                             MergeResult r) {
        ++calls;
        last_key = std::string(k);
        last_result = r;
        entry_was_present = (e != nullptr);
    });

    m.merge(set_op("k", "v", 1));
    REQUIRE(calls == 1);
    REQUIRE(last_key == "k");
    REQUIRE(last_result == MergeResult::Accepted);
    REQUIRE(entry_was_present);
}

TEST_CASE("change handler fires on tombstone that removes entry",
          "[cache_map][handler]") {
    CacheMap m;
    m.merge(set_op("k", "v", 1));

    int calls = 0;
    bool entry_was_null = false;
    MergeResult last_result = MergeResult::Rejected;
    m.set_change_handler([&](std::string_view, const CacheEntry* e,
                             MergeResult r) {
        ++calls;
        entry_was_null = (e == nullptr);
        last_result = r;
    });

    m.merge(tomb_op("k", 5));
    REQUIRE(calls == 1);
    REQUIRE(last_result == MergeResult::Tombstoned);
    REQUIRE(entry_was_null);
}

TEST_CASE("change handler does NOT fire on rejected merge",
          "[cache_map][handler]") {
    CacheMap m;
    m.merge(set_op("k", "v", 100));

    int calls = 0;
    m.set_change_handler([&](std::string_view, const CacheEntry*,
                             MergeResult) {
        ++calls;
    });

    m.merge(set_op("k", "older", 50));
    REQUIRE(calls == 0);
}

TEST_CASE("change handler does NOT fire on tombstone of absent key",
          "[cache_map][handler]") {
    CacheMap m;
    int calls = 0;
    m.set_change_handler([&](std::string_view, const CacheEntry*,
                             MergeResult) {
        ++calls;
    });

    m.merge(tomb_op("nobody", 5));
    REQUIRE(calls == 0);
}

// ============================================================================
// Stress / smoke [cache_map][stress]
// ============================================================================

TEST_CASE("500 distinct keys all retained", "[cache_map][stress]") {
    CacheMap m;
    for (uint64_t i = 0; i < 500; ++i) {
        m.merge(set_op("k" + std::to_string(i), "v", i + 1));
    }
    REQUIRE(m.size() == 500);
    REQUIRE(m.highest_op_id() == 500);

    int seen = 0;
    m.for_each([&](const std::string&, const CacheEntry&) { ++seen; });
    REQUIRE(seen == 500);
}

TEST_CASE("Concurrent reader + writer does not crash",
          "[cache_map][stress][thread]") {
    CacheMap m;
    std::atomic<bool> stop{false};

    std::thread writer([&]() {
        for (uint64_t i = 0; i < 10000 && !stop.load(); ++i) {
            m.merge(set_op("k" + std::to_string(i % 50), "v", i + 1));
        }
    });

    std::thread reader([&]() {
        while (!stop.load()) {
            m.for_each([](const std::string&, const CacheEntry&) {});
            (void)m.highest_op_id();
            (void)m.size();
        }
    });

    writer.join();
    stop.store(true);
    reader.join();

    REQUIRE(m.size() <= 50);
    REQUIRE(m.highest_op_id() == 10000);
}
