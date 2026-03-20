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
- `m_poppedFramesTotal` — new exact consumed-frame counter, incremented only at the real-pop
  site in `getNewStream()`

Per wakeup, the sender computes `poppedFramesDelta = currentPoppedFramesTotal - seenPoppedFramesTotal`
and sends exactly that many frames from the decoded cache. Because the credit unit is exact
consumed frames (computed at the pop site as `currentBytesPerBuffer / m_cachedBytesPerFrame`),
the 44/45-frame cadence at 44.1 kHz is absorbed automatically and the result is correct for
any sink bit depth.

`getBufferLevel()` polling is confined to the fallback recovery path. It is never called in
steady state.

---

## DirettaSync Changes

### `diretta/DirettaSync.h`

Add to the public interface, after the existing `getPopEpoch()` block:

```cpp
// Frame credit counter — incremented at real-pop site only, never on silence/underrun/rebuffering.
// Counts frames in sink format (= input format: bit-depth conversion does not change frame count).
alignas(64) std::atomic<uint64_t> m_poppedFramesTotal{0};

uint64_t getPoppedFramesTotal() const {
    return m_poppedFramesTotal.load(std::memory_order_acquire);
}

bool isRebuffering() const {
    return m_rebuffering.load(std::memory_order_acquire);
}
```

`getPoppedFramesTotal()` uses `memory_order_acquire` to match `getPopEpoch()` semantics.

`getBytesPerBuffer()` and `getFramesPerBuffer()` are not needed: the sender uses frame credits
directly and does not re-derive the SDK frame quantum.

### `diretta/DirettaSync.cpp` — real-pop site in `getNewStream()`

After `m_ringBuffer.pop(dest, currentBytesPerBuffer)`, publish credit in this order:

```cpp
// Credit accounting: compute exact frames consumed for this callback.
// m_cachedBytesPerFrame = channels * bytesPerSample (sink format); same value used to size this pop.
// Guard against zero (brief startup window before format is configured).
if (m_cachedBytesPerFrame > 0) {
    m_poppedFramesTotal.fetch_add(
        static_cast<uint64_t>(currentBytesPerBuffer) /
        static_cast<uint64_t>(m_cachedBytesPerFrame),
        std::memory_order_relaxed);
}
m_popEpoch.fetch_add(1, std::memory_order_release);
```

**Write order is mandatory.** The `memory_order_release` on `m_popEpoch` makes the preceding
relaxed store to `m_poppedFramesTotal` visible to any thread that subsequently acquires
`m_popEpoch`. Reversing the order permits `epochDelta > 0` with a stale frame credit.

`m_poppedFramesTotal` must not advance on silence, underrun, or rebuffering returns — only at
this site. At 44.1 kHz this produces alternating 44/45-frame increments matching the SDK's
own accumulator, which is why using frames (not bytes) is the correct credit unit regardless
of sink bit depth.

---

## Sender State Machine (`src/main.cpp`)

### Initialization

Immediately after `direttaPtr->open(audioFmt)` succeeds:

```cpp
uint64_t seenEpoch             = direttaPtr->getPopEpoch();
uint64_t seenPoppedFramesTotal = direttaPtr->getPoppedFramesTotal();
```

Both initialized from live consumer values after `open()`. If initialized to zero and
pre-existing callbacks have already fired (e.g., format-change reopen), the first delta
computation would produce a spurious large credit.

`firstPopReceived` is not needed. The recovery condition uses `isPrefillComplete()` instead,
which correctly covers both the startup window and post-resume re-prefill (see below).

### Per-Wakeup Loop

