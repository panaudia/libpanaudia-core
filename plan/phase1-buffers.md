# Phase 1: Ring Buffer & Jitter Buffer

**Goal:** Two production-quality, RT-safe buffer implementations with thorough tests. These are the foundation everything else builds on — the RT thread touches nothing else.

**Depends on:** Phase 0 (project compiles)
**Blocks:** Phase 4 (session manager wires buffers to transport)

**Reference:** [jitter_buffer_design.md](../../spatial-mixer/plan/jitter_buffer_design.md), Go implementation `spatial-mixer/core/buffers/circular_buffer.go`

## Tasks

### RingBuffer (outbound audio)

Plain SPSC lock-free ring buffer. RT thread writes, send thread reads.

- [x] `ring_buffer.h` / `ring_buffer.cpp` — class with:
  - [x] Constructor: `RingBuffer(uint32_t capacity_frames, uint32_t channels)`
  - [x] `uint32_t write(const float* samples, uint32_t frame_count)` — writer side
  - [x] `uint32_t read(float* buffer, uint32_t frame_count)` — reader side
  - [x] `uint32_t write_available() const`
  - [x] `uint32_t read_available() const`
  - [x] `void flush()` — discard all buffered data (for reconnect)
- [x] `alignas(64)` on `std::atomic<uint32_t> write_pos_`, `read_pos_`, `buffered_`
- [x] Storage: `std::vector<float>` sized at construction (capacity_frames × channels)
- [x] Wrap-around copy logic (split copy when crossing buffer end)
- [x] Overflow policy: writer drops oldest (advances read_pos)

### RingBuffer Tests
- [x] Basic write/read roundtrip — write N frames, read N frames, verify match
- [x] Wrap-around — write enough to wrap past buffer end, verify data integrity
- [x] Partial reads — read less than available, verify remainder preserved
- [x] Overflow — write more than capacity, verify oldest data dropped and newest preserved
- [x] Empty read — read from empty buffer, verify returns 0
- [x] `write_available` / `read_available` accuracy
- [x] `flush` — verify buffer reports empty after flush
- [x] Multi-channel — verify interleaved stereo (2ch), quad (4ch) data integrity

### JitterBuffer (inbound audio)

Port of Go `CircularBuffer` with SPSC thread safety.

- [x] `jitter_buffer.h` / `jitter_buffer.cpp` — class with:
  - [x] Config struct matching Go's `CircularBufferConfig`
  - [x] `void write(const float* src, uint32_t float_count)` — recv thread writes decoded audio
  - [x] `bool read(float* dst, uint32_t float_count)` — RT thread reads. Returns false if silence (FILLING).
  - [x] Stats struct matching Go's `CircularBufferStats`
  - [x] `Stats get_stats() const`
- [x] State machine: FILLING / PLAYING
  - [x] Initial state: FILLING
  - [x] FILLING → PLAYING when fill ≥ targetLow
  - [x] PLAYING → FILLING when fill < min (underrun)
  - [x] Reset correction counter on FILLING → PLAYING transition
- [x] Overrun snap: when fill > max, snap readPos to targetCentre
- [x] Drift correction: ±1 sample when outside target window, every correctionInterval reads
  - [x] Below targetLow: insert (duplicate last sample)
  - [x] Above targetHigh: drop (advance readPos extra)
- [x] Thread safety:
  - [x] `buffered` as `std::atomic<int32_t>`
  - [x] `writePos` as `std::atomic<uint32_t>` (writer stores with release, reader loads with acquire during overflow snap)
  - [x] `readPos` reader-owned only
  - [x] Writer overflow: set `std::atomic<bool> overflow_flag_` instead of moving readPos
  - [x] Reader checks overflow_flag at start of read, performs snap if set
- [x] `alignas(64)` on atomics to prevent false sharing

### JitterBuffer Tests
- [x] FILLING → PLAYING transition — write until targetLow, verify read returns true
- [x] Silence during FILLING — read before targetLow reached, verify zeros and false return
- [x] Steady-state — write and read at matched rates, verify data passes through
- [x] Clock drift (slow writer) — writer slightly slower, verify insert corrections occur
- [x] Clock drift (fast writer) — writer slightly faster, verify drop corrections occur
- [x] Underrun recovery — stop writing, verify FILLING, silence, resume → PLAYING
- [x] Correction counter reset on recovery — no spurious corrections after FILLING → PLAYING
- [x] Overrun snap — fill past max, verify snap to targetCentre
- [x] Recovery thrashing resistance — interleaved write/read stays in PLAYING
- [x] Overflow flag — concurrent write overflow, reader handles snap correctly
- [x] Multi-channel — stereo, 4ch FOA, verify sample alignment preserved
- [x] Stats accuracy — verify all stats fields reflect actual state

### Stress / Timing Tests
- [x] RingBuffer concurrent stress — writer and reader on separate threads, 5 seconds at 48kHz stereo
- [x] JitterBuffer concurrent stress — writer and reader on separate threads, 2 seconds at 48kHz
- [x] Performance: all operations < 50ns (measured via Catch2 benchmarks in `bench_buffers.cpp`)

## Done When
- [x] Both `RingBuffer` and `JitterBuffer` compile and pass all tests
- [x] Thread-safety tests run under ThreadSanitizer (TSan) with no warnings
- [x] Performance: read/write operations < 1µs — actual: all under 50ns
