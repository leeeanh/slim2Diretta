# FLAC Hot-Path Jitter Reduction Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace per-call front-compaction in `FlacDecoder` with persistent read offsets and threshold-gated compaction, eliminating repeated tail memmoves from the hot path.

**Architecture:** Two surgical changes inside `FlacDecoder`: (1) make `m_outputPos` persistent across `readDecoded()` calls and gate output compaction on `OUTPUT_COMPACT_THRESHOLD_SAMPLES`, eliminating 3 tail memmoves per FLAC block; (2) gate input compaction on `INPUT_COMPACT_THRESHOLD_BYTES`, eliminating the unconditional per-call erase on the compressed-input side. No changes to rollback semantics, metadata retry, or producer/sender interaction.

**Design doc:** `docs/plans/2026-03-08-flac-hotpath-jitter-design.md`

**Tech Stack:** C++17, libFLAC stream decoder API, CMake. No automated test suite — verification is build + manual listening test.

**Build:**
```bash
cd build && make -j$(nproc)
```

---

### Task 1: Add threshold constants to `FlacDecoder.h`

**Files:**
- Modify: `src/FlacDecoder.h:68` (output buffer comment block)

**Context:** `m_outputPos` is declared at line 69. The comment above it currently says only "Output buffer (filled by write callback)". The constants should sit with the members they govern.

**Step 1: Open the file and locate the output buffer block**

Read `src/FlacDecoder.h` lines 62–75. You will see:

```cpp
// Output buffer (filled by write callback)
std::vector<int32_t> m_outputBuffer;
size_t m_outputPos = 0;
```

**Step 2: Replace that block**

```cpp
// Output buffer (filled by write callback).
// m_outputPos is a persistent read offset — not reset after every copy.
// Compaction is deferred until OUTPUT_COMPACT_THRESHOLD_SAMPLES of consumed
// samples have accumulated and the move is worthwhile.
std::vector<int32_t> m_outputBuffer;
size_t m_outputPos = 0;

// Compaction thresholds — unit matches the buffer's element unit.
// Input threshold: bytes (m_inputBuffer is uint8_t).
// Output threshold: int32_t samples (m_outputBuffer is int32_t).
static constexpr size_t INPUT_COMPACT_THRESHOLD_BYTES    = 16384;
static constexpr size_t OUTPUT_COMPACT_THRESHOLD_SAMPLES = 16384;
```

**Step 3: Build to confirm no errors**

```bash
cd build && make -j$(nproc)
```

Expected: exits 0. Review terminal output for any `error:` or `warning:` lines before proceeding.

**Step 4: Commit**

```bash
git add src/FlacDecoder.h
git commit -m "perf(flac): add compaction threshold constants to FlacDecoder"
```

---

### Task 2: Fix output compaction — make `m_outputPos` persistent

**Files:**
- Modify: `src/FlacDecoder.cpp:236–258`

**Context:** Read `src/FlacDecoder.cpp` lines 236–258 before editing. The current code:

```cpp
// Copy available output frames
if (!m_formatReady || m_format.channels == 0) return 0;

size_t framesAvailable = (m_outputBuffer.size() - m_outputPos) / m_format.channels;
size_t framesToCopy = std::min(framesAvailable, maxFrames);

if (framesToCopy > 0) {
    size_t samplesToCopy = framesToCopy * m_format.channels;
    std::memcpy(out, m_outputBuffer.data() + m_outputPos,
                samplesToCopy * sizeof(int32_t));
    m_outputPos += samplesToCopy;
    m_decodedSamples += framesToCopy;

    // Compact output buffer
    if (m_outputPos > 0) {
        m_outputBuffer.erase(m_outputBuffer.begin(),
                             m_outputBuffer.begin() + m_outputPos);
        m_outputPos = 0;
    }
}

return framesToCopy;
```

The problem: `m_outputPos` is reset to 0 after every call by the unconditional erase. A 4096-sample FLAC block decoded once causes a tail memmove (3072, 2048, 1024 samples) on each of the next 3 calls that consume 1024 samples.

