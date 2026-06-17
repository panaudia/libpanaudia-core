# Jitter buffer v3 port — `libpanaudia-core`

**Status:** proposed (not started)
**Owner:** —
**Last updated:** 2026-06-16

Port the C++ inbound-audio jitter buffer (`include/panaudia/jitter_buffer.h`,
`src/jitter_buffer.cpp`) from the **legacy** Go `CircularBuffer` design it
currently mirrors to the **v3 asymmetric adaptive** design that the Go server
(`core/buffers/jitter_buffer.go`) and the TypeScript client
(`panaudia-client/.../moq/jitter-buffer-core.ts`) now ship. This brings all
three native/host implementations onto one algorithm and gives every host
(Unreal plugin, macOS CoreAudio HAL, Linux PortAudio) the drift correction and
adaptive latency that only the Go and browser clients have today.

> Naming note: the file is `unreal-buffer-update.md` because Unreal is the
> immediate motivation, but the change is **host-agnostic** — it lives entirely
> in `panaudia-core` and benefits every host that links it.

---

## 0. Resolved decisions & immediate findings (2026-06-16)

Decisions taken in review (resolve the original §8 open questions):

1. **`SessionConfig` fields will be renamed, not reused** (Q1). The v3 semantics
   differ enough (`jitter_buffer_max_ms` was an overrun ceiling; the v3 analog is
   a *latency* ceiling) that silently changing meaning under the old names would
   mislead callers. Rename to the v3 `low_*` / `high_*` / `writer_frame_ms` /
   `reader_frame_ms` / `safety_ms` shape. This is a deliberate breaking change to
   the public config; call it out in the changelog. (Updates §4.3.)
2. **Drop the `JitterBufferState` enum** (Q3). v3 has no FILLING/PLAYING — only
   `started = (rp > 0)`. Remove the enum entirely; `get_stats()` exposes
   `started` (bool) instead of `state`. Update the two read sites
   (`session_manager.cpp`, the tests). (Updates §4.1, §4.4.)

**ROC finding (was Q4) — no fix needed; the premise was wrong.** Investigation of
the Go tree shows ROC input is **already on v3**:

- `directroc/roc_track_input.go:18` constructs `buffers.NewJitterBuffer(...)`
  with `WriterFrameSize: 10ms, ReaderFrameSize: 5ms, LowInit: 10ms` — exactly the
  design-doc ROC config. So ROC already uses the same v3 buffer as the MOQ and
  WebRTC inputs (`direct/backend.go:601` also uses `NewJitterBuffer`).
- The only stale artifact is the doc comment at `core/buffers/circular_buffer.go`
  (~line 34) that still claims `CircularBufferA` is *"Used by directroc/ for ROC
  track input."* That is outdated — directroc no longer uses it. **Action: fix
  that comment** (tiny Go-side doc cleanup; not a behavioural change).
- Note: ROC lives in the **Go server** (`panaudia/directroc`, "Panaudia Link"),
  not in `libpanaudia-core` — the C++ transport core has no ROC path. So there is
  nothing ROC-related to port into C++.

**Carry-over task for the morning:**
- [ ] One-line Go doc fix: correct the `CircularBufferA` "used by directroc"
      comment in `core/buffers/circular_buffer.go`.

---

## 1. Why

The current C++ `JitterBuffer` is a faithful port of the **legacy**
`core/buffers/circular_buffer.go`, which is explicitly marked in the Go tree as
*"Legacy; new callers should use NewJitterBuffer."* It is two design generations
behind:

- It predates the **v2 anti-crackle** revision (mid-band snap; floor that pads
  the late side).
- It predates the entire **v3 adaptive-window** rework (the fixed floor + two
  independent allowances `L`/`H`, driven by counting the buffer's own ±1
  corrections).

Concrete deficits in the current C++ buffer, all fixed by v3:

