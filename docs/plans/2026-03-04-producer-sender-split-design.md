# Producer/Sender Split — PCM Pipeline Design

**Date**: 2026-03-04
**Status**: Approved
**Scope**: `src/main.cpp`, `diretta/DirettaSync.cpp`, `diretta/DirettaSync.h`

---

## Problem

The current PCM audio path runs HTTP read, decode, and `sendAudio()` in a single thread
(`main.cpp:833–1089`). A decode burst or network stall in Phase 1a/1b directly delays
Phase 4 (`sendAudio`), adding jitter to the data feed into `DirettaRingBuffer`.

The Diretta SDK worker already runs at the hardware DAC clock rate. The goal is to make
the `sendAudio` side equally deterministic by decoupling it from decode latency.

---

## Architecture

```
[Audio thread]
    spawns:
        [Producer thread]   HTTP read → decoder → SPSC ring (writeSeq)
        [Sender thread]     SPSC ring (readSeq) → sendAudio() → DirettaRingBuffer
                            wakes on SDK pop epoch (hardware-clock-driven)

[DirettaSync SDK worker]    DirettaRingBuffer → DAC (hardware clock)
                            increments m_popEpoch on each pop → wakes Sender
```

The audio thread itself only spawns and joins the two sub-threads, then handles
`audioThreadDone` / STMo notification as before.

---

## SPSC Ring Buffer

Replace `cacheReadPos` / `cacheWritePos` / `cacheSamplesAvail` (three shared variables,
two writers) with two single-owner monotonic sequence counters.

```cpp
alignas(64) std::atomic<size_t> writeSeq{0};  // producer ONLY writes
alignas(64) std::atomic<size_t> readSeq{0};   // sender   ONLY writes
std::vector<int32_t> decodeCache;             // unchanged, power-of-2 capacity
```

`alignas(64)` prevents false sharing between producer and sender cache lines.
Capacity must be a power of 2 so indexing uses `& (capacity - 1)` instead of `%`.

### Producer side

```cpp
size_t w = 0;  // local — never accessed by sender
// per iteration:
size_t r   = readSeq.load(std::memory_order_acquire);
size_t free = capacity - (w - r);
// write samples into decodeCache[w & (capacity-1)]
writeSeq.store(w += written, std::memory_order_release);
```

### Sender side

```cpp
size_t r = 0;  // local — never accessed by producer
// per iteration:
size_t w   = writeSeq.load(std::memory_order_acquire);
size_t avail = w - r;
// read samples from decodeCache[r & (capacity-1)]
readSeq.store(r += consumed, std::memory_order_release);
```

One acquire load + one release store per iteration on each side. No shared-write
variable, no cache-line ping-pong.

---

## Epoch-Based Flow Control

### DirettaSync changes

Add to `DirettaSync.h`:

```cpp
alignas(64) std::atomic<uint64_t> m_popEpoch{0};
uint64_t getPopEpoch() const {
    return m_popEpoch.load(std::memory_order_acquire);
}
```

In `DirettaSync.cpp` SDK pop callback, before the existing `try_lock`:

```cpp
m_ringBuffer.pop(dest, currentBytesPerBuffer);
m_popEpoch.fetch_add(1, std::memory_order_release);  // always increments
if (m_flowMutex.try_lock()) {                        // best-effort notify
    m_flowMutex.unlock();
    m_spaceAvailable.notify_one();
}
```

Epoch is incremented unconditionally. Even if `try_lock` fails (sender briefly holds
mutex during predicate re-check), the epoch change is visible on the next predicate
evaluation. The 2ms timeout is worst-case latency only for the try_lock-miss edge case.

### Sender wait loop

```cpp
uint64_t seenEpoch = direttaPtr->getPopEpoch();

while (audioTestRunning.load(std::memory_order_acquire)) {
    {
        std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
        direttaPtr->waitForSpace(lock,
            [&]{
                return direttaPtr->getPopEpoch() != seenEpoch
                    || !audioTestRunning.load(std::memory_order_acquire);
            },
            std::chrono::milliseconds(2));
    }
    seenEpoch = direttaPtr->getPopEpoch();

    size_t w = writeSeq.load(std::memory_order_acquire);
    size_t avail = w - r;
    if (avail == 0) continue;

    size_t chunk = std::min(avail, MAX_DECODE_FRAMES);
    // pop from decodeCache → sendAudio()
    readSeq.store(r += chunk, std::memory_order_release);
}
```

### Stop / flush

On stop or stream flush, after setting `audioTestRunning = false`:

```cpp
{
    std::lock_guard<std::mutex> lk(direttaPtr->getFlowMutex());
    // ensures sender is not mid-predicate-check
}
direttaPtr->notifySpaceAvailable();  // wakes sender immediately, exits wait
```

Sender predicate sees `audioTestRunning == false` → exits without waiting for timeout.

---

## Shared State Between Producer and Sender

| Variable | Owner | Other side |
|---|---|---|
| `writeSeq` | producer writes (release) | sender reads (acquire) |
| `readSeq` | sender writes (release) | producer reads (acquire) |
| `decodeCache[]` | producer writes `[w&mask]` | sender reads `[r&mask]` — non-overlapping |
| `audioFmt` | producer writes once (before `audioFmtReady`) | sender reads after flag |
| `dopDetected` | producer writes once | sender reads after `audioFmtReady` |
| `audioFmtReady` | producer sets (release) | sender polls (acquire) |
| `prebufferReady` | producer sets (release) | sender polls — gates `direttaPtr->open()` |
| `httpEof` | producer sets (release) | sender: exit when `httpEof && avail == 0` |
| `audioTestRunning` | outer audio thread | both threads read |

---

## Files Changed

| File | Change |
|---|---|
| `src/main.cpp` | Split audio lambda into producer thread + sender thread; replace cache index vars with `writeSeq`/`readSeq`; add `audioFmtReady`, `prebufferReady`, `httpEof` atomics; stop path calls `notifySpaceAvailable()` |
| `diretta/DirettaSync.h` | Add `m_popEpoch` atomic + `getPopEpoch()`; add predicate overload of `waitForSpace` |
| `diretta/DirettaSync.cpp` | Add `m_popEpoch.fetch_add` before `try_lock` in SDK pop callback |

---

## Optional Improvements (not blocking)

- Drop `try_lock` in SDK callback → plain `notify_one()`: epoch scheme makes timeout
  fallback rare; removing `try_lock` simplifies code and eliminates the missed-notify
  case entirely (safe if SDK callback budget allows a brief mutex acquisition).
- Power-of-2 `decodeCache` capacity: enforced in constructor, enables `& (capacity-1)`
  index masking.