**Step 1: Replace the "Copy available output frames" block**

Replace the entire block from `// Copy available output frames` through `return framesToCopy;` with:

```cpp
// Copy available output frames
if (!m_formatReady || m_format.channels == 0) return 0;

size_t framesAvailable = (m_outputBuffer.size() - m_outputPos) / m_format.channels;
size_t framesToCopy = std::min(framesAvailable, maxFrames);

if (framesToCopy > 0) {
    size_t samplesToCopy = framesToCopy * m_format.channels;
    std::memcpy(out, m_outputBuffer.data() + m_outputPos,
                samplesToCopy * sizeof(int32_t));
    m_outputPos += samplesToCopy;
    m_decodedSamples += framesToCopy;
}

// Compact output buffer only when the consumed prefix is large enough and
// reclaiming it is worthwhile: either the majority is consumed, or the
// remaining tail is itself below the threshold.
size_t outputUnread = m_outputBuffer.size() - m_outputPos;
if (m_outputPos >= OUTPUT_COMPACT_THRESHOLD_SAMPLES &&
    (m_outputPos >= m_outputBuffer.size() / 2 ||
     outputUnread < OUTPUT_COMPACT_THRESHOLD_SAMPLES)) {
    m_outputBuffer.erase(m_outputBuffer.begin(),
                         m_outputBuffer.begin() +
                             static_cast<std::ptrdiff_t>(m_outputPos));
    m_outputPos = 0;
}

return framesToCopy;
```

Key differences from the original:
- `m_outputPos` accumulates across calls instead of being reset.
- The erase block is moved outside the `if (framesToCopy > 0)` scope so it also runs after a no-copy call (e.g. format not ready, then format becomes ready).
- The two-condition gate controls when physical compaction happens.

**Step 2: Build**

```bash
cd build && make -j$(nproc)
```

Expected: exits 0. Review terminal output for any `error:` or `warning:` lines before proceeding.

**Step 3: Manually verify the invariant holds**

Read back the edited function. Confirm:
- `m_outputBuffer.data() + m_outputPos` is used in both the `std::memcpy` and the `size_t framesAvailable` calculation, so `m_outputPos` is always the consistent read cursor.
- `writeCallback` (line ~339) does `m_outputBuffer.resize(prevSize + blocksize * channels)` and writes to `data() + prevSize`. Since `prevSize = m_outputBuffer.size()` at the time of the callback, new data always lands after any existing unread data. Persistent `m_outputPos` does not conflict.

**Step 4: Commit**

```bash
git add src/FlacDecoder.cpp
git commit -m "perf(flac): make m_outputPos persistent, threshold output compaction"
```

---

### Task 3: Fix input compaction — threshold-gate the confirmed-region erase

**Files:**
- Modify: `src/FlacDecoder.cpp:226–234`

**Context:** Read `src/FlacDecoder.cpp` lines 226–234 before editing. The current code:

```cpp
// Compact input buffer: only remove confirmed-consumed bytes.
// Read-ahead bytes (between confirmed pos and m_inputPos) stay in the buffer.
size_t confirmedBufPos = static_cast<size_t>(m_confirmedAbsolutePos - m_tellOffset);
if (confirmedBufPos > 0) {
    m_inputBuffer.erase(m_inputBuffer.begin(),
                        m_inputBuffer.begin() + confirmedBufPos);
    m_inputPos -= confirmedBufPos;
    m_tellOffset += confirmedBufPos;
}
```

The invariant that makes this safe: `confirmedBufPos <= m_inputPos <= m_inputBuffer.size()`.
- `m_confirmedAbsolutePos >= m_tellOffset` always, so the cast is safe.
- `m_inputPos` is the libFLAC read-ahead cursor, always at or ahead of the confirmed boundary.

The problem: the erase fires on every call where `confirmedBufPos > 0`, which is every call after a successful frame decode.

**Step 1: Replace the compact block**

Replace the block starting at `// Compact input buffer:` with:

```cpp
// Compact input buffer: only remove confirmed-consumed bytes.
// Read-ahead bytes (between confirmed pos and m_inputPos) stay in the buffer.
// Invariant: confirmedBufPos <= m_inputPos <= m_inputBuffer.size().
// m_tellOffset advances only when bytes are physically erased — keep the
// three assignments together inside the gate.
size_t confirmedBufPos = static_cast<size_t>(m_confirmedAbsolutePos - m_tellOffset);
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

**Critical**: `m_inputPos -= confirmedBufPos` and `m_tellOffset += confirmedBufPos` must remain **inside** the gate. If they ran unconditionally while the erase did not, `tellCallback` would return a wrong value. The gate controls all three together.

**Step 2: Build**

```bash
cd build && make -j$(nproc)
```

Expected: exits 0. Review terminal output for any `error:` or `warning:` lines before proceeding.

**Step 3: Verify rollback path is unaffected**

Read `src/FlacDecoder.cpp:186–200` (the ABORT branch). It computes:

```cpp
size_t confirmedBufPos = static_cast<size_t>(
    m_confirmedAbsolutePos - m_tellOffset);
m_inputPos = confirmedBufPos;
```

This is a separate local variable — it is the rollback assignment of `m_inputPos`. It does not touch `m_tellOffset`. Confirm this path is unchanged and that deferred compaction (when the gate did not fire) does not affect it: `confirmedBufPos` will still be within bounds by the invariant stated in the comment.

**Step 4: Build full binary and smoke test**

```bash
cd build && make -j$(nproc)
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Play a FLAC track. Confirm:
- Playback starts and sustains without underruns.
- No `[FLAC]` error lines in the log.
- No `[FLAC] Metadata incomplete` spam (unrelated, but confirms the decoder is initialising correctly).

**Step 5: Commit**

```bash
git add src/FlacDecoder.cpp
git commit -m "perf(flac): threshold input buffer compaction in FlacDecoder"
```

---

### Task 4: Final review and optional hi-res test

**Step 1: Review all three changed regions together**

Read the full `readDecoded()` function in `src/FlacDecoder.cpp`. Confirm:

1. Metadata/audio boundary erase (`FlacDecoder.cpp:146`) is still unconditional — correct, it fires once per stream.
2. Input compaction gate fires only when `confirmedBufPos >= INPUT_COMPACT_THRESHOLD_BYTES` and the two-condition heuristic is satisfied.
3. Output compaction gate fires only when `m_outputPos >= OUTPUT_COMPACT_THRESHOLD_SAMPLES` and the two-condition heuristic is satisfied.
4. `m_tellOffset` is updated in exactly three places: the one-shot metadata/audio boundary erase (~line 149) and its fallback path (~line 154) — both unchanged — and inside the new input compaction gate. Confirm no new updates to `m_tellOffset` exist outside these three sites. Do not remove or modify the existing metadata-phase updates; they are required for `tellCallback` to return a correct absolute position after metadata handling.
5. `m_outputPos = 0` is set in exactly one place: inside the output gate.
6. `flush()` still clears both buffers and resets both offsets to zero.

**Step 2: Optional — hi-res stream test**

If a hi-res FLAC source is available (96 kHz or 192 kHz), play it. At hi-res rates, the compressed frame size is larger (~20–40 KB), which means `INPUT_COMPACT_THRESHOLD_BYTES = 16384` is crossed more quickly. Confirm sustained playback and no errors.

**Step 3: No further commits needed unless review reveals an issue.**

---

## What This Does Not Change

- One-shot metadata/audio boundary erase (`FlacDecoder.cpp:146`).
- `m_confirmedAbsolutePos` update after successful frame (`FlacDecoder.cpp:219`).
- ABORT rollback path (`FlacDecoder.cpp:186–200`).
- `tellCallback` implementation (`FlacDecoder.cpp:406`).
- Metadata retry strategy, producer/sender threads, DirettaSync, SPSC ring buffer.

## Follow-on Work (out of scope)

- `feed()` append-side realloc (`FlacDecoder.cpp:68`) — next FLAC optimization candidate.
- Metadata retry decoder churn — requires control-flow changes, separate design.
