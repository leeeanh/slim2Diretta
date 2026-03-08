# FLAC Hot-Path Jitter Reduction — Design

**Date**: 2026-03-08
**Status**: Approved
**Scope**: `src/FlacDecoder.cpp`, `src/FlacDecoder.h`

---

## Problem

Following the producer/sender jitter fixes for PCM (see `2026-03-08-pcm-hotpath-jitter-design.md`),
the FLAC path has analogous but distinct burst sources inside `FlacDecoder`:

1. **Output compaction on every `readDecoded()` call** (`FlacDecoder.cpp:249`): after
   copying decoded samples to the caller, the consumed prefix is unconditionally erased
   with `m_outputBuffer.erase(begin, begin + m_outputPos); m_outputPos = 0`. A FLAC
   block is typically 4096 frames; `MAX_DECODE_FRAMES` is 1024. This means calls 2, 3,
   and 4 on the same block each trigger a tail memmove of decreasing size (3072, 2048,
   1024 samples) with no corresponding decode work.

2. **Input compaction on every `readDecoded()` call** (`FlacDecoder.cpp:228`): the
   confirmed-consumed region of `m_inputBuffer` is erased unconditionally whenever
   `confirmedBufPos > 0`. The input is compressed, so the per-call memmove is smaller
   than for output, but it fires on every frame decode.

---

## Architecture

Localized buffer-mechanics changes inside the existing decoder control flow. No change
to metadata retry strategy, rollback semantics, or producer/sender interaction.

The implementation keeps `m_confirmedAbsolutePos`, `m_tellOffset`, the metadata/audio
phase split, and `process_single()` behavior intact. It replaces per-call front
compaction with persistent read offsets plus conditional compaction — for both
`m_outputBuffer` and the confirmed-consumed region of `m_inputBuffer`.

This design is the FLAC twin of the PCM hot-path jitter design: buffer mechanics only,
within the existing control flow.

---

## Components

### `src/FlacDecoder.h` — two new constants

```cpp
static constexpr size_t INPUT_COMPACT_THRESHOLD_BYTES    = 16384;
static constexpr size_t OUTPUT_COMPACT_THRESHOLD_SAMPLES = 16384;
```

`INPUT_COMPACT_THRESHOLD_BYTES`: byte-based, matching the unit of `m_inputBuffer` and
`m_inputPos`. At CD quality (~400 KB/s compressed), a confirmed frame advances by
4–8 KB; at hi-res up to ~40 KB. 16 KB amortises compaction across 1–4 frames.

`OUTPUT_COMPACT_THRESHOLD_SAMPLES`: int32_t-sample-based, matching the unit of
`m_outputBuffer` and `m_outputPos`. 16 384 samples = 65 536 bytes = 8 192 stereo
frames ≈ 2 FLAC blocks at 44.1 kHz. Equivalent to PcmDecoder's 65 536-byte threshold
stated in the unit the buffer actually uses.

### `src/FlacDecoder.cpp` — two compaction sites replaced

**Output compaction** (`FlacDecoder.cpp:249–254`): unconditional erase-and-reset
replaced with threshold-gated compaction. `m_outputPos` becomes persistent across
`readDecoded()` calls.

**Input compaction** (`FlacDecoder.cpp:228–234`): unconditional
`if (confirmedBufPos > 0) { erase... }` replaced with the same threshold-gate pattern.
The gate controls only when the physical erase fires; rollback accounting
(`m_confirmedAbsolutePos`, `m_tellOffset`, `m_inputPos`) is unchanged.

**One-shot metadata/audio boundary erase** (`FlacDecoder.cpp:146`): remains
unconditional. It fires once per stream to strip the metadata header; deferring it
buys nothing.

---

## Invariants and Data Flow

### Output buffer

`writeCallback` always appends at `m_outputBuffer.end()` (`FlacDecoder.cpp:339`).
`readDecoded` reads from `m_outputBuffer.data() + m_outputPos` (`FlacDecoder.cpp:244`).
These two directions never collide regardless of when compaction fires.

**Compaction gate** (mirrors PcmDecoder heuristic, with iterator cast matching
`src/PcmDecoder.cpp:108`):

```cpp
size_t unread = m_outputBuffer.size() - m_outputPos;
if (m_outputPos >= OUTPUT_COMPACT_THRESHOLD_SAMPLES &&
    (m_outputPos >= m_outputBuffer.size() / 2 ||
     unread < OUTPUT_COMPACT_THRESHOLD_SAMPLES)) {
    m_outputBuffer.erase(m_outputBuffer.begin(),
                         m_outputBuffer.begin() +
                             static_cast<std::ptrdiff_t>(m_outputPos));
    m_outputPos = 0;
}
```