| # | Issue in current C++ | v3 resolution |
|---|---|---|
| 1 | **Fixed** `[targetLow, targetHigh]` window — no adaptation to live jitter. | Fixed floor `R+S`; adaptive `L` (latency side) + `H` (capacity side). |
| 2 | **No clock-drift reconciliation** beyond hitting a bound and snapping (audible discontinuity ~every 17 min at 30 ppm). | Continuous ±1 splice every out-of-band read. |
| 3 | **Snaps to `target_centre`** on overrun — the pre-anti-crackle bug that starves the next 1–3 reads → click + underrun burst. | Snaps to `snapTarget = T + W` (a full writer frame of slack above target). |
| 4 | **Overflow path can corrupt the ring** (writer overwrites cells the RT reader may be mid-read on; reader only snaps on its *next* call — `jitter_buffer.cpp:43-60`). | Cumulative non-wrapping `writePos`/`readPos`; lap/overrun detected and snapped **by the reader**, never by the writer. |
| 5 | **Insert duplicates the last sample**; drop hard-skips. | Linear-interp splice: inserted/dropped frame is the per-channel average of the adjacent frames. |
| 6 | `correction_interval`-gated ±1 (a rate-ish cadence). | No cadence: the adaptive band is the jitter absorber; ±1 fires only on genuine drift. |
| 7 | Default constants (`60/20/16`) match neither legacy Go nor v3. | v3 frame-based geometry with documented starting constants. |

### Reference sources (the things this port must match)

- **Algorithm authority:** `core/buffers/jitter_buffer.go` (in this repo's sibling
  `panaudia/` Go tree). Port it line-for-line; only atomic syntax differs.
- **Design rationale:** `cloud-mixer/plan/history/jitter-buffer/design_v3.md`
  (LANDED 2026-06-04) and `plan_v3.md`.
- **Prior port to copy the shape of:** `jitter-buffer-core.ts` (already did this
  port for the browser; same integer-only, wall-clock-free structure).
- **Tests to mirror:** `core/buffers/jitter_buffer_test.go`.

> ⚠️ Do **not** port anything from `design.md`, `design_v1.md`, or
> `autonomous_tuning.md` — those describe superseded v1/v2 mechanisms (`M`
> sliding margin, excursion EMAs, debounce streaks, wall-clock decays) that v3
> deliberately deleted. **No wall-clock, no rates, no decay constants** anywhere
> in the adaptation path — this is a hard review invariant.

---

## 2. Goals / non-goals

**Goals**

- Replace the legacy state-machine buffer with the v3 adaptive jitter buffer,
  semantically identical to `jitter_buffer.go`.
- Preserve the SPSC contract (recv thread writes via `write()`, RT thread reads
  via `read()`) with explicit C++ memory ordering.
- Keep `PanaudiaCore`'s public surface working: `read_audio()`,
  `get_buffer_status()`, the inbound-audio wiring in `session_manager.cpp`.
- Mirror the Go test suite so behaviour is provably equivalent.

**Non-goals**

- No packet-loss concealment (PLC/NetEQ). v3 outputs silence on underrun — same
  as today; this is a known, accepted limitation.
- No change to the **send** path (`ring_buffer.{h,cpp}`). The capture ring is a
  dumb drain-to-empty SPSC pipe and must stay that way — it has the opposite job
  and must add zero discretionary latency.
- No change to MOQ/transport/opus code.
- No new host-side threading model — the C++ core is already SPSC, which is
  exactly the v3 contract.

---

## 3. The v3 algorithm (porting reference)

All quantities are **per-channel frames**. A *frame* is `nc` interleaved
samples; ±1 corrections move a whole frame so stereo L/R stay sample-locked
(ITD preserved).

### 3.1 Geometry (derived per-read from live `L`, `H`)

```
floor      = R + S                       // FIXED — never moves at runtime
T          = floor + L                   // operating target / sawtooth bottom
snapTarget = T + W                       // recovery point for EVERY snap
dropLine   = T + W + H                   // drift-DROP fires above this
overrunAt  = T + 2W + H                  // overrun snap fires above this

bandTopMax = R + S + Lmax + 2W + Hmax    // overrunAt at full adaptation
capacity   = 2*bandTopMax + 2*max(W, R)  // sized once, worst case
```

`W` = writer/codec frame, `R` = reader/callback frame, `S` = safety pad. `L`,`H`
are the only runtime-mutable quantities.

