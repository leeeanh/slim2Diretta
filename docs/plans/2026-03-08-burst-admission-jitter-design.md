# Burst Admission and Startup Jitter Reduction — Design

**Date**: 2026-03-08
**Status**: Approved
**Scope**: `src/main.cpp`, `src/FlacDecoder.cpp`, `src/FlacDecoder.h`, `src/PcmDecoder.cpp`

---

## Problem

Three residual jitter sources remain after the PCM and FLAC hot-path compaction fixes:

1. **Sender recovery burst** (`main.cpp`): when the Diretta ring falls below `STEADY_EXIT`,
   `kRecovery` mode refills with an uncapped `while` loop calling `sendChunk()` back-to-back
   until `RECOVERY_TARGET = 0.90`. The sender thread has RT priority, so this burst runs
   uninterrupted against the DAC clock.

2. **FLAC metadata decoder churn** (`FlacDecoder.cpp`): the metadata phase deletes and
   recreates `FLAC__StreamDecoder` on every ABORT until enough bytes are buffered. On streams
   with large album-art blocks, this can require 100+ retries, each incurring decoder
   construction and libFLAC init overhead before audio decode begins.

3. **Append-side buffer growth** (`PcmDecoder.cpp`, `FlacDecoder.cpp`): `m_dataBuf` and
   `m_outputBuffer` start with conservative reserves and grow by vector doublings when the
   format-aware sizing is not applied up front. Individual doubling events are short-lived but
   unpredictable in timing.

---

## Architecture

Three independent, localized changes with no shared state. Each targets one file or one
concern. They are implemented and committed in order of impact:

1. `main.cpp` — replace uncapped recovery loop with per-wakeup frame budget
2. `FlacDecoder.cpp` — replace retry churn with a pre-scan gate
3. `PcmDecoder.cpp` + `FlacDecoder.cpp` — apply format-aware reserve after header parse

---

## Component 1: Recovery pacing (`main.cpp`)

### What changes

`RECOVERY_TARGET = 0.90f` is **retired and removed**. The constant served only to gate the
recovery `while` loop; once the loop becomes a single per-wakeup call, the target has no role.

The mode state machine is **unchanged**: `kRecovery → kSteady` at `STEADY_ENTER = 0.75`,
`kSteady → kRecovery` at `STEADY_EXIT = 0.40`.

### New constants

```cpp
// Recovery pacing — replaces RECOVERY_TARGET
constexpr float  DEEP_RECOVERY_THRESHOLD   = 0.20f;   // below → emergency budget
constexpr float  RECOVERY_CHUNK_MS         = 5.0f;    // normal recovery per wakeup
constexpr float  DEEP_RECOVERY_CHUNK_MS    = 10.0f;   // emergency recovery per wakeup
constexpr size_t RECOVERY_CHUNK_MIN_FRAMES = 128;     // floor when rate unavailable
constexpr size_t RECOVERY_CHUNK_MAX_FRAMES = 1024;    // ceiling
```

### Behavior

Each `kRecovery` wakeup samples the buffer level once and sends at most one chunk:

```cpp
// level was already sampled for the mode transition check — reuse it
float chunkMs = (level < DEEP_RECOVERY_THRESHOLD)
    ? DEEP_RECOVERY_CHUNK_MS : RECOVERY_CHUNK_MS;
size_t chunkFrames = (rate > 0)
    ? std::clamp(static_cast<size_t>(rate * chunkMs / 1000.0f),
                 RECOVERY_CHUNK_MIN_FRAMES, RECOVERY_CHUNK_MAX_FRAMES)
    : RECOVERY_CHUNK_MIN_FRAMES;
size_t contiguous = cacheContiguousFrames();
if (contiguous > 0) sendChunk(std::min(contiguous, chunkFrames));
```

The outer `waitForSpace` loop (2ms timeout) provides inter-wakeup cadence. Recovery advances
at one bounded chunk per Diretta pop event rather than draining continuously.

### Three-tier pacing summary

| Mode | Budget | Condition |
|------|--------|-----------|
| Steady | 2ms (`STEADY_CHUNK_MS`) | level ∈ [STEADY_EXIT, STEADY_CEILING] |
| Recovery | 5ms (`RECOVERY_CHUNK_MS`) | level ∈ [DEEP_RECOVERY_THRESHOLD, STEADY_ENTER) |
| Emergency | 10ms (`DEEP_RECOVERY_CHUNK_MS`) | level < DEEP_RECOVERY_THRESHOLD |

### `kSteady` path

Unchanged: ceiling gate at `STEADY_CEILING = 0.65`, chunk sized from `STEADY_CHUNK_MS = 2.0f`,
same `STEADY_CHUNK_MIN_FRAMES` / `STEADY_CHUNK_MAX_FRAMES` clamp.

---

## Component 2: FLAC metadata pre-scan (`FlacDecoder.cpp`)

### What changes

The recreate-and-retry loop in the metadata phase is replaced by a byte-level pre-scan that
gates `initDecoder()`. libFLAC is never constructed until all metadata bytes are confirmed
present.

### Pre-scan logic

Runs on raw `m_inputBuffer` bytes before any libFLAC call. A new `m_metadataPrescanDone`
bool replaces `m_metadataRetries` as the gate for the metadata phase. No byte-count cap
is applied — metadata section size is not bounded here because legitimate streams with
large embedded cover art can exceed any reasonable fixed limit. Termination is driven
solely by stream completion (all bytes arrived) or EOF (truncated/corrupt stream).