```
1. Wait on pop event (waitForPop(2ms) or condvar fallback) — unchanged from current code.
2. Read currentEpoch      = direttaPtr->getPopEpoch()
        currentFramesTotal = direttaPtr->getPoppedFramesTotal()
3. seenEpoch = currentEpoch   // always advance; keeps condvar predicate fresh each iteration
4. If direttaPtr->isPaused(): sleep 100ms, continue.    // preserve current paused behavior
5. Compute poppedFramesDelta = currentFramesTotal - seenPoppedFramesTotal
6. If poppedFramesDelta > 0: run credit path.
7. Else if !direttaPtr->isPrefillComplete()
        || direttaPtr->isRebuffering()
        || producerDone.load(std::memory_order_acquire):
       run fallback recovery.
8. Else: continue waiting.
```

**Note on `isPrefillComplete()` replacing `firstPopReceived`**: `resumePlayback()` clears the
ring buffer and resets `m_prefillComplete = false` (DirettaSync.cpp line 1497). With
`firstPopReceived = true` still set, the original design had no recovery condition that could
fire, deadlocking the sender. `!isPrefillComplete()` is false both at initial startup and
after any resume that resets the prefill gate, which is exactly when recovery injection is
required.

### Credit Path

```cpp
if (poppedFramesDelta > 0) {
    size_t targetCacheFrames = static_cast<size_t>(poppedFramesDelta);

    size_t contiguous   = cacheContiguousFrames();
    size_t framesToSend = std::min(targetCacheFrames, contiguous);
    if (framesToSend > 0) {
        sendChunk(framesToSend);
    }

    // Advance by exactly what was sent, not by the full delta.
    // Any unsent residual (cache wrap boundary, partial sendAudio acceptance)
    // remains in poppedFramesDelta on the next wakeup and is repaid then.
    seenPoppedFramesTotal += framesToSend;
    continue;
}
```

`acceptedFramesFromConsumedPcmBytes()` is **not used** in the credit path. Frame credits from
`m_poppedFramesTotal` are in the same unit as the decoded cache (one frame = one sample point
per channel), so no conversion is needed.

**Residual handling**: `seenPoppedFramesTotal` advances only by `framesToSend`. If
`cacheContiguousFrames()` is smaller than `targetCacheFrames` (circular cache wrap boundary
or partial `sendAudio()` acceptance), the unpaid residual persists as future credit. On the
next wakeup the residual is included in `poppedFramesDelta` and repaid before new pop credits
are stacked on top.

**Invariant**: if the cache has sufficient contiguous frames, each consumer pop is repaid by
the same number of decoded input frames, restoring ring occupancy to exactly its pre-pop
level. If the cache is short, the deficit is a genuine producer/decode lag, not a sender
artefact.

### Fallback Recovery Path

Recovery runs only when there are no new credits and one of the three exceptional conditions
holds. It is no longer the primary pacing mode.

```cpp
constexpr float  RECOVERY_SEND_MAX_MS    = 10.0f;
constexpr size_t RECOVERY_SEND_MIN_FRAMES = 256;

} else if (!direttaPtr->isPrefillComplete() ||
           direttaPtr->isRebuffering() ||
           producerDone.load(std::memory_order_acquire)) {

    bool draining = producerDone.load(std::memory_order_acquire);

    if (!draining) {
        // Startup / rebuffering: check threshold BEFORE sending to prevent overshoot.
        // Injecting past REBUFFER_THRESHOLD_PCT would add unnecessary latency before
        // real pops resume. The SDK resumes real pops once this threshold is crossed.
        if (direttaPtr->getBufferLevel() >= DirettaBuffer::REBUFFER_THRESHOLD_PCT) {
            continue;
        }

        // Cap each recovery send to bound per-iteration ring fill.
        // Without a cap, one wakeup could fill the ring from near-empty to capacity.
        size_t recoveryCapFrames = (rate > 0)
            ? static_cast<size_t>(rate * RECOVERY_SEND_MAX_MS / 1000.0f)
            : RECOVERY_SEND_MIN_FRAMES;

        size_t contiguous = cacheContiguousFrames();
        if (contiguous > 0) {
            sendChunk(std::min(contiguous, recoveryCapFrames));
        }
    } else {
        // Producer done: drain remaining decoded cache into ring without ceiling.
        // Natural-end detection fires once bufferedBytes reaches zero.
        size_t contiguous = cacheContiguousFrames();
        if (contiguous > 0) {
            sendChunk(contiguous);
        }
    }

    // Exit recovery immediately on first resumed real pop (event-driven transition).
    if (direttaPtr->getPopEpoch() != seenEpoch) {
        continue;
    }
}
```

