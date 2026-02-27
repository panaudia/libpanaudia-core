# Phase 6: Platform Integration Prep

**Goal:** Verify the core library integrates cleanly into both target platforms (macOS HAL driver, Linux PortAudio app) and produce working integration examples.

**Depends on:** Phase 5 (core library tested and stable)
**Blocks:** Separate platform projects (macOS driver, Linux client)

## Tasks

### macOS HAL Driver Integration
- [ ] Create minimal libASPL-based HAL driver that links `libpanaudia-core.a`
  - [ ] Verify static library links without symbol conflicts (msquic, libopus, std library)
  - [ ] Verify driver loads in coreaudiod (install to `/Library/Audio/Plug-Ins/HAL/`, restart coreaudiod)
  - [ ] Verify no sandbox violations: msquic opens UDP sockets from within coreaudiod — confirm this works
- [ ] Minimal audio test:
  - [ ] libASPL creates one virtual output device (VAD)
  - [ ] `OnWriteMixedOutput` callback writes captured audio to core via `write_audio()`
  - [ ] libASPL creates one virtual input device
  - [ ] `OnReadClientInput` callback reads from core via `read_audio()`
  - [ ] Connect to Panaudia server, verify audio flows end-to-end through the driver
- [ ] Build configuration:
  - [ ] Document required CMake/Xcode settings for building a `.driver` bundle with the core
  - [ ] Static linking flags for msquic and libopus within the bundle
  - [ ] Code signing: verify the bundle can be signed with a Developer ID
  - [ ] Notarization: submit signed bundle to Apple, verify acceptance

### Linux PortAudio Integration
- [ ] Create minimal PortAudio app that links `libpanaudia-core.a`
  - [ ] Verify static library links without issues on Linux (x86_64, aarch64)
  - [ ] Verify PortAudio callback integration:
    - [ ] Input callback → `write_audio()` for outbound tracks
    - [ ] Output callback → `read_audio()` for inbound tracks
- [ ] Minimal audio test:
  - [ ] Open PortAudio input stream (microphone)
  - [ ] Open PortAudio output stream (speakers)
  - [ ] Connect to Panaudia server, verify audio roundtrip
- [ ] PortAudio backend selection: ALSA, PulseAudio, PipeWire — verify at least one works
- [ ] Build configuration:
  - [ ] Document required CMake settings for linking PortAudio + core
  - [ ] PortAudio as system dependency or FetchContent?

### Platform-Specific Build Flags
- [ ] Document any macOS-specific compiler/linker flags needed
- [ ] Document any Linux-specific compiler/linker flags needed
- [ ] Verify both platforms build with the same CMakeLists.txt (platform conditionals only where necessary)

### Integration Examples
- [ ] `examples/macos_driver/` — minimal HAL driver with libpanaudia-core
  - [ ] CMakeLists.txt
  - [ ] Minimal libASPL device setup
  - [ ] Audio callback wiring
  - [ ] Hardcoded session config for testing
- [ ] `examples/linux_portaudio/` — minimal PortAudio app with libpanaudia-core
  - [ ] CMakeLists.txt
  - [ ] PortAudio stream setup
  - [ ] Audio callback wiring
  - [ ] Command-line session config

### Known Risks to Verify
- [ ] **msquic in coreaudiod sandbox** — msquic needs to open UDP sockets and do TLS. Verify coreaudiod's sandbox allows this. roc-vad uses RTP (also UDP) successfully, which is encouraging.
- [ ] **msquic thread priority** — msquic creates its own worker threads. In coreaudiod, do these run at the expected priority? Any interaction with the RT audio thread priority?
- [ ] **Static library size** — msquic + quictls + libopus + core. Measure total `.a` size and final `.driver` bundle size. Should be < 10MB.
- [ ] **Symbol visibility** — When statically linked into a `.driver` bundle, ensure msquic/libopus symbols don't conflict with anything else in coreaudiod's process space. May need `-fvisibility=hidden` and explicit exports.

## Done When
- `libpanaudia-core.a` links and runs inside a macOS HAL driver (audio flows through coreaudiod)
- `libpanaudia-core.a` links and runs inside a Linux PortAudio app (audio flows through speakers)
- Build requirements documented for both platforms
- Integration examples compile and work
- No sandbox, signing, or symbol visibility issues on macOS
