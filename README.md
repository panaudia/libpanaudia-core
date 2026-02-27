# libpanaudia-core

A cross-platform C++ library for multitrack spatial audio streaming over MOQ/QUIC.

## Overview

libpanaudia-core provides the shared streaming infrastructure used by Panaudia's native clients (macOS and Linux). It handles MOQ transport, Opus encoding/decoding, multitrack session management, and spatial positioning — exposing a clean C API for integration with platform-specific shells (Swift on macOS, PortAudio on Linux).

## Key Capabilities

- **MOQ/QUIC transport** — bidirectional media streaming using msquic
- **Opus codec** — encode/decode with configurable frame sizes
- **Multitrack** — N outbound + M inbound audio tracks per session
- **Jitter buffering** — per-track adaptive jitter buffers
- **OSC server** — receive and forward spatial position data
- **JWT authentication** — Ed25519 token-based auth
- **C API** — FFI-friendly surface for Swift, Python, or other language bindings

## Lineage

The streaming core is derived from the proven MOQ/QUIC implementation in the [Panaudia Unreal Engine plugin](../panaudia-client/sdks/unreal/), generalised for multitrack use and decoupled from UE dependencies.

## Intended Consumers

| Client | Platform | Audio Layer | Integration |
|--------|----------|-------------|-------------|
| Panaudia macOS app | macOS | HAL driver (libASPL) + IPC | Swift via C API |
| Panaudia Linux client | Linux | PortAudio | Direct C++ linking |

## Documentation

- [Architecture & Implementation Plan](plan/plan.md)
- [Multitrack Client Overview](../spatial-mixer/plan/multitrack/plan.md)

## Status

Phase 1 complete — ring buffer and jitter buffer implementations with comprehensive tests.
