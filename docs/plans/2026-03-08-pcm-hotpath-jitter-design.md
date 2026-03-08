# PCM Hot-Path Jitter Reduction — Design

**Date**: 2026-03-08
**Status**: Approved
**Scope**: `src/PcmDecoder.cpp/h`, `src/main.cpp`, `diretta/DirettaSync.h`

---

## Problem

Following the producer/sender split (commit `9647162`), the PCM pipeline still has
three burst-generation sources that add scheduling noise and increase underrun risk:

1. `PcmDecoder::readDecoded` — O(n) `vector::erase` after every call shifts remaining
   buffer contents, creating CPU spikes proportional to buffered data size.
2. Producer HTTP reads — always requests 64 KB regardless of cache state, driving
   bursty decode-cache fill.
3. Sender refill policy — fills the Diretta ring to 95% on every wakeup, turning
   bursty cache input into bursty ring output.

The root cause is that HTTP/TCP delivers PCM in bursts; all three issues compound that
burstiness downstream rather than absorbing it.

---

## Design

### Fix 1: PcmDecoder — read-offset instead of erase

**Files**: `src/PcmDecoder.h`, `src/PcmDecoder.cpp`

Add `size_t m_readPos = 0` to `PcmDecoder`. Replace `erase` with offset advancement:

```cpp
// Instead of: m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + bytesToConvert);
m_readPos += bytesToConvert;
```

Compact only when reclaiming the front is worthwhile:

```cpp
if (m_readPos >= COMPACT_THRESHOLD &&
    (m_readPos >= m_dataBuf.size() / 2 || (m_dataBuf.size() - m_readPos) < COMPACT_THRESHOLD)) {
    m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + m_readPos);
    m_readPos = 0;
}
```

`COMPACT_THRESHOLD = 65536` bytes.

**Invariant**:
- Unread data starts at `m_dataBuf.data() + m_readPos`
- Available bytes = `m_dataBuf.size() - m_readPos`
- All pointer and size arithmetic in `readDecoded` and `feedData` adjusts by `m_readPos`

**Reset policy**:
- `flush()` and any full stream reinit: `m_readPos = 0`
- `setEof()`: no reset — `setEof` is only an end-of-input flag; resetting here would
  corrupt unread buffered PCM

---

### Fix 2: Adaptive HTTP read sizing (producer side)

**File**: `src/main.cpp` — producer loop (~L995)

Read size is determined by decoded-cache fill ratio. Codec-agnostic: uses cache
occupancy, not format-derived byte estimates.

```cpp
constexpr size_t HTTP_READ_MIN    =  4096;
constexpr size_t HTTP_READ_STEADY =  8192;
constexpr size_t HTTP_READ_HIGH   = 16384;
constexpr size_t HTTP_READ_BURST  = 65536;

size_t readSize;
if (!prebufferReady) {
    readSize = HTTP_READ_BURST;
} else {
    size_t free  = cacheFreeFrames();
    size_t total = DECODE_CACHE_MAX_SAMPLES / std::max(detectedChannels, 1);
    float fillRatio = 1.0f - static_cast<float>(free) / static_cast<float>(total);

    if      (fillRatio > 0.80f) readSize = HTTP_READ_MIN;
    else if (fillRatio > 0.50f) readSize = HTTP_READ_STEADY;
    else                        readSize = HTTP_READ_HIGH;
}
// Pass readSize instead of sizeof(httpBuf) to httpStream->read()
```

**Properties**:
- `HttpStreamClient` contract unchanged — honors caller's `maxLen` as before.
- Burst-size reads are reserved for startup/prebuffer phase.
- Steady-state maximum is 16 KB, a 4× reduction from current 64 KB.
- `detectedChannels` defaults to 2 before format detection — acceptable startup
  approximation, consistent with existing code.

---

### Fix 3: Two-mode sender refill policy

**Files**: `src/main.cpp` (sender loop), `diretta/DirettaSync.h` (new accessor)

#### New DirettaSync accessor

```cpp
// Returns base callback frame count: rate / 1000 (integer division).
// Does NOT expose the remainder-accumulator state — base frames are
// sufficient and stable for sender pacing purposes.
size_t getFramesPerBuffer() const {
    return static_cast<size_t>(m_sampleRate.load(std::memory_order_relaxed)) / 1000;
}
```

#### Sender state machine

```cpp
enum class SenderMode { kRecovery, kSteady };

constexpr float RECOVERY_TARGET = 0.90f;  // fill to here in recovery
constexpr float STEADY_ENTER    = 0.75f;  // enter steady once ring is above this
constexpr float STEADY_EXIT     = 0.40f;  // drop back to recovery below this
constexpr int   STEADY_CHUNKS_PER_WAKEUP = 2;

SenderMode senderMode = SenderMode::kRecovery;  // start in recovery

// --- Per-wakeup logic (replaces current getBufferLevel() <= 0.95f loop) ---

// 1. State transition — once per wakeup, outside all inner loops
float level = direttaPtr->getBufferLevel();
if (senderMode == SenderMode::kRecovery && level >= STEADY_ENTER)
    senderMode = SenderMode::kSteady;
else if (senderMode == SenderMode::kSteady && level < STEADY_EXIT)
    senderMode = SenderMode::kRecovery;

// 2. Fill decision for this wakeup
if (senderMode == SenderMode::kRecovery) {
    while (audioTestRunning.load(std::memory_order_relaxed) &&
           direttaPtr->getBufferLevel() < RECOVERY_TARGET) {
        size_t contiguous = cacheContiguousFrames();
        if (contiguous == 0) break;
        sendChunk(std::min(contiguous, MAX_DECODE_FRAMES));
    }
} else {
    size_t frameCap = STEADY_CHUNKS_PER_WAKEUP * direttaPtr->getFramesPerBuffer();
    size_t framesSent = 0;
    while (audioTestRunning.load(std::memory_order_relaxed) &&
           framesSent < frameCap) {
        size_t contiguous = cacheContiguousFrames();
        if (contiguous == 0) break;
        size_t chunk = std::min(contiguous, frameCap - framesSent);
        sendChunk(chunk);
        framesSent += chunk;
    }
}
```

**Properties**:
- Hysteresis gap (enter=0.75, exit=0.40) prevents mode flap under Linux scheduling noise.
- Steady cap uses `getFramesPerBuffer()` — matches the actual SDK drain quantum,
  no dependence on sink packing format in `main.cpp`.
- Recovery mode retains fast catch-up for startup, post-stall, and rebuffer.
- Automatic recovery re-entry when ring falls below 40%.
- State transition is checked once per wakeup — no mid-burst mode changes.

---

## Summary of Changes

| File | Change |
|------|--------|
| `src/PcmDecoder.h` | Add `m_readPos`, `COMPACT_THRESHOLD` |
| `src/PcmDecoder.cpp` | Replace `erase` with offset+conditional compact; adjust all pointer/size arithmetic |
| `src/main.cpp` | Adaptive HTTP read sizing in producer; two-mode sender state machine |
| `diretta/DirettaSync.h` | Add `getFramesPerBuffer()` accessor |

---

## What This Does Not Change

- `HttpStreamClient` interface — no internal capping or policy changes.
- SPSC ring buffer or producer/sender thread structure.
- DirettaSync internal drain path or SDK callback logic.
- FLAC path — Fix 1 is PcmDecoder-only; Fix 2 is codec-agnostic by design.