```
1. if (m_inputBuffer.size() < 4 && !m_eof)   → return 0 (wait for more data)
   if (m_inputBuffer.size() < 4 &&  m_eof)   → log error, set m_error, return 0
2. if (memcmp(buf, "fLaC", 4) != 0)          → log error, set m_error, return 0
3. pos = 4
4. loop:
     if (pos + 4 > m_inputBuffer.size())
         !m_eof                               → return 0 (wait for header bytes)
          m_eof                               → log "truncated metadata", set error, return 0
     last    = buf[pos] & 0x80
     blockLen = (buf[pos+1]<<16)|(buf[pos+2]<<8)|buf[pos+3]
     blockEnd = pos + 4 + blockLen
     if (blockEnd > m_inputBuffer.size())
         !m_eof                               → return 0 (wait for block body)
          m_eof                               → log "truncated metadata block", set error, return 0
     pos = blockEnd
     if (last) → pre-scan complete, set m_metadataPrescanDone, proceed to init
```

### Initialization and metadata processing

Once `m_metadataPrescanDone` is set, `initDecoder()` is called exactly once, followed by
`process_until_end_of_metadata()` exactly once. At this point all bytes are known to be
present, so an ABORT here indicates stream corruption — it is treated as a hard error, not
a retry trigger.

### Logging

- Optional throttled `LOG_DEBUG` while waiting for more metadata bytes (one log per call
  is excessive; suppress or throttle by buffer-size milestone).
- One `LOG_DEBUG` or `LOG_INFO` when the pre-scan completes and metadata processing begins,
  noting buffer size and any retry-equivalent wait count.

### What is removed

- `m_metadataRetries` counter and all retry logging
- The `FLAC__stream_decoder_delete` / recreate path on metadata ABORT
- The "retries % 50 == 0" throttle log

### What is kept unchanged

- Post-metadata audio-boundary compaction via `get_decode_position`
- Phase 2 audio decode (ABORT rollback, `flush`, `m_confirmedAbsolutePos`)
- All callbacks (`readCallback`, `writeCallback`, `metadataCallback`, `errorCallback`,
  `tellCallback`)

---

## Component 3: Format-aware buffer reservation

### 3a: PCM `m_dataBuf` (`PcmDecoder.cpp`)

After `parseWavHeader()` or `parseAiffHeader()` resolves `bytesPerFrame` and `sampleRate`,
apply a one-shot reserve before copying the audio payload from `m_headerBuf` into `m_dataBuf`.
Only grow, never shrink:

```cpp
size_t target = static_cast<size_t>(bytesPerFrame) * sampleRate / 4;  // ~250ms raw PCM
target = (target + 65535u) & ~65535u;                                   // round up to 64KB
target = std::clamp(target, size_t(128 * 1024), size_t(512 * 1024));
if (m_dataBuf.capacity() < target) m_dataBuf.reserve(target);
```

Same formula applied in the raw PCM path (`detectContainer`, before transitioning to
`State::DATA`). The constructor reserve of 32768 bytes is kept as a startup floor for the
header-parse phase.

**Sizing rationale**:
- 250ms absorbs HTTP read bursts without duplicating the larger decoded-frame SPSC cache.
- 128 KB floor covers the startup read burst with headroom.
- 512 KB cap keeps reserve bounded on high-rate PCM (e.g. 384 kHz / 32-bit / 2ch ≈ 3 MB/s).
- Round-up to 64 KB aligns with typical OS huge-page and allocator slab boundaries.

### 3b: FLAC `m_outputBuffer` (`FlacDecoder.cpp`)

`metadataCallback` gains capture of `info.max_blocksize` into a new member `m_maxBlocksize`.
After `m_metadataDone = true` (immediately after `process_until_end_of_metadata` succeeds),
reserve 4 blocks of output capacity:

```cpp
if (m_maxBlocksize > 0 && m_format.channels > 0) {
    size_t target = m_maxBlocksize * m_format.channels * 4;
    if (m_outputBuffer.capacity() < target) m_outputBuffer.reserve(target);
}
```

**First-frame fallback**: if `m_maxBlocksize` was not populated by STREAMINFO (seek streams),
`writeCallback` applies the same 4-block reserve on the first decoded frame:

```cpp
if (!m_outputReserved && m_format.channels > 0) {
    size_t target = std::min(frame->header.blocksize * channels * 4, size_t(65536));
    if (m_outputBuffer.capacity() < target) m_outputBuffer.reserve(target);
    m_outputReserved = true;
}
```

The cap of 65536 samples (256 KB at int32) prevents a pathological `max_blocksize` from
leaving a permanently oversized buffer.

`flush()` calls `m_outputBuffer.clear()` which preserves capacity — intentional, as the
reserve survives track-to-track within a session.

---

## Summary of Changes

| Commit | File(s) | Change |
|--------|---------|--------|
| 1 | `src/main.cpp` | Remove `RECOVERY_TARGET`; replace uncapped recovery `while` loop with single per-wakeup `sendChunk` sized from 3-tier budget |
| 2 | `src/FlacDecoder.h`, `src/FlacDecoder.cpp` | Add `FLAC_METADATA_PRESCAN_MAX_BYTES`, `m_maxBlocksize`, `m_metadataPrescanDone`, `m_outputReserved`; replace retry loop with pre-scan gate; add post-metadata output reserve |
| 3 | `src/PcmDecoder.cpp`, `src/FlacDecoder.cpp` | Apply format-aware `m_dataBuf` reserve after header parse; apply FLAC first-frame fallback reserve |

---

## What This Does Not Change

- SPSC decoded-frame cache, `DirettaSync`, `DirettaRingBuffer`
- `kSteady` sender path, `waitForSpace` loop, mode hysteresis thresholds
- FLAC audio-phase rollback (`m_confirmedAbsolutePos`, `flush`, ABORT handling)
- PCM container detection and header parsing logic
- Threshold-gated compaction introduced in the previous PCM/FLAC jitter commits