### 3.2 Read path — five branches, in order (port verbatim)

Load `L`,`H` **once** at the top so every branch sees consistent geometry.

1. **Startup** (`rp == 0`): if `fill < snapTarget` → silence + `adapt()` +
   return false; else set `rp = wp - snapTarget`, fall through and play.
2. **Lap** (`fill >= capacity`): snap `rp = wp - snapTarget`, `laps++`. Does
   **not** feed adaptation.
3. **Overrun** (`fill > overrunAt`): snap `rp = wp - snapTarget`, `overruns++`.
   Does **not** feed adaptation.
4. **Underrun** (`fill < nFrames`): silence, `underruns++`, `adapt()`, return
   false. `rp` unchanged, **no state reset**.
5. **Playing ± splice**: `corr=+1` (DROP) if `fill > dropLine && fill >=
   nFrames+1`; `corr=-1` (INSERT) if `fill < floor && nFrames >= 2`; else 0.
   Apply splice, advance `rp`, bump matching `dropCount`/`insertCount`,
   `adapt()`, return true.

Splice detail:
- **Drop** (`+1`): consume `nFrames+1`, output `nFrames`; last output frame =
  `(last_consumed + skipped) * 0.5` per channel.
- **Insert** (`-1`): consume `nFrames-1`, output `nFrames`; extra tail frame =
  `(last_consumed + peek_next) * 0.5` per channel (peeked frame stays in ring).

### 3.3 Adaptive controller (`adapt` / `decide`)

Tumbling window of `N` **reads** (a count, not a duration). Each read ticks
`readsThisWindow`; ±1 corrections bump `insertCount`/`dropCount`. Every `N`
reads, publish `lastWin*`, run `decide()`, reset the three counters.

```
decide(insertCount, dropCount):
  if min(insertCount, dropCount) >= widenThreshold:        // two-directional ⇒ JITTER
      if insertCount >= widenThreshold: L = min(L + widenStep, Lmax)   // eager up
      if dropCount   >= widenThreshold: H = min(H + widenStep, Hmax)
      return
  if insertCount == 0: L = max(L - narrowStep, Lmin)        // calm side ⇒ narrow
  if dropCount   == 0: H = max(H - narrowStep, Hmin)        // reluctant down
  // a side that is lit but un-gated is drift — left to the ±1 corrector
```

`narrowStep < widenStep` (eager-up / reluctant-down) is the stability guarantee.
One-directional signals (drift, outage) **never widen** — this is what prevents
the v2 positive-feedback ratchet.

### 3.4 Constants (v3 starting values, from `jitter_buffer.go`)

| Symbol | Value | Field |
|---|---|---|
| `S` safety | 1 ms | `Safety` |
| `L_init` warm start | 5 ms (MOQ) / 10 ms (WebRTC, ROC) | `LowInit` |
| `L_min` | 2 ms | `LowMin` |
| `L_max` | 30 ms | `LowMax` |
| `H_init` = `H_min` | `W` | `HighInit`/`HighMin` |
| `H_max` | `3·W` | `HighMax` |
| `W` writer frame | 20 ms default (MOQ); host passes codec frame ms | `WriterFrameSize` |
| `R` reader frame | 5 ms default; host passes its callback size | `ReaderFrameSize` |
| `N` window | 400 reads (≈2 s @ 5 ms) — **re-derive from C++ read cadence** | const |
| `widenThreshold` | 5 / side / window | const |
| `widenStep` | 2 ms | const |
| `narrowStep` | 0.5 ms (500 µs) | const |

> **`N` must be re-derived from the host's actual read cadence** to preserve the
> ~2 s recency horizon — the browser uses 750 (2 s ÷ 2.667 ms), Go uses 400 (2 s
> ÷ 5 ms). For the C++ core, derive `N` from `R` (the configured reader frame):
> `N = round(2_000_000 µs / R_µs)`, clamped to a sane floor. Document the choice.

---

## 4. C++ API & wiring changes

### 4.1 `include/panaudia/jitter_buffer.h`

