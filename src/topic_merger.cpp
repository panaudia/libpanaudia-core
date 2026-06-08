#include "panaudia/topic_merger.h"

#include <nlohmann/json.hpp>

#include <utility>

namespace panaudia {

std::vector<uint8_t> encode_resume_op_id(uint64_t op_id) {
    std::vector<uint8_t> bytes(8);
    for (int i = 7; i >= 0; --i) {
        bytes[i] = static_cast<uint8_t>(op_id & 0xFF);
        op_id >>= 8;
    }
    return bytes;
}

namespace {

using json = nlohmann::json;

// Parse an inner op (JSON object) and merge it into the cache. Pushes
// into the right output bucket and returns the per-key MergeResult so
// the caller can also build the debug-info "rejected" list.
MergeResult process_one_op(const json& jop,
                           const CacheOp& envelope_template,
                           CacheMap& cache,
                           MergeBatchResult& out_result,
                           std::string& out_key /*moved into bucket on success*/) {
    out_key.clear();

    if (!jop.is_object()) return MergeResult::Rejected;

    auto key_it = jop.find("key");
    if (key_it == jop.end() || !key_it->is_string()) return MergeResult::Rejected;
    std::string key = key_it->get<std::string>();
    if (key.empty()) return MergeResult::Rejected;

    bool tomb = false;
    auto tomb_it = jop.find("tombstone");
    if (tomb_it != jop.end() && tomb_it->is_boolean()) {
        tomb = tomb_it->get<bool>();
    }

    std::string value_str;
    if (!tomb) {
        auto val_it = jop.find("value");
        if (val_it != jop.end()) {
            // .dump() re-serialises the value as canonical JSON. This
            // mirrors the TS code that calls JSON.stringify(jsonOp.value)
            // before storing — strings keep their quotes, numbers stay
            // numeric, etc.
            value_str = val_it->dump();
        }
    }

    CacheOp op = envelope_template;
    op.key       = key;
    op.value     = value_str;
    op.tombstone = tomb;

    const auto r = cache.merge(op);

    switch (r) {
        case MergeResult::Accepted:
            out_result.accepted.push_back(
                TopicValue{std::move(key), std::move(value_str)});
            break;
        case MergeResult::Tombstoned:
            // Only surface tombstones the cache actually applied.
            // The cache returns Tombstoned even for absent keys; we only
            // want to report keys that were really removed. Detect via
            // post-merge presence — but actually the cleaner signal is
            // "did the key exist before?". We don't have that here without
            // an extra cache method, so fall back to reporting all
            // tombstones that beat the op_id gate. The host can choose to
            // ignore removals for keys it never knew about.
            //
            // (Matches the TS subscriber behaviour: it also surfaces
            // tombstones that beat the gate, regardless of prior presence.)
            out_result.tombstoned.push_back(std::move(key));
            break;
        case MergeResult::Rejected:
            // Caller adds to debug-only rejected list via out_key.
            out_key = std::move(key);
            break;
    }
    return r;
}

}  // namespace

TopicMerger::TopicMerger()
    : cache_(std::make_shared<CacheMap>()) {}

TopicMerger::TopicMerger(std::shared_ptr<CacheMap> cache)
    : cache_(std::move(cache)) {
    if (!cache_) cache_ = std::make_shared<CacheMap>();
}

TopicMerger::~TopicMerger() = default;

std::optional<MergeBatchResult>
TopicMerger::apply_envelope(const uint8_t* data, size_t len) {
    if (!is_cache_envelope(data, len)) {
        errors_dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    auto envelope = decode_cache_op(data, len);
    if (!envelope) {
        errors_dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    // Parse the inner JSON. parse(..., nullptr, /*allow_exceptions=*/false)
    // returns a 'discarded' JSON value rather than throwing.
    auto j = json::parse(envelope->value.begin(), envelope->value.end(),
                         /*cb=*/nullptr,
                         /*allow_exceptions=*/false);
    if (j.is_discarded()) {
        errors_dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    MergeBatchResult result;
    std::vector<std::string> rejected_keys;

    // Build a CacheOp template once per envelope; per-op fields (key,
    // value, tombstone) are filled in process_one_op.
    CacheOp op_template;
    op_template.topic   = envelope->topic;
    op_template.op_id   = envelope->op_id;
    op_template.node_id = envelope->node_id;

    auto handle = [&](const json& jop) {
        std::string rk;
        const auto r = process_one_op(jop, op_template, *cache_, result, rk);
        if (r == MergeResult::Rejected && !rk.empty()) {
            rejected_keys.push_back(std::move(rk));
        } else if (r == MergeResult::Accepted || r == MergeResult::Tombstoned) {
            updates_received_.fetch_add(1, std::memory_order_relaxed);
        }
    };

    size_t op_count = 0;
    if (j.is_array()) {
        op_count = j.size();
        for (const auto& jop : j) handle(jop);
    } else if (j.is_object()) {
        op_count = 1;
        handle(j);
    } else {
        errors_dropped_.fetch_add(1, std::memory_order_relaxed);
        return std::nullopt;
    }

    if (debug_handler_) {
        MergeDebugInfo info;
        info.topic    = envelope->topic;
        info.op_id    = envelope->op_id;
        info.op_count = op_count;
        info.accepted_keys.reserve(result.accepted.size());
        for (const auto& v : result.accepted) info.accepted_keys.push_back(v.key);
        info.tombstoned_keys = result.tombstoned;  // copy
        info.rejected_keys   = std::move(rejected_keys);
        debug_handler_(info);
    }

    return result;
}

std::optional<CacheEntry> TopicMerger::get(std::string_view key) const {
    return cache_->get(key);
}

void TopicMerger::for_each(const CacheVisitor& visitor) const {
    cache_->for_each(visitor);
}

void TopicMerger::for_each_prefix(std::string_view prefix,
                                  const CacheVisitor& visitor) const {
    cache_->for_each_prefix(prefix, visitor);
}

uint64_t TopicMerger::resume_op_id() const {
    return cache_->highest_op_id();
}

TopicMergerStats TopicMerger::stats() const {
    TopicMergerStats s;
    s.updates_received = updates_received_.load(std::memory_order_relaxed);
    s.errors_dropped   = errors_dropped_.load(std::memory_order_relaxed);
    s.entry_count      = cache_->size();
    return s;
}

void TopicMerger::set_debug_handler(MergeDebugHandler handler) {
    debug_handler_ = std::move(handler);
}

}  // namespace panaudia
