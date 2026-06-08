#include "panaudia/cache_map.h"

#include <utility>

namespace panaudia {

CacheMap::CacheMap()  = default;
CacheMap::~CacheMap() = default;

MergeResult CacheMap::merge(const CacheOp& op) {
    std::unique_lock lock(mutex_);

    // Highest opID always advances, regardless of accept/reject. The
    // resume parameter must reflect everything we've SEEN, not just
    // everything we've STORED — otherwise a stale-but-newer envelope
    // arriving after a fresh one would force redundant backfill.
    if (op.op_id > highest_op_id_) {
        highest_op_id_ = op.op_id;
    }

    auto it = entries_.find(op.key);
    if (it != entries_.end() && op.op_id <= it->second.op_id) {
        return MergeResult::Rejected;
    }

    if (op.tombstone) {
        if (it != entries_.end()) {
            entries_.erase(it);
            if (change_handler_) {
                change_handler_(op.key, nullptr, MergeResult::Tombstoned);
            }
        }
        return MergeResult::Tombstoned;
    }

    CacheEntry entry;
    entry.value   = op.value;
    entry.op_id   = op.op_id;
    entry.node_id = op.node_id;

    const CacheEntry* stored = nullptr;
    if (it != entries_.end()) {
        it->second = std::move(entry);
        stored = &it->second;
    } else {
        auto [ins_it, inserted] = entries_.emplace(op.key, std::move(entry));
        (void)inserted;
        stored = &ins_it->second;
    }

    if (change_handler_) {
        change_handler_(op.key, stored, MergeResult::Accepted);
    }
    return MergeResult::Accepted;
}

std::optional<CacheEntry> CacheMap::get(std::string_view key) const {
    std::shared_lock lock(mutex_);
    // std::map::find with string_view requires C++14 transparent comparator
    // or an exact std::string. Cheapest portable path: construct a string.
    auto it = entries_.find(std::string(key));
    if (it == entries_.end()) return std::nullopt;
    return it->second;
}

void CacheMap::for_each(const CacheVisitor& visitor) const {
    if (!visitor) return;
    std::shared_lock lock(mutex_);
    for (const auto& [k, v] : entries_) {
        visitor(k, v);
    }
}

void CacheMap::for_each_prefix(std::string_view prefix,
                               const CacheVisitor& visitor) const {
    if (!visitor) return;
    std::shared_lock lock(mutex_);

    if (prefix.empty()) {
        for (const auto& [k, v] : entries_) visitor(k, v);
        return;
    }

    // lower_bound jumps directly to the first key >= prefix; walk from
    // there while the prefix still matches. O(log N + k).
    const std::string prefix_str(prefix);
    auto it = entries_.lower_bound(prefix_str);
    for (; it != entries_.end(); ++it) {
        const std::string& k = it->first;
        if (k.size() < prefix.size()) break;
        if (k.compare(0, prefix.size(), prefix) != 0) break;
        visitor(k, it->second);
    }
}

uint64_t CacheMap::highest_op_id() const {
    std::shared_lock lock(mutex_);
    return highest_op_id_;
}

size_t CacheMap::size() const {
    std::shared_lock lock(mutex_);
    return entries_.size();
}

bool CacheMap::empty() const {
    std::shared_lock lock(mutex_);
    return entries_.empty();
}

void CacheMap::clear() {
    std::unique_lock lock(mutex_);
    entries_.clear();
    highest_op_id_ = 0;
}

void CacheMap::set_change_handler(CacheChangeHandler handler) {
    std::unique_lock lock(mutex_);
    change_handler_ = std::move(handler);
}

}  // namespace panaudia
