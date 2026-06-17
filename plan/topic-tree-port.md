# Plan: port the topic-tree / cache-subscriber layer into `panaudia-statecache`

> **Created 2026-06-17. DEFERRED — not scheduled. Capturing the design while
> it's fresh; other work comes first.** No code moves yet.
>
> Brings the C++ `panaudia-statecache` library up to parity with the TS client
> (`panaudia-client/sdks/typescript/src/shared`), which keeps the
> flat→nested reconstruction and the subscriber glue *in the SDK*. C++ today
> stops one layer short — it ships the merge primitives (`cache_wire`,
> `cache_map`, `topic_merger`) but not the tree or the subscriber, so every
> host re-implements them by hand.

## Why

After the statecache extraction ([`extract-statecache.md`](extract-statecache.md))
the C++ side has three building blocks: `cache_wire` (decode), `cache_map`
(opID-gated flat merge), `topic_merger` (decode + per-key merge →
`MergeBatchResult`). The TS client has the same three (`cache-wire`,
`cache-map`, `topic-merger`) **plus** two layers the C++ side lacks:

| Layer | TS | C++ today |
|---|---|---|
| envelope decode | `cache-wire.ts` | `cache_wire` ✅ |
| opID merge → flat map | `cache-map.ts` + `topic-merger.ts` | `cache_map` + `topic_merger` ✅ |
| flat `{uuid}.field` → nested per-uuid object | `topic-tree.ts` | **missing** |
| rootless variant (space topic) | `single-record-tree.ts` | **missing** |
| subscriber glue (merge + dispatch handlers) | `cache-topic-subscriber.ts` | **missing** |

The consequence showed up concretely: the Unreal `PanaudiaPresenceComponent`
now hand-rolls the flat→nested reconstruction in
`HandleAttributeValues` / `SerializeNodeAttributes`
(`panaudia-client/sdks/unreal/panaudia-presence/.../PanaudiaPresenceComponent.cpp`).
That logic is a direct re-implementation of `topic-tree.ts` and belongs in the
shared library, where it can be tested once and reused by every C++ host
(Unreal, macOS/Swift, future native clients).

This is parity work, **not** a bug fix — the merge path already works end to
end. Defer freely.

## What to add — three new pairs in `panaudia-statecache`

All in the statecache target, which already depends on `nlohmann/json`. The
transport core (`panaudia-core`) stays untouched and JSON-free.

### 1. `TopicTree` — port of `topic-tree.ts`

Nested per-uuid view of a flat-key topic. Backed by `nlohmann::json`.

```cpp
class TopicTree {
  std::set<std::string> apply_values(const std::vector<TopicValue>&);  // affected uuids
  struct RemovedResult { std::set<std::string> updated, removed; };
  RemovedResult         apply_removed(const std::vector<std::string>&);
  const nlohmann::json* get(std::string_view uuid) const;             // nested record
  const std::map<std::string, nlohmann::json>& get_all() const;
  size_t size() const;  void clear();
private:
  std::map<std::string, nlohmann::json> records_;   // uuid -> nested object
};
```

Key splits on `.`; first segment is the uuid. Match the TS atomicity guarantee:
build a new uuid's record off to the side and insert it only once fully
populated, so a concurrent reader never observes a half-built record.

### 2. `SingleRecordTree` — port of `single-record-tree.ts`

For the `space` topic (rootless keys: `roles-muted.{role}`, `roles-kicked.{role}`,
…). A single `nlohmann::json` record, same dotted-path build/remove as
`TopicTree` but keyed at the root.

### 3. `CacheTopicSubscriber` — port of `cache-topic-subscriber.ts`

Ties `TopicMerger` (+ optional `TopicTree`) + dispatch together so a host wires
one object instead of hand-coding the merge/dispatch block.

```cpp
class CacheTopicSubscriber {
  explicit CacheTopicSubscriber(std::shared_ptr<CacheMap> = nullptr, bool build_tree = true);
  void on_datagram(const uint8_t* data, size_t len);   // host calls from data_recv_callback
  void on_values(ValuesHandler);    void on_removed(RemovedHandler);
  void on_tree_change(TreeChangeHandler);  void on_tree_remove(TreeRemoveHandler);
  uint64_t resume_op_id() const;    // host returns from subscribe_params_callback
  const TopicTree& tree() const;    std::optional<CacheEntry> get(std::string_view) const;
};
```

### Unavoidable difference from TS

In TS the subscriber lives *inside* the transport SDK, so it auto-registers a
datagram handler. In C++, `panaudia-core` is a separate transport-only library,
so the host still wires the two one-line seams it wires today:
`data_recv_callback → subscriber.on_datagram` and
`subscribe_params_callback → subscriber.resume_op_id()`. The subscriber removes
the merge/dispatch/marshalling boilerplate in between — not those two hooks.

## Host wiring impact — Unreal plugin

Once ported, `PanaudiaPresenceComponent` (and/or `PanaudiaAudioComponent`) can
drop the hand-rolled reconstruction:

- Replace `NodeAttributes` + `HandleAttributeValues` + `SerializeNodeAttributes`
  + the dotted-path walk with a `CacheTopicSubscriber` (or a bare `TopicTree`
  fed from the existing `OnAttributeValuesChanged` path).
- `GetAttributesForNode` can return the nested record from `tree().get(uuid)`
  instead of prefix-walking the flat `CacheMap`.

This is the cleanup the minimal client fix
([client fix commit / `PanaudiaPresenceComponent.cpp`]) deliberately did *not*
do — it kept the reconstruction in the plugin so the fix stayed small.

## Tests

Mirror the TS `topic-tree` / `cache-map` test coverage: nested build, atomic
insert of new uuids, leaf removal with empty-intermediate cleanup, batch
atomicity, and the space-topic rootless variant. Share fixtures with TS where
practical (the per-key op shape is identical across languages).

## Sequencing (when picked up)

1. Add `TopicTree` + `SingleRecordTree` + their tests; build statecache standalone.
2. Add `CacheTopicSubscriber` + tests.
3. Migrate the Unreal plugin to the new types; rebuild the plugin; verify in
   `unreal_test` (two clients, remote participants spawn).
4. Update the plugin docs (`panaudia/docs.md`, `panaudia-presence/README.md`) to
   point at the library types instead of the hand-rolled reconstruction.

## Open / to confirm during execution

- Whether hosts want push (`on_*` handlers) or pull (`tree()`) as the primary
  API — TS exposes both; C++ can too.
- Node type: `nlohmann::json` (matches TS object semantics) vs a typed struct.
  `nlohmann::json` is the low-friction choice and the dep is already present.
- Thread model: handlers fire on the core's datagram thread; document that hosts
  marshal to their own thread (Unreal `AsyncTask`), as the plugin does today.
