# Phase 5: Test Harness & Integration

**Goal:** Verify the complete library works end-to-end against the real Panaudia server, under realistic conditions including multi-track, sustained operation, and failure scenarios.

**Depends on:** Phase 4 (functional PanaudiaCore)
**Blocks:** Phase 6 (platform integration — want confidence before embedding in driver/app)

## Tasks

### Test Harness Executable
- [ ] `tests/test_harness.cpp` — standalone program (not unit test) that:
  - [ ] Creates a `PanaudiaCore` session with configurable tracks
  - [ ] Simulates RT callbacks at 48kHz timing (high-priority thread, reads/writes every 5ms or configurable)
  - [ ] Generates test audio: sine wave, white noise, or silence (selectable)
  - [ ] Measures RT callback timing jitter (should be < 1ms standard deviation)
  - [ ] Runs for configurable duration (default: 60 seconds)
  - [ ] Reports stats on exit: buffer levels, underruns, overruns, drift corrections, packets sent/received
- [ ] Command-line options:
  - [ ] `--server <url>` — Panaudia server URL
  - [ ] `--jwt <token>` — JWT for authentication
  - [ ] `--tracks-out <N>` — number of outbound mono audio tracks (default 1)
  - [ ] `--tracks-in <N>` — number of inbound audio tracks (default 1)
  - [ ] `--channels-in <N>` — channel count for inbound tracks (default 2 = stereo)
  - [ ] `--codec <opus|pcm>` — codec for audio tracks (default opus)
  - [ ] `--frame-ms <N>` — Opus frame size (default 5)
  - [ ] `--duration <secs>` — test duration (default 60)
  - [ ] `--data-tracks` — include state/control data tracks (default: yes)

### Audio Roundtrip Test
- [ ] Send known audio pattern (e.g., 1kHz sine) on outbound track
- [ ] Server mixes and returns on inbound track
- [ ] Verify received audio contains the expected signal (cross-correlation or simple energy check)
- [ ] Measure roundtrip latency: timestamp outbound frame, detect arrival of corresponding inbound frame
  - Note: requires server to return audio promptly — depends on server mixing tick rate

### Multi-Track Test
- [ ] 8 outbound mono Opus tracks simultaneously
- [ ] 1 inbound stereo Opus track
- [ ] State + control data tracks
- [ ] Run for 5 minutes
- [ ] Verify: no crashes, no memory leaks, all tracks streaming, buffer stats healthy

### Mixed Codec Test
- [ ] Outbound: Opus tracks + PCM tracks in same session
- [ ] Inbound: Opus + PCM
- [ ] Verify both codecs work simultaneously without interference

### Data Track Test
- [ ] Send state data (48-byte binary) at 10Hz on outbound data track
- [ ] Receive state/attributes on inbound data tracks
- [ ] Verify DataRecvCallback fires with correct data
- [ ] Verify send_data works from control thread while audio is streaming

### Reconnection Test
- [ ] Establish session, stream for 10 seconds
- [ ] Kill network (e.g., firewall rule, or server restart)
- [ ] Verify:
  - [ ] StatusCallback reports Reconnecting
  - [ ] Inbound audio → silence (jitter buffers drain → FILLING)
  - [ ] Outbound ring buffers continue accepting writes (don't block RT thread)
  - [ ] Connection re-establishes automatically
  - [ ] StatusCallback reports Connected
  - [ ] Audio resumes (jitter buffers re-fill → PLAYING)
  - [ ] Outbound ring buffers flushed (no stale audio sent)

### Sustained Operation Test
- [ ] Run test harness for 1 hour with moderate track count (4 out, 1 in, data tracks)
- [ ] Monitor: memory usage (should be stable — no leaks), buffer stats, CPU usage
- [ ] Verify drift correction keeps buffers stable over extended period

### Stress Test
- [ ] 16 outbound mono Opus tracks + 1 inbound 4ch FOA track + data tracks
- [ ] Verify CPU usage is acceptable on target hardware
- [ ] Identify performance ceiling: at what track count does the system degrade?

### Latency Measurement
- [ ] Measure component latencies:
  - [ ] Ring buffer: write-to-read latency (should be ~1 buffer period)
  - [ ] Opus encode time per frame (varies with frame size and channel count)
  - [ ] Opus decode time per frame
  - [ ] Jitter buffer: initial fill time (should match jitter_buffer_initial_ms)
  - [ ] End-to-end: RT write_audio → network → RT read_audio
- [ ] Produce a latency budget breakdown table

## Done When
- Test harness runs and produces clear pass/fail results
- Audio roundtrip verified against real Panaudia server
- Multi-track (8+ out, stereo in) sustained for 5+ minutes with healthy stats
- Reconnection test passes
- 1-hour sustained test shows no memory leaks or degradation
- Latency budget documented
