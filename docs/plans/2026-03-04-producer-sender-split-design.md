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

**Unit: frames throughout.** `sendAudio()` takes frames in the PCM path; keeping counters
in frames avoids a `* channels` multiply in the hot path and keeps accounting consistent.

```cpp
alignas(64) std::atomic<size_t> writeSeq{0};  // producer ONLY writes, counts frames
alignas(64) std::atomic<size_t> readSeq{0};   // sender   ONLY writes, counts frames
std::vector<int32_t> decodeCache;             // existing buffer, indexed by sample
                                              // (frame * channels + ch)
```

`alignas(64)` prevents false sharing between producer and sender cache lines.
Existing `decodeCache` capacity and `%`-based indexing are unchanged.

### Producer side

```cpp
size_t w = 0;  // local frame counter — never accessed by sender
// per iteration:
size_t r     = readSeq.load(std::memory_order_acquire);         // frames
size_t free  = capacity_frames - (w - r);                       // frames
// write decoded frames into decodeCache[(w % capacity_frames) * channels ...]
writeSeq.store(w += written_frames, std::memory_order_release);
```

### Sender side

```cpp
size_t r = 0;  // local frame counter — never accessed by producer
// per iteration:
size_t w     = writeSeq.load(std::memory_order_acquire);        // frames
size_t avail = w - r;                                           // frames
// read frames from decodeCache[(r % capacity_frames) * channels ...]
// call sendAudio(ptr, consumed_frames)
readSeq.store(r += consumed_frames, std::memory_order_release);
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

Epoch is incremented unconditionally before the `try_lock`. Even if `try_lock` fails
(sender briefly holds mutex during predicate re-check), the epoch change is already
visible. On the next predicate evaluation — whether from a spurious wakeup or the 2ms
timeout — the sender sees `epoch != seenEpoch` and unblocks. The 2ms timeout is
worst-case latency only for the try_lock-miss edge case.

Note: `notify_one()` does not acquire `m_flowMutex`. The `try_lock` gate exists solely
to avoid any blocking on the time-critical DAC callback thread. If the callback budget
allows even a brief `notify_one()` call unconditionally, the `try_lock` can be dropped
entirely — the epoch scheme already prevents lost wakeups, making the timeout fallback
very rare.

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

    size_t w     = writeSeq.load(std::memory_order_acquire);
    size_t avail = w - r;                         // frames
    if (avail == 0) continue;

    size_t chunk = std::min(avail, MAX_DECODE_FRAMES);   // frames
    // pop chunk frames from decodeCache → sendAudio(ptr, chunk)
    readSeq.store(r += chunk, std::memory_order_release);
}
```

### Stop / flush

On stop or stream flush, set `audioTestRunning = false`, then wake the sender:

```cpp
audioTestRunning.store(false, std::memory_order_release);
direttaPtr->notifySpaceAvailable();  // sender predicate sees !audioTestRunning → exits
```

The sender exits via the predicate (`!audioTestRunning`) on the next wakeup — either
from the notify or from the 2ms timeout fallback. No lock block is needed here;
correctness relies on the predicate, not on synchronizing with the mutex.

---

## Shared State Between Producer and Sender

| Variable | Unit | Owner | Other side |
|---|---|---|---|
| `writeSeq` | frames | producer writes (release) | sender reads (acquire) |
| `readSeq` | frames | sender writes (release) | producer reads (acquire) |
| `decodeCache[]` | samples | producer writes `[w%cap * ch..]` | sender reads `[r%cap * ch..]` — non-overlapping |
| `audioFmt` | — | producer writes once (before `audioFmtReady`) | sender reads after flag |
| `dopDetected` | — | producer writes once | sender reads after `audioFmtReady` |
| `audioFmtReady` | — | producer sets (release) | sender polls (acquire) |
| `prebufferReady` | — | producer sets (release) | sender polls — gates `direttaPtr->open()` |
| `httpEof` | — | producer sets (release) | sender: exit when `httpEof && avail == 0` |
| `audioTestRunning` | — | outer audio thread sets | both threads read |

---

## Files Changed

| File | Change |
|---|---|
| `src/main.cpp` | Split audio lambda into producer thread + sender thread; replace cache index vars with `writeSeq`/`readSeq` (frames); add `audioFmtReady`, `prebufferReady`, `httpEof` atomics; stop path calls `notifySpaceAvailable()` |
| `diretta/DirettaSync.h` | Add `m_popEpoch` atomic + `getPopEpoch()`; add predicate overload of `waitForSpace` |
| `diretta/DirettaSync.cpp` | Add `m_popEpoch.fetch_add` before `try_lock` in SDK pop callback |
