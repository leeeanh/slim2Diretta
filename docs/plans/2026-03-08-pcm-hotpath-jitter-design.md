# PCM Hot-Path Jitter Reduction — Design

**Date**: 2026-03-08
**Status**: Approved (revised after plan review)
**Scope**: `src/PcmDecoder.cpp/h`, `src/main.cpp`

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

**File**: `src/main.cpp` (sender loop only — no DirettaSync changes)

#### Why not a frame-count cap

An earlier draft capped steady-mode fills using `getFramesPerBuffer()` from
`DirettaSync`. This was rejected for three reasons:

1. **DoP frame unit mismatch** — `sendChunk(N)` sends N sender-cache frames. For DoP,
   each carrier frame is expanded 16× inside `sendAudio()`. Sink callback frame counts
   don't map back to sender frames without knowing the conversion factor.
2. **Stale `m_sampleRate` in DSD mode** — `configureRingDSD()` does not update
   `m_sampleRate`, so any accessor based on it returns a PCM-era value in DSD mode.
3. **Ring creep** — with 2 × framesPerBuffer sent per wakeup (one wakeup per SDK pop),
   average fill rate exceeds drain rate and the ring fills over time. Even 1× only
   keeps up exactly, with no tolerance for a missed wakeup.

#### Approach: level ceiling + rate-derived chunk size

The ring level already answers "should I send?" more directly than frame arithmetic.
Steady mode sends one small chunk per wakeup, gated by a fill ceiling. Chunk size is
derived from the sender's own rate — `dopPcmRate` for DoP, `snapshotSampleRate` for
PCM — which is always in sender-cache frame units and requires no DirettaSync format
knowledge.

#### Sender state machine

```cpp
enum class SenderMode { kRecovery, kSteady };

constexpr float  RECOVERY_TARGET         = 0.90f;  // fill to here in recovery
constexpr float  STEADY_ENTER            = 0.75f;  // enter steady once ring is above this
constexpr float  STEADY_EXIT             = 0.40f;  // drop back to recovery below this
constexpr float  STEADY_CEILING          = 0.65f;  // max fill allowed in steady mode
constexpr float  STEADY_CHUNK_MS         = 2.0f;   // target chunk duration (ms)
constexpr size_t STEADY_CHUNK_MIN_FRAMES = 128;
constexpr size_t STEADY_CHUNK_MAX_FRAMES = 512;

SenderMode senderMode = SenderMode::kRecovery;

// --- Per-wakeup logic ---

// 1. State transition — once per wakeup, outside all fill loops
float level = direttaPtr->getBufferLevel();
if (senderMode == SenderMode::kRecovery && level >= STEADY_ENTER)
    senderMode = SenderMode::kSteady;
else if (senderMode == SenderMode::kSteady && level < STEADY_EXIT)
    senderMode = SenderMode::kRecovery;

// 2. Fill decision for this wakeup
if (senderMode == SenderMode::kRecovery) {
    // Aggressive fill: used at startup, after stalls, and after seek/rebuffer
    while (audioTestRunning.load(std::memory_order_relaxed) &&
           direttaPtr->getBufferLevel() < RECOVERY_TARGET) {
        size_t contiguous = cacheContiguousFrames();
        if (contiguous == 0) break;
        sendChunk(std::min(contiguous, MAX_DECODE_FRAMES));
    }
} else {
    // Steady: one rate-derived chunk per wakeup, gated by fill ceiling.
    // Chunk size is in sender-cache frame units — codec-agnostic and DoP-safe.
    uint32_t rate = dopDetected ? dopPcmRate : snapshotSampleRate;
    size_t steadyChunkFrames = (rate > 0)
        ? std::clamp(
              static_cast<size_t>(rate * STEADY_CHUNK_MS / 1000.0f),
              STEADY_CHUNK_MIN_FRAMES,
              STEADY_CHUNK_MAX_FRAMES)
        : STEADY_CHUNK_MIN_FRAMES;

    if (direttaPtr->getBufferLevel() < STEADY_CEILING) {
        size_t contiguous = cacheContiguousFrames();
        if (contiguous > 0)
            sendChunk(std::min(contiguous, steadyChunkFrames));
    }
}
```

**Properties**:
- Hysteresis gap (enter=0.75, exit=0.40) prevents mode flap under Linux scheduling noise.
- Steady ceiling (0.65) prevents ring creep without frame-count arithmetic.
- Chunk duration ~2 ms across rates: 88 frames @ 44.1k, 192 @ 96k, 384 @ 192k — all
  within the 128..512 clamp.
- DoP-safe: `dopPcmRate` is the carrier PCM rate, which is already in sender-cache
  frame units matching the SPSC ring.
- Recovery mode retains fast catch-up for startup, post-stall, and rebuffer.
- State transition is checked once per wakeup — no mid-burst mode changes.
- No changes to `DirettaSync` required.

---

## Summary of Changes

| File | Change |
|------|--------|
| `src/PcmDecoder.h` | Add `m_readPos`, `COMPACT_THRESHOLD` |
| `src/PcmDecoder.cpp` | Replace `erase` with offset+conditional compact; adjust all pointer/size arithmetic; reset `m_readPos` in `flush()` |
| `src/main.cpp` | Adaptive HTTP read sizing in producer; two-mode sender state machine |

---

## What This Does Not Change

- `HttpStreamClient` interface — no internal capping or policy changes.
- SPSC ring buffer or producer/sender thread structure.
- DirettaSync internal drain path or SDK callback logic.
- FLAC path — Fix 1 is PcmDecoder-only; Fix 2 is codec-agnostic by design.
- `DirettaSync` — no changes required; the initial `getFramesPerBuffer()` accessor
  design was rejected because `configureRingDSD()` does not update `m_sampleRate`
  and because sink callback frame counts don't map to sender-cache frame units for DoP.
