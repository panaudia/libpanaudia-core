# Plan: extract the attribute/topic cache out of the transport core

> **Created 2026-06-08.** Restores `libpanaudia-core` to its stated contract
> (pure transport, opaque data tracks, msquic+libopus only — `plan/plan.md`
> §7/§17/§106/§287) by moving the WIP attribute-cache stack into its own
> library, consumed by the host. Makes C++ consistent with Go
> (`spatial-mixer/core/statecache`) and TS (`panaudia-client/.../src/shared`),
> which already keep this logic outside their transports.
>
> **Phased — wait for "start phase N" between phases.** No code moves yet.

## Why

The WIP added JSON parsing, opID-gated merge, tombstones, and cache-resume
*into the pure-transport core* (new dep: nlohmann/json). That contradicts the
core's written contract and creates an inconsistency: the **state** track
(NodeInfo3) is decoded host-side, but the **attributes** track (cache envelope)
got pulled into the core. They're the same kind of thing — app payload formats
on opaque data tracks — and belong at the same layer.

The good news: the cache code is **already fully decoupled** from the transport.
`cache_wire` / `cache_map` / `topic_merger` take bytes in and produce values out,
with no session/MOQ dependency. The coupling is only **three call sites** in the
core, listed below. So this is a *relocation*, not a rewrite.

## The new library — `panaudia-statecache`

A separate CMake **target** in this same repo (not a new repo — mirrors how TS
keeps it in `src/shared/` within the same package). The **core target stays
JSON-free**; the statecache target owns the JSON dep.

- **Contents (moved verbatim):** `cache_wire.{h,cpp}`, `cache_map.{h,cpp}`,
  `topic_merger.{h,cpp}` + their tests. Plus the `0xFF01` ResumeHLC constant and
  `make_resume_op_id_param()` (an app convention — `0xFF01` is Panaudia-specific).
- **Dependencies:** nlohmann/json only. **No msquic, no libopus, no transport.**
  Pure logic: envelope bytes in → `MergeBatchResult` + `resume_op_id()` out.
- **Naming:** C++ peer of Go `statecache` / TS `shared/cache-*`. (Name negotiable.)

## Core reverts to pure transport

Three coupling points to remove (`src/session_manager.cpp`), plus the public API:

1. **`:182`** — `handle->merger = make_unique<TopicMerger>()` → **delete**. No
   merger on `TrackHandle`. `attributes_output` becomes a plain opaque inbound
   Data track.
2. **`:592–594`** — auto-inject `make_resume_op_id_param(merger->resume_op_id())`
   → **replace** with a generic host seam (below). The core must not compute
   resume state.
3. **`:955` `dispatch_data_datagram`** — the `if (track->merger) apply_envelope…`
   branch → **delete**. All inbound data-track payloads go to `data_recv_callback`
   as raw bytes (exactly like NodeInfo3 on the state track).

Public API removed from `core.h`: `CacheValueView`, `CacheValuesCallback`,
`CacheRemovedCallback`, the cache callbacks/ctx on `SessionConfig`,
`TrackConfig::cached`, and `PanaudiaCore::get_cache_map()` (drop the
`panaudia_core.cpp` facade).

**Kept (transport-neutral):** generic `KvpParam`, `encode_params`/`decode_params`,
and `SubscribeConfig::extra_params` (opaque KVPs the host asks to attach).

### New generic seam — host-supplied subscribe params

Replaces the cache-aware auto-resume. The resume param must advance on every
(re)subscribe, so the host needs a hook the core calls at subscribe time:

```cpp
// SessionConfig: invoked by the core just before each (re)SUBSCRIBE for a track.
// The host appends opaque KVP params (e.g. the resume param from its merger).
// The core stays unaware of what they mean.
using SubscribeParamsCallback =
    void (*)(TrackHandle* track, std::vector<KvpParam>& out, void* ctx);
SubscribeParamsCallback subscribe_params_callback = nullptr;
void*                    subscribe_params_ctx      = nullptr;
```

At `:592`, the core calls this (if set) and appends the result to
`sub.extra_params`. Transport-neutral: the core asks the host for opaque params.

## Host wiring — Panaudia Unreal plugin

`panaudia-client/sdks/unreal/panaudia` (the plugin that already links the core):

- **Build.cs:** also link `panaudia-statecache` + its include dir.
- **`ConfigureCore`:** `attributes_output` becomes a plain `Data` inbound track
  (drop `cached=true`). Register `subscribe_params_callback`.
- **Own a `TopicMerger`** (per cached topic) in the component. On reconnect,
  reuse its `CacheMap` (the borrowing ctor) so resume state survives.
- **`PanaudiaCoreDataReceived_Impl`** (the raw data callback): for the attributes
  track, feed bytes to `merger->apply_envelope`. On a value result → fire
  `OnAttributeValuesChanged` / `OnAttributesRemoved` (the existing delegates).
  `std::nullopt` → treat as raw (existing fallback). This replaces the old
  `cache_values_callback` / `cache_removed_callback` path.
- **`GetAttribute` / `GetAttributesForNode` / `GetAllAttributes`:** read from the
  plugin's `merger` cache instead of `core->get_cache_map()`.
- **`subscribe_params_callback`:** append `make_resume_op_id_param(
  merger->resume_op_id())` (helper now from statecache).

`PanaudiaPresence` is unaffected (it already consumes `OnDataTrackReceived` /
delegates; no core link).

## Tests

The cache tests move with the lib (already standalone — `cache_wire`,
`cache_map`, `topic_merger`). The core test suite drops its cache assertions.
Add a plugin-side smoke test if practical.

## Sequencing

0. **Preserve the WIP** — commit the current uncommitted cache work on a branch
   first, so the relocation is a clean follow-on (and nothing is lost).
1. Create the `panaudia-statecache` target; move the 3 files + tests + JSON dep
   + the `0xFF01` helper. Build it standalone.
2. Revert the core wiring (3 call sites + public API), add the
   `SubscribeParamsCallback` seam. Core target builds JSON-free; tests pass.
3. Wire the Unreal plugin to statecache; rebuild the plugin.
4. Build + test everything (core, statecache, plugin).

**Then resume the draft-16 Phase 3 C++ port** on the now-clean core — it edits
`moq_protocol.{h,cpp}`, which is smaller once the cache wiring is gone, and the
dirty working tree is resolved.

## Open / to confirm during execution

- Final library name (`panaudia-statecache`?).
- Whether the Swift/macOS client wiring gets stubbed now or deferred (it links
  the same statecache target when it lands).
- Whether `make_resume_op_id_param` lives in statecache (app convention) or the
  host writes the 8-byte KvpParam inline.