Rewrite. Replace `JitterBufferConfig` (legacy `target/window/min/max/capacity/
correction_interval` ms fields) and the `JitterBufferState` enum with the v3
shape:

```cpp
struct JitterBufferConfig {
    uint32_t sample_rate    = 48000;
    uint32_t num_channels   = 1;
    uint32_t writer_frame_ms = 20;   // W — codec/server frame
    uint32_t reader_frame_ms = 5;    // R — host callback frame
    uint32_t safety_ms       = 1;    // S
    uint32_t low_init_ms     = 5;    // L warm start
    uint32_t low_min_ms      = 2;
    uint32_t low_max_ms      = 30;
    uint32_t high_init_ms    = 0;    // 0 ⇒ = W
    uint32_t high_min_ms     = 0;    // 0 ⇒ = W
    uint32_t high_max_ms     = 0;    // 0 ⇒ = 3*W
};
```

- Positions become **cumulative `int64_t`** (`std::atomic<int64_t> write_pos_`,
  `read_pos_`), never wrapping; index the ring with `(pos % capacity) * nc`.
- Replace `target_*`/`min_samples_`/`max_samples_` with `floor_`, `w_`,
  `l_min_..h_max_`, and a `levels(l, h)` helper returning
  `{t, snapTarget, dropLine, overrunAt}`.
- Add reader-owned controller state (`insert_count_`, `drop_count_`,
  `reads_this_window_`) and live `std::atomic<int64_t> current_l_/current_h_`.
- Keep `write()`/`read()` signatures unchanged (drop-in for `session_manager`).
- **Drop the `JitterBufferState` enum** (decision §0.2). v3 has no
  FILLING/PLAYING; `get_stats()` exposes `started` (`bool`, = `rp > 0`) instead
  of `state`. Remove the enum and update both read sites.
- `get_stats()` keeps returning a struct usable by `get_buffer_status()` — see
  §4.3. Add a richer `snapshot()` mirroring Go `JitterBufferStats` (floor, L, H,
  target, lastWindow inserts/drops) for observability.
- Update the file header comment (currently *"Port of Go CircularBuffer
  (core/buffers/circular_buffer.go)"*) to point at `jitter_buffer.go` /
  `design_v3.md`.

### 4.2 `src/jitter_buffer.cpp`

Reimplement `write`/`read`/`adapt`/`decide`/`levels`/`writeToRing`/
`readFromRing`/`fillFrames` as direct ports of the Go methods (§3). Delete the
legacy FILLING/PLAYING state machine, the `overflow_flag_` hack, and the
`correction_interval` gating.

### 4.3 `include/panaudia/core.h` — public `SessionConfig`

Current fields are legacy-shaped:

```cpp
uint32_t jitter_buffer_min_ms     = 10;
uint32_t jitter_buffer_max_ms     = 200;
uint32_t jitter_buffer_initial_ms = 60;
```

**Decided (§0.1): rename, don't reuse.** The legacy fields are removed and
replaced with the v3-shaped config (semantics differ; reusing the names would
mislead). New `SessionConfig` fields:

- `jitter_buffer_initial_ms` → **`jitter_low_init_ms`** (warm-start target
  latency). Default 5 (MOQ) — set 10 for WebRTC/ROC-style inputs.
- `jitter_buffer_min_ms` → **`jitter_low_min_ms`** (default 2).
- `jitter_buffer_max_ms` → **`jitter_low_max_ms`** — *latency* ceiling now, not
  the old overrun ceiling (default 30).
- Add **`reader_frame_ms`** (default 5; host sets it to its device buffer size —
  see §4.4) and optional `safety_ms` / high-side bounds (`0` ⇒ derived from `W`).
- `W` is **not** a `SessionConfig` field — it comes from the inbound track's
  codec frame (§4.4).

This is a deliberate breaking change to the public config; note it in the
changelog / `docs/guide.md`.

### 4.4 `src/session_manager.cpp`

- `~line 151-157`: populate the new `JitterBufferConfig`. Crucially the caller
  **knows `W`**: it is the inbound track's codec frame — `opus_frame_size_ms`
  for Opus, `pcm_frame_size_ms` for PCM (same fields already used for the send
  path at `:132-138`). Set `jb_cfg.writer_frame_ms` from the track config rather
  than defaulting to 20.
- `R` (reader frame) is the host's `read_audio` `frame_count`, not known at
  configure time. Default `reader_frame_ms` from `SessionConfig` (host should
  set it to its audio device buffer size; geometry is only approximate if it
  differs, and capacity is worst-cased so a mismatch is at worst a recoverable
  lap/overrun).
- `~line 996-1011`: `get_buffer_status()` reads `stats.fill_level_samples`,
  `underrun_count`, `overrun_count`. Keep these field names in the new
  `get_stats()` so this site is unchanged. The "approximate capacity from
  `jitter_buffer_max_ms`" line should instead report the real ring capacity (now
  exposed) or `dropLine`-based headroom.
