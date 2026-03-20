# Pop-Credit Pacing — Design

**Date**: 2026-03-20
**Status**: Approved
**Scope**: `src/main.cpp`, `diretta/DirettaSync.h`, `diretta/DirettaSync.cpp`

---

## Problem

The current PCM sender in `src/main.cpp` uses a threshold-based two-mode controller
(`kRecovery` / `kSteady`) to decide how many frames to push per wakeup. This creates two
structural problems:

1. **Sawtooth ring occupancy.** In `kSteady` mode, the sender pushes a fixed 2ms chunk
   whenever the ring is below `STEADY_CEILING` (65%), then stalls until the ring drains back
   past `STEADY_EXIT` (40%). Ring occupancy oscillates in a 25%-amplitude sawtooth. The sender
   fires in bursts rather than at a cadence matching the consumer.

2. **44.1 kHz fractional drift.** `getNewStream()` alternates between popping 44 and 45 frames
   per cycle via `m_framesPerBufferAccumulator`. A sender using `m_bytesPerBuffer / bytesPerFrame`
   as a fixed quantum sends 44 frames per pop on average but the consumer takes 44.1. This
   produces a slow ring drain (≈24 frames/sec at standard MTU) that eventually causes underrun
   without an external correction term.

Both problems share the same root cause: the sender classifies wakeups by threshold bands
instead of reacting to what the consumer actually consumed.

---

## Solution

Replace the threshold controller with a **credit-first sender** driven by two consumer-owned
monotonic signals:

- `m_popEpoch` — existing wake/event counter, unchanged
- `m_poppedBytesTotal` — new exact consumed-byte counter, incremented only at the real-pop
  site in `getNewStream()`

Per wakeup, the sender computes `poppedBytesDelta = currentPoppedBytesTotal - seenPoppedBytesTotal`
and repays that exact amount from the decoded cache. Because the credit unit is exact consumed
bytes (not a derived frame count), the 44/45-frame cadence at 44.1 kHz is absorbed
automatically with no drift.

`getBufferLevel()` polling is confined to the fallback recovery path. It is never called in
steady state.

---

## DirettaSync Changes

### `diretta/DirettaSync.h`

Add to the public interface:

```cpp
// Credit counter — incremented at real-pop site only, never on silence/underrun/rebuffering.
alignas(64) std::atomic<uint64_t> m_poppedBytesTotal{0};

uint64_t getPoppedBytesTotal() const {
    return m_poppedBytesTotal.load(std::memory_order_acquire);
}

size_t getBytesPerBuffer() const {
    return static_cast<size_t>(m_bytesPerBuffer.load(std::memory_order_acquire));
}

bool isRebuffering() const {
    return m_rebuffering.load(std::memory_order_acquire);
}
```

`getFramesPerBuffer()` and `getRebufferResumeLevel()` are not added. The sender derives
frame counts via `acceptedFramesFromConsumedPcmBytes()` and uses
`DirettaBuffer::REBUFFER_THRESHOLD_PCT` directly.

### `diretta/DirettaSync.cpp` — real-pop site in `getNewStream()`

After `m_ringBuffer.pop(dest, currentBytesPerBuffer)`, publish credit in this order:

```cpp
m_poppedBytesTotal.fetch_add(currentBytesPerBuffer, std::memory_order_relaxed);
m_popEpoch.fetch_add(1, std::memory_order_release);
```

**This order is mandatory.** The sender reads `m_popEpoch` with acquire semantics and uses
`epochDelta > 0 && poppedBytesDelta > 0` as a coherent guard. The `memory_order_release` on
`m_popEpoch` makes the preceding relaxed store to `m_poppedBytesTotal` visible to any thread
that subsequently acquires `m_popEpoch`. Reversing the order permits `epochDelta > 0` with a
stale `poppedBytesDelta`.

`m_poppedBytesTotal` must not advance on silence, underrun, or rebuffering returns — only at
this site.

---

## Sender State Machine (`src/main.cpp`)

### Initialization

Immediately after `direttaPtr->open(audioFmt)` succeeds:

```cpp
uint64_t seenEpoch            = direttaPtr->getPopEpoch();
uint64_t seenPoppedBytesTotal = direttaPtr->getPoppedBytesTotal();
bool     firstPopReceived     = false;
```

Initialization must happen after `open()`, not at zero. If initialized to zero and pre-existing
callbacks have already fired (e.g., format-change reopen), the first delta computation would
produce a spurious large credit.

`firstPopReceived` defines the startup recovery window precisely: from `open()` until the first
real pop credit arrives.

### Per-Wakeup Loop