Recovery semantics:
- **Startup**: `m_prefillComplete` is false until the ring holds enough data to start playback.
  Recovery injects in bounded chunks until the threshold is reached and real pops begin.
- **Post-resume**: `resumePlayback()` resets `m_prefillComplete = false` and clears the ring.
  Recovery re-fires and refills the ring before the first real pop of the new play cycle.
- **Rebuffering**: `DirettaSync` intentionally withholds real pops during rebuffering.
  Recovery injects bounded chunks until `getBufferLevel() >= REBUFFER_THRESHOLD_PCT`, at which
  point `DirettaSync` exits rebuffering and real pops resume.
- **Producer-done drain**: flush remaining decoded cache into the ring; natural-end detection
  fires as before once the ring is empty or stuck.

The recovery-to-credit transition is event-driven: the first nonzero `poppedFramesDelta` after
a drought immediately returns control to the credit path. No explicit mode-switch signal is
added.

---

## Properties

| Property | Current | After |
|---|---|---|
| Steady-state pacing signal | `getBufferLevel()` threshold bands | exact `poppedFramesDelta` |
| 44.1 kHz frame drift | ≈24 frames/sec at standard MTU | eliminated |
| Non-32-bit sink repayment | N/A (new path) | correct — frame credit is format-independent |
| `getBufferLevel()` call in hot path | every wakeup (×2) | recovery only |
| Ring occupancy shape | sawtooth 40%–65% | flat near target |
| Recovery trigger | primary mode | fallback for 3 exceptional cases |
| Pause/resume deadlock | not applicable | prevented by `!isPrefillComplete()` |
| Residual credit across cache wrap | N/A | preserved via partial `seenPoppedFramesTotal` advance |
| Recovery overshoot | N/A | bounded per-iteration + threshold check before send |

---

## Verification Criteria

1. **44.1 kHz steady-state**: no long-run ring occupancy drift; level stays flat over a
   multi-minute 44.1 kHz track. Verify by logging `direttaPtr->getBufferedBytes()` at 10-second
   intervals and confirming no monotonic downward trend.

2. **Pause/resume**: after resuming from pause, playback restarts without deadlock. Confirm
   the sender enters recovery (ring is cleared and `isPrefillComplete()` is false after resume),
   refills to the threshold, and credit mode resumes on the first real pop.

3. **Rebuffer episode**: `m_poppedFramesTotal` does not advance during silence returns.
   After rebuffer clears, `seenPoppedFramesTotal` captures the first resumed pop correctly and
   credit mode resumes without a spurious large delta.

4. **Startup**: `seenEpoch` and `seenPoppedFramesTotal` initialized from live consumer values
   after `open()`; no spurious credit on the first wakeup.

5. **Producer-done drain**: final cache drain completes, natural-end detection fires, and
   `STMd`/`STMu` are sent correctly as today.

6. **Non-32-bit sink**: on a target that negotiates 16-bit or 24-bit PCM output, ring occupancy
   does not drift. Credit path uses `m_poppedFramesTotal` (format-independent), not byte-divided
   by `sizeof(int32_t)`.

---

## Files Changed

| File | Change |
|---|---|
| `diretta/DirettaSync.h` | add `m_poppedFramesTotal`, `getPoppedFramesTotal()`, `isRebuffering()` |
| `diretta/DirettaSync.cpp` | increment `m_poppedFramesTotal` (frames, not bytes) before `m_popEpoch` at real-pop site |
| `src/main.cpp` | replace `kRecovery`/`kSteady` controller with credit-first loop; split recovery by `draining` flag |