- Any site reading `stats.state == JitterBufferState::Filling` switches to
  `!stats.started` (the enum is gone — decision §0.2).

### 4.5 Concurrency / memory ordering (C++ specifics)

Mirror the Go release/acquire contract:

- `write()`: `copy_to_ring(...)` then `write_pos_.store(wp + n,
  std::memory_order_release)`. Producer touches **only** `write_pos_` and the
  ring.
- `read()`: `write_pos_.load(std::memory_order_acquire)` pairs with the writer's
  release (guarantees ring data behind `wp` is visible).
- `read_pos_`: reader-owned; `store(release)` so a stats observer's
  `fillFrames()` (load `read_pos_` before `write_pos_`) never sees negative
  fill.
- `current_l_`/`current_h_`/`last_win_*`/stats counters: reader-written
  `std::atomic<int64_t>`, `relaxed` is sufficient (only observed by stats, no
  data dependency). Match Go, which uses plain atomics here.
- Controller counters (`insert_count_` etc.) are plain `int64_t` — reader-thread
  only, not shared.

---

## 5. Phased implementation plan

Mirrors the Go `plan_v3.md` phasing so each phase is independently reviewable and
testable.

### Phase 0 — Scaffolding
- [ ] Add this doc; link it from `README.md` / `plan/plan.md`.
- [ ] Snapshot current behaviour: run existing `test_jitter_buffer.cpp` +
      `bench_buffers.cpp`, record baseline numbers.

### Phase 1 — Geometry & construction
- [ ] New `JitterBufferConfig` + constructor: compute `floor`, `w`, `lMin..hMax`,
      `capacity`, controller constants; allocate `data_` = `capacity*nc`.
- [ ] `levels(l, h)` helper.
- [ ] Validation (assert/throw) on mis-ordered bounds & non-positive `W`/`R`,
      matching the Go panics.
- [ ] Unit test: geometry math at 48 kHz matches Go worked examples exactly.

### Phase 2 — Read/write path with a static window
- [ ] `write()`, `writeToRing()` (cumulative int64 + wrap).
- [ ] `read()` five-branch path + ±1 linear-interp splice; `adapt()` is a no-op
      stub but maintains `insert_count_`/`drop_count_`.
- [ ] `readFromRing()`, `fillFrames()`.
- [ ] Memory ordering per §4.5.
- [ ] Tests: startup gate, lap, overrun snap-to-`snapTarget`, underrun silence,
      drop/insert splices incl. stereo (`nc=2`) and across-wrap.

### Phase 3 — Adaptive controller
- [ ] `adapt()` tumbling window + `decide()` widen/narrow.
- [ ] Re-derive `N` from `reader_frame_ms` (§3.4 note).
- [ ] Tests: two-directional jitter widens both sides; one-directional drift
      narrows the calm side / leaves the lit side; eager-up/reluctant-down;
      bounds clamping; underrun/lap/overrun do **not** widen.

### Phase 4 — Stats & observability
- [ ] `get_stats()` (compat: `fill_level_samples`, `underrun_count`,
      `overrun_count`, `state`) + rich `snapshot()`.
- [ ] Confirm `session_manager.cpp:996-1011` compiles unchanged.