```
1. Wait on pop event (waitForPop(2ms) or condvar fallback) — unchanged from current code.
2. Read currentEpoch = direttaPtr->getPopEpoch()
        currentPoppedBytes = direttaPtr->getPoppedBytesTotal()
3. If direttaPtr->isPaused(): sleep 100ms, continue.           // preserve current paused behavior
4. Compute epochDelta = currentEpoch - seenEpoch
          poppedBytesDelta = currentPoppedBytes - seenPoppedBytesTotal
5. If epochDelta > 0 && poppedBytesDelta > 0: run credit path.
6. Else if !firstPopReceived
        || direttaPtr->isRebuffering()
        || producerDone.load(std::memory_order_acquire):       // producerDoneDrain
       run fallback recovery.
7. Else: continue waiting.
```

### Credit Path

```cpp
if (epochDelta > 0 && poppedBytesDelta > 0) {
    firstPopReceived = true;

    size_t targetCacheFrames =
        acceptedFramesFromConsumedPcmBytes(
            static_cast<size_t>(poppedBytesDelta),
            detectedChannels);

    size_t contiguous    = cacheContiguousFrames();
    size_t framesToSend  = std::min(targetCacheFrames, contiguous);
    if (framesToSend > 0) {
        sendChunk(framesToSend);
    }

    seenEpoch             = currentEpoch;
    seenPoppedBytesTotal  = currentPoppedBytes;
    continue;
}
```

`acceptedFramesFromConsumedPcmBytes()` from `src/PcmSenderPolicy.cpp` is reused unchanged.
No new byte-to-frame conversion logic is introduced.

**Invariant**: if the cache has sufficient contiguous frames, each consumer pop is repaid by
the same number of input frames, restoring ring occupancy to exactly its pre-pop level.
If the cache is short, the deficit is a genuine producer/decode lag — not a sender artefact.

### Fallback Recovery Path

Recovery runs only when there are no new credits and one of the three exceptional conditions
holds. It is no longer the primary pacing mode.

```cpp
} else if (!firstPopReceived || direttaPtr->isRebuffering() ||
           producerDone.load(std::memory_order_acquire)) {

    size_t contiguous = cacheContiguousFrames();
    if (contiguous > 0) {
        sendChunk(contiguous);   // full catch-up, bounded by ring capacity via sendAudio()
    }

    // Exit recovery immediately on first resumed real pop.
    if (direttaPtr->getPopEpoch() != seenEpoch) {
        continue;
    }

    // Stop injecting once the ring has reached the rebuffer-resume threshold.
    if (direttaPtr->getBufferLevel() >= DirettaBuffer::REBUFFER_THRESHOLD_PCT) {
        continue;
    }
}
```

Recovery semantics:
- **Startup**: inject decoded frames into the ring during the stabilization silence period so
  data is ready when the first real pops begin.
- **Rebuffering**: `DirettaSync` intentionally withholds real pops during rebuffering.
  Recovery injects until `getBufferLevel() >= REBUFFER_THRESHOLD_PCT`, at which point
  `DirettaSync` exits rebuffering and real pops resume.
- **Producer-done drain**: flush remaining decoded cache into the ring; natural-end detection
  fires as before once the ring is empty or stuck.

The recovery-to-credit transition is event-driven: the first nonzero `epochDelta` after a
drought immediately returns control to the credit path. No explicit mode-switch signal is added.

---

## Properties

| Property | Current | After |
|---|---|---|
| Steady-state pacing signal | `getBufferLevel()` threshold bands | exact `poppedBytesDelta` |
| 44.1 kHz frame drift | ≈24 frames/sec at standard MTU | eliminated |
| `getBufferLevel()` call in hot path | every wakeup (×2) | recovery only |
| Ring occupancy shape | sawtooth 40%–65% | flat near target |
| Recovery trigger | primary mode | fallback for 3 exceptional cases |
| Sender-side accumulator | none | none |
| New threshold constants | none | none |

---

## Verification Criteria

1. **44.1 kHz steady-state**: no long-run ring occupancy drift; level stays flat over a
   multi-minute 44.1 kHz track. Verify by logging `direttaPtr->getBufferedBytes()` at 10-second
   intervals and confirming no monotonic downward trend.

2. **Pause/resume**: sender remains in the explicit 100ms paused sleep, not the 2ms pop-wait
   loop. Confirm no spurious credit path entries while paused.

3. **Rebuffer episode**: `m_poppedBytesTotal` does not advance during silence returns.
   After rebuffer clears, `seenPoppedBytesTotal` captures the first resumed pop correctly and
   credit mode resumes without a spurious large delta.

4. **Startup**: `seenEpoch` and `seenPoppedBytesTotal` initialized from live consumer values
   after `open()`; no spurious credit on the first wakeup.

5. **Producer-done drain**: final cache drain completes, natural-end detection fires, and
   `STMd`/`STMu` are sent correctly as today.

---

## Files Changed

| File | Change |
|---|---|
| `diretta/DirettaSync.h` | add `m_poppedBytesTotal`, `getPoppedBytesTotal()`, `getBytesPerBuffer()`, `isRebuffering()` |
| `diretta/DirettaSync.cpp` | increment `m_poppedBytesTotal` before `m_popEpoch` at real-pop site |
| `src/main.cpp` | replace `kRecovery`/`kSteady` controller with credit-first loop |
