# Phase 0: Repository & Build Setup

**Goal:** A compilable CMake project with the public API header, stub implementation, dependency fetching, and CI. Nothing functional yet — just the skeleton.

**Depends on:** Nothing
**Blocks:** All other phases

## Tasks

### CMake Project Structure
- [x] Create top-level `CMakeLists.txt` with project name, C++17 standard, compile options
- [x] `FetchContent` for msquic (pinned to v2.6.0 commit `ed14762f` — same as UE plugin, commented out until Phase 3)
- [x] `FetchContent` for libopus (pinned to v1.5.2, commented out until Phase 2)
- [x] Verify both dependencies build from source on macOS arm64 and Linux x86_64 (verified in phases 2/3 + Docker)
- [x] Static library target `libpanaudia-core` from `src/` sources
- [x] Test target `panaudia-core-tests` (Catch2 v3.7.1 via FetchContent)
- [x] Test harness target `panaudia-test-harness` (standalone executable)
- [x] Install rules: header + static lib

### Public API Header
- [x] Create `include/panaudia/core.h` with all types from interfaces.md §8:
  - [x] `TrackType`, `TrackDirection`, `AudioCodec` enums
  - [x] `ConnectionState`, `LogLevel` enums
  - [x] `TrackConfig` struct
  - [x] `SessionConfig` struct
  - [x] `BufferStatus`, `SessionStats` structs
  - [x] Callback typedefs: `DataRecvCallback`, `StatusCallback`, `LogCallback`
  - [x] `TrackHandle` forward declaration (opaque pointer)
  - [x] `PanaudiaCore` class declaration (pimpl pattern)
- [x] Verify header compiles standalone

### Stub Implementation
- [x] Create `src/panaudia_core.cpp` — all methods stubbed (no-op or return defaults)
- [x] Create empty source files for all planned modules:
  - [x] `src/session_manager.cpp`
  - [x] `src/moq_transport.cpp`
  - [x] `src/opus_codec.cpp`
  - [x] `src/ring_buffer.cpp`
  - [x] `src/jitter_buffer.cpp`
  - [x] `src/send_worker.cpp`
  - [x] `src/recv_worker.cpp`
- [x] Verify the static library builds on macOS (arm64) — 0 warnings, 97KB
- [x] Verify the static library builds on Linux (verified via Docker/Ubuntu 24.04)

### CI
- [ ] GitHub Actions workflow: build on macOS (arm64 via M1 runner or x86_64)
- [ ] GitHub Actions workflow: build on Linux (x86_64, Ubuntu)
- [ ] Run tests (even though they're placeholder stubs at this point)
- [ ] Consider: aarch64 Linux build (cross-compile or native ARM runner)

### Directory Structure Verification
- [x] Confirm final directory layout matches:
```
libpanaudia-core/
├── .gitignore
├── CMakeLists.txt
├── include/
│   └── panaudia/
│       └── core.h
├── src/
│   ├── panaudia_core.cpp
│   ├── session_manager.cpp
│   ├── moq_transport.cpp
│   ├── opus_codec.cpp
│   ├── ring_buffer.cpp
│   ├── jitter_buffer.cpp
│   ├── send_worker.cpp
│   └── recv_worker.cpp
├── tests/
│   ├── test_ring_buffer.cpp
│   ├── test_jitter_buffer.cpp
│   ├── test_opus_codec.cpp
│   ├── test_session.cpp
│   └── test_harness.cpp
└── plan/
    ├── plan.md
    ├── phase0-repo-setup.md
    ├── phase1-buffers.md
    ├── phase2-opus.md
    ├── phase3-moq-transport.md
    ├── phase4-session-manager.md
    ├── phase5-testing.md
    └── phase6-platform-integration.md
```

## Done When
- [x] `cmake --build .` succeeds on macOS
- [x] Static library `libpanaudia-core.a` is produced
- [x] Test binary compiles and runs (6 assertions in 6 test cases, all pass)
- [ ] CI is green (not yet set up)

## Status
Phase 0 is **complete for local development**. CI setup is deferred — can be added when the repo is pushed to GitHub.