### Phase 5 — Integration swap & config
- [ ] Wire `session_manager.cpp:151-157` to pass real `W` from the track codec
      frame and `R` from `SessionConfig`.
- [ ] Apply the `SessionConfig` rename (§4.3) + update any in-tree callers.
- [ ] Build with `-tags`/CMake on macOS, Linux, Windows (CI matrix).

### Phase 6 — Test port & soak
- [ ] Rewrite `tests/test_jitter_buffer.cpp` to mirror `jitter_buffer_test.go`
      (drop the legacy FILLING/PLAYING tests).
- [ ] Run under TSan (`build-tsan/`) to validate the SPSC ordering.
- [ ] Update `bench_buffers.cpp` if it references removed fields; capture
      before/after latency + correction-rate numbers.
- [ ] Update `docs/guide.md` jitter-buffer section + the `README.md` JitterBuffer
      row.

---

## 6. Testing plan

Port these `jitter_buffer_test.go` cases (Catch2):

- Construction/geometry: defaults, custom bounds, validation failures.
- Startup: silence until `snapTarget`, then first audio.
- Steady state: no corrections when fill rides `[T, snapTarget]`.
- Drop splice (mono + stereo + across-wrap): output length, per-channel average,
  `rp` advanced by `nFrames+1`, `samplesDropped`/`dropCount`.
- Insert splice (mono + stereo): peek-average tail, `rp` advanced by
  `nFrames-1`, `samplesInserted`/`insertCount`.
- Lap & overrun: snap to `snapTarget`, counters, **no** adaptation.
- Underrun: silence, no `rp` move, no state reset, feeds only the next insert.
- Controller: two-directional widen (both sides), one-directional no-widen, calm
  narrow, clamping at min/max, eager/reluctant asymmetry.
- Concurrency: producer/consumer threads under TSan; `fillFrames()` never
  negative.

Cross-check the v3 constants in the header stay in sync with `jitter_buffer.go`
(a small static-assert or test, like the TS `PLAYOUT_TUNING` cross-check).

---

## 7. Risks

- **`R` not statically known.** The host's callback size may differ from the
  configured `reader_frame_ms`. Mitigation: capacity is worst-cased; a wrong `R`
  only shifts the geometry slightly and is recoverable via lap/overrun. Document
  that hosts should set `reader_frame_ms` to their device buffer.
- **Public config semantic change.** `jitter_buffer_max_ms` changes meaning
  (overrun ceiling → latency ceiling). Hosts that tuned it will behave
  differently. Mitigation: document loudly; consider a deprecation comment.
- **Stereo correctness.** Frame-atomic ±1 is essential for ITD. Mitigation:
  explicit `nc=2` splice tests (the Go suite has `TestV3Read_insertSpliceStereo`;
  add the drop-stereo case the TS port added).
- **Memory ordering.** Easy to get subtly wrong in C++. Mitigation: TSan in CI;
  follow §4.5 exactly; keep the producer touching only `write_pos_` + ring.

---

## 8. Open questions (for review)

Resolved in §0: SessionConfig rename (Q1), drop `JitterBufferState` (was Q3),
ROC already on v3 (was Q4). Remaining:

1. **`reader_frame_ms` placement:** `SessionConfig` (per-session) is the plan's
   assumption — the RT callback size is a device property, not a track property.
   Confirm no host needs per-track read sizes.
2. **`N` derivation:** confirm deriving `N` from `reader_frame_ms`
   (`N = round(2_000_000µs / R_µs)`) vs a fixed 400/750, and pick the clamp
   floor.

---

## 9. Progress tracker

| Phase | Status | Notes |
|---|---|---|
| 0 — Scaffolding | ☐ not started | |
| 1 — Geometry & construction | ☐ not started | |
| 2 — Read/write path (static window) | ☐ not started | |
| 3 — Adaptive controller | ☐ not started | |
| 4 — Stats & observability | ☐ not started | |
| 5 — Integration swap & config | ☐ not started | |
| Go doc fix — `circular_buffer.go` ROC comment | ☐ not started | tiny; do first (§0) |
| 6 — Test port & soak | ☐ not started | |
