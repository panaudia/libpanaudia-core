#pragma once

#include "panaudia/cache_map.h"
#include "panaudia/cache_wire.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace panaudia {

// Cache-resume SUBSCRIBE parameter convention (Panaudia-specific, shared
// across hosts and languages — see spatial-mixer/CLAUDE.md). The host reads
// resume_op_id() from its merger and attaches {kResumeParamKey,
// encode_resume_op_id(op_id)} as an opaque SubscribeParam. The transport
// core stays unaware of what this means.
inline constexpr uint64_t kResumeParamKey = 0xFF01;  // odd → length-prefixed bytes

// 8-byte big-endian encoding of an op_id for the resume parameter value.
std::vector<uint8_t> encode_resume_op_id(uint64_t op_id);

// One key-value pair surfaced after a successful merge. `value` is the
// JSON-serialised representation of the inner op's `value` field —
// "\"alice\"", "42", "true", "null". Tombstones never appear here.
struct TopicValue {
    std::string key;
    std::string value;
};

// Per-envelope outcome. Preserves batch atomicity — if an envelope
// carried a JSON array of N ops, all N appear in this single result
// (those with newer op_ids than the cache, anyway).
struct MergeBatchResult {
    std::vector<TopicValue>  accepted;
    std::vector<std::string> tombstoned;
};

// Optional diagnostic snapshot — fired once per apply_envelope call,
// after the merge, regardless of accept/reject outcome. Lets callers
// distinguish "envelope arrived but every op was stale" from "no
// envelope arrived". Mirrors TS MergeDebugInfo.
struct MergeDebugInfo {
    std::string topic;
    uint64_t    op_id = 0;
    size_t      op_count = 0;
    std::vector<std::string> accepted_keys;
    std::vector<std::string> tombstoned_keys;
    std::vector<std::string> rejected_keys;
};

using MergeDebugHandler = std::function<void(const MergeDebugInfo&)>;

struct TopicMergerStats {
    uint64_t updates_received = 0;  // accepted + tombstoned per-key ops
    uint64_t errors_dropped   = 0;  // bad envelopes / unparseable JSON
    size_t   entry_count      = 0;
};

// TopicMerger — decode a binary cache envelope and merge each op inside
// it into a CacheMap.
//
// Mirrors panaudia-client/sdks/typescript/src/shared/topic-merger.ts.
// All methods are thread-safe via the underlying CacheMap.
class TopicMerger {
public:
    // Owns its own CacheMap.
    TopicMerger();

    // Borrows a caller-supplied CacheMap. Use this to share resume state
    // across subscriber lifetimes (tear down the merger on disconnect,
    // build a fresh one with the same CacheMap on reconnect).
    explicit TopicMerger(std::shared_ptr<CacheMap> cache);

    ~TopicMerger();

    TopicMerger(const TopicMerger&) = delete;
    TopicMerger& operator=(const TopicMerger&) = delete;

    // Decode envelope, parse inner JSON ops (single `{...}` or batch
    // `[...]`), merge each, return what beat the opID gate. Returns
    // std::nullopt if the payload was not a valid envelope or its JSON
    // could not be parsed; the caller should fall back to delivering
    // raw payload as before (matches TS behaviour).
    std::optional<MergeBatchResult>
    apply_envelope(const uint8_t* data, size_t len);

    // Convenience read-throughs.
    std::optional<CacheEntry> get(std::string_view key) const;
    void for_each(const CacheVisitor& visitor) const;
    void for_each_prefix(std::string_view prefix,
                         const CacheVisitor& visitor) const;

    // Resume parameter for the next SUBSCRIBE on this topic.
    uint64_t resume_op_id() const;

    TopicMergerStats stats() const;

    // Direct access to the underlying CacheMap (e.g. to share across
    // reconnects via the borrowing constructor).
    std::shared_ptr<CacheMap> cache() const { return cache_; }

    // Optional per-envelope debug handler. Pass nullptr to clear.
    void set_debug_handler(MergeDebugHandler handler);

private:
    std::shared_ptr<CacheMap> cache_;
    MergeDebugHandler         debug_handler_;
    std::atomic<uint64_t>     updates_received_{0};
    std::atomic<uint64_t>     errors_dropped_{0};
};

}  // namespace panaudia