Condition 1 (`>= threshold`): don't compact small waste — memmove cost exceeds reclaim
benefit.
Condition 2a (`>= half`): consumed is the majority, move is short relative to what is
freed.
Condition 2b (`unread < threshold`): tail is small — compact to prevent the buffer
staying large after a near-full drain.

### Input buffer

`m_confirmedAbsolutePos >= m_tellOffset` always, so
`confirmedBufPos = m_confirmedAbsolutePos - m_tellOffset` is never logically negative
before the `size_t` cast.

The ordering `confirmedBufPos <= m_inputPos <= m_inputBuffer.size()` always holds:
`m_inputPos` is the libFLAC read-ahead cursor (at or ahead of the confirmed boundary,
at or within the buffer). This bound makes `m_inputPos -= confirmedBufPos` safe when
compaction fires, and makes the ABORT rollback assignment `m_inputPos = confirmedBufPos`
safe when it does not.

**Compaction gate**:

```cpp
size_t inputUnread = m_inputBuffer.size() - confirmedBufPos;
if (confirmedBufPos >= INPUT_COMPACT_THRESHOLD_BYTES &&
    (confirmedBufPos >= m_inputBuffer.size() / 2 ||
     inputUnread < INPUT_COMPACT_THRESHOLD_BYTES)) {
    m_inputBuffer.erase(m_inputBuffer.begin(),
                        m_inputBuffer.begin() +
                            static_cast<std::ptrdiff_t>(confirmedBufPos));
    m_inputPos   -= confirmedBufPos;
    m_tellOffset += confirmedBufPos;
}
```

**Critical invariant**: `m_tellOffset` advances **only inside the erase branch**.
When the gate does not fire, `confirmedBufPos` bytes remain at their current buffer
offsets. `tellCallback` returns `m_tellOffset + m_inputPos`, which stays correct
because neither value changes until the next erase.

---

## Edge Cases

**ABORT rollback with uncompacted input**: when the gate does not fire, the
`m_confirmedAbsolutePos` / `m_tellOffset` / `m_inputPos` relationship is unchanged.
On the next ABORT, `confirmedBufPos` recomputes from the same base, and the rollback
assignment `m_inputPos = confirmedBufPos` remains within bounds by the ordering
invariant above.

**Buffered output across calls**: a FLAC block of 4096 frames decoded once serves
multiple `readDecoded()` calls with `MAX_DECODE_FRAMES = 1024`. Subsequent calls copy
directly from `m_outputBuffer` without entering the `process_single()` loop, *provided
the caller's `maxFrames` does not exceed what remains buffered*. If it does, the decode
loop resumes after copying what is available.

**Near-empty drain at stream end**: if the consumed prefix has already crossed
`OUTPUT_COMPACT_THRESHOLD_SAMPLES`, the small remaining tail satisfies condition 2b and
the gate fires. If the consumed prefix has not yet crossed the threshold, the final
sub-threshold buffer remains uncompacted — this is harmless because `flush()` or
destruction clears it.

**`flush()` correctness**: `m_outputBuffer.clear(); m_outputPos = 0;` and
`m_inputBuffer.clear(); m_inputPos = 0; m_tellOffset = 0;` already present; no change
needed. No deferred state survives a flush.

---

## Summary of Changes

| File | Change |
|------|--------|
| `src/FlacDecoder.h` | Add `INPUT_COMPACT_THRESHOLD_BYTES`, `OUTPUT_COMPACT_THRESHOLD_SAMPLES`; update `m_outputPos` comment to note it is now persistent across calls |
| `src/FlacDecoder.cpp` | Replace unconditional output erase (line 249) with threshold-gated compaction; replace unconditional input erase (line 228) with threshold-gated compaction |

---

## What This Does Not Change

- One-shot metadata/audio boundary erase (`FlacDecoder.cpp:146`).
- `m_confirmedAbsolutePos` update after successful frame (`FlacDecoder.cpp:219`).
- ABORT rollback path (`FlacDecoder.cpp:188`).
- `tellCallback` implementation (`FlacDecoder.cpp:406`).
- Metadata retry strategy (decoder delete/recreate on ABORT during metadata phase).
- Producer/sender thread structure, SPSC ring buffer, DirettaSync.

---

## Follow-on Work

**`feed()` append-side realloc**: `m_inputBuffer.insert(m_inputBuffer.end(), data, data + len)`
(`FlacDecoder.cpp:68`) can trigger vector reallocation during large metadata blocks.
This is a real FLAC-specific burst source but is a different class of problem from
front-compaction mechanics. It is the next FLAC optimization candidate after this
change is measured.

**Metadata retry decoder churn**: recreating `FLAC__StreamDecoder` on every ABORT
during the metadata phase is bursty on streams with large album art. Fixing it requires
changes to metadata phase control flow, not buffer mechanics. Deferred to a separate
design.
