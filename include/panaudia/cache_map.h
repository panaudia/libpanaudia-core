#pragma once

#include "panaudia/cache_wire.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>

namespace panaudia {

// Flat per-topic key→value map with opID-gated merge.
//
// Mirrors panaudia-client/sdks/typescript/src/shared/cache-map.ts.
// One CacheMap per cached topic (attributes, entity, space, ...).
//
// Threading model:
//   - Single writer: the datagram-receive thread calls merge().
//   - Many readers: the host (game thread, render thread, ...) calls
//     get / for_each / for_each_prefix / highest_op_id.
//   - Locking is provided here via std::shared_mutex — host need not wrap.
//   - Callbacks (change_handler, for_each visitors) run with the lock
//     held. They MUST NOT call back into this CacheMap or they will
//     deadlock. Marshal to another thread if you need to do real work.

struct CacheEntry {
    std::string value;
    uint64_t    op_id = 0;
    uint32_t    node_id = 0;
};

enum class MergeResult {
    Accepted,    // value stored (new key or higher op_id)
    Tombstoned,  // tombstone applied (key removed if it existed)
    Rejected,    // op_id was equal-or-lower than the cached entry
};

// Fired after each successful merge call (Accepted, or Tombstoned that
// actually removed an entry). Not fired on Rejected. On Tombstoned the
// entry pointer is null. The pointer is only valid for the duration of
// the callback — copy if you need to outlive it.
using CacheChangeHandler =
    std::function<void(std::string_view key,
                       const CacheEntry* entry,
                       MergeResult result)>;

// Visitor signature shared by for_each / for_each_prefix.
using CacheVisitor =
    std::function<void(const std::string& key, const CacheEntry& entry)>;

class CacheMap {
public:
    CacheMap();
    ~CacheMap();

    CacheMap(const CacheMap&) = delete;
    CacheMap& operator=(const CacheMap&) = delete;

    // === Writer side (datagram thread) ===

    // Apply an op. Highest op_id per key wins; tombstones erase.
    // Updates highest_op_id even when the per-key op_id is rejected,
    // so the resume parameter still advances correctly.
    MergeResult merge(const CacheOp& op);

    // === Reader side (host threads) ===

    // Returns a copy of the entry, if present.
    std::optional<CacheEntry> get(std::string_view key) const;

    // Visit every entry in sorted-key order. Lock is held throughout —
    // keep the visitor cheap.
    void for_each(const CacheVisitor& visitor) const;

    // Visit every entry whose key starts with `prefix`. Useful for
    // "all attributes for node X" — pass the node UUID + '.' as prefix.
    // O(log N + k) by walking from lower_bound(prefix).
    void for_each_prefix(std::string_view prefix,
                         const CacheVisitor& visitor) const;

    // Highest op_id ever observed. Pass to the server in the SUBSCRIBE
    // resume parameter (kParamKeyResumeOpId) on reconnect.
    uint64_t highest_op_id() const;

    // Number of entries.
    size_t size() const;
    bool empty() const;

    // Drop all entries and reset highest_op_id to 0.
    void clear();

    // Install a change handler. Pass nullptr to clear. Fires synchronously
    // inside merge() with the write lock held — see threading note above.
    void set_change_handler(CacheChangeHandler handler);

private:
    mutable std::shared_mutex          mutex_;
    std::map<std::string, CacheEntry>  entries_;
    uint64_t                           highest_op_id_ = 0;
    CacheChangeHandler                 change_handler_;
};

}  // namespace panaudia
