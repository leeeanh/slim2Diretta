# Pop-Credit Pacing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the threshold-based `kRecovery`/`kSteady` PCM sender controller in `src/main.cpp` with a credit-first loop driven by exact consumed-frame accounting from `DirettaSync`.

**Architecture:** `DirettaSync` exposes a new monotonic `m_poppedFramesTotal` counter incremented at the real-pop site in `getNewStream()` as `currentBytesPerBuffer / m_cachedBytesPerFrame`. The sender computes `poppedFramesDelta` per wakeup and repays that many decoded frames. Fallback recovery handles the three exceptional cases where the consumer deliberately withholds pops: startup/post-resume (detected via `!isPrefillComplete()`), rebuffering, and producer-done drain.

**Design doc:** `docs/plans/2026-03-20-pop-credit-pacing-design.md`

**Tech Stack:** C++17, `std::atomic`, no new helpers — frame credits are used directly in the sender loop.

---

### Task 1: Add `m_poppedFramesTotal` and accessors to `DirettaSync`

**Files:**
- Modify: `diretta/DirettaSync.h:525-529` (Flow Control section, after `getPopEpoch`)

**Step 1: Add the frame credit counter and two accessors**

In `diretta/DirettaSync.h`, after the existing `getPopEpoch()` block (line 529), insert:

```cpp
    alignas(64) std::atomic<uint64_t> m_poppedFramesTotal{0};

    uint64_t getPoppedFramesTotal() const {
        return m_poppedFramesTotal.load(std::memory_order_acquire);
    }

    bool isRebuffering() const {
        return m_rebuffering.load(std::memory_order_acquire);
    }
```

The insertion point is the blank line after `getPopEpoch()` at line 529, before the `//=== Target Management ===` comment at line 531.

`m_poppedFramesTotal` uses `alignas(64)` to give it its own cache line, matching `m_popEpoch` at line 525. `getPoppedFramesTotal()` uses `memory_order_acquire` to match `getPopEpoch()` semantics.

`getConsumedBytesPerFrame()` is **not** added. After the 16-bit fallback is removed from
`configureSinkPCM()` (Task 2, Step 2), `sendAudio()` consistently returns input bytes for all
reachable paths (32-bit direct and pack24bit), so `acceptedFramesFromConsumedPcmBytes()` is
already correct — no change to `sendChunk()` is required.

**Step 2: Verify it compiles**

```bash
cd /path/to/slim2Diretta/build && make -j$(nproc) > /tmp/slim2d_build.log 2>&1 \
    && echo "Build OK" \
    || { tail -20 /tmp/slim2d_build.log; echo "Build FAILED"; exit 1; }
```

Expected: "Build OK", no new warnings in the log.

**Step 3: Commit**

```bash
git add diretta/DirettaSync.h
git commit -m "feat(diretta): add m_poppedFramesTotal and isRebuffering accessors"
```

---

### Task 2: Increment `m_poppedFramesTotal` at the real-pop site

**Files:**
- Modify: `diretta/DirettaSync.cpp:1837-1841` (real-pop site in `getNewStream()`)

**Step 1: Insert the frame credit increment before the epoch release**

In `diretta/DirettaSync.cpp`, the real-pop site currently reads (lines 1837–1841):

```cpp
    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    // Epoch incremented unconditionally before try_lock — allows sender to
    // detect pops even when it holds the mutex (avoiding lost wakeups)
    m_popEpoch.fetch_add(1, std::memory_order_release);
```

Replace with:

```cpp
    // Pop from ring buffer
    m_ringBuffer.pop(dest, currentBytesPerBuffer);

    // Credit accounting: publish exact frames consumed before the epoch release.
    // m_cachedBytesPerFrame = channels * bytesPerSample (sink format).
    // Dividing here gives exact 44 or 45 frames at 44.1 kHz, correct for any sink bit depth.
    // Guard against zero (brief startup window before format is configured).
    // Write order is mandatory: relaxed store before release fence on m_popEpoch.
    if (m_cachedBytesPerFrame > 0) {
        m_poppedFramesTotal.fetch_add(
            static_cast<uint64_t>(currentBytesPerBuffer) /
            static_cast<uint64_t>(m_cachedBytesPerFrame),
            std::memory_order_relaxed);
    }
    m_popEpoch.fetch_add(1, std::memory_order_release);
```

`m_poppedFramesTotal` is incremented here and **nowhere else**. It must not advance on silence, underrun, or rebuffering returns — all of which return before reaching this line.

**Step 2: Remove the 16-bit fallback from `configureSinkPCM()`**

In `diretta/DirettaSync.cpp`, inside `configureSinkPCM()` at lines 1136–1142, delete:

```cpp
    fmt.setFormat(DIRETTA::FormatID::FMT_PCM_SIGNED_16);
    if (checkSinkSupport(fmt)) {
        setSinkConfigure(fmt);
        acceptedBits = 16;
        DIRETTA_LOG("Sink PCM: " << rate << "Hz " << channels << "ch 16-bit");
        return;
    }
```

The 16-bit direct path in `sendAudio()` calls `m_ringBuffer.push(data, frames × ch × 2)`,
which copies only the low two bytes of each int32_t sample — not a 32→16 downconversion.
No `push32To16()` exists in DirettaRingBuffer. With this removal, sinks that reject both
32-bit and 24-bit receive the existing `throw std::runtime_error("No supported PCM format
found")` rather than producing corrupt audio. No 16-bit-only target was ever producing
correct output through this path.

**Step 3: Verify it compiles**

```bash
make -j$(nproc) > /tmp/slim2d_build.log 2>&1 \
    && echo "Build OK" \
    || { tail -20 /tmp/slim2d_build.log; echo "Build FAILED"; exit 1; }
```

Expected: "Build OK".

**Step 4: Commit**

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(diretta): increment m_poppedFramesTotal; remove broken 16-bit sink fallback"
```

---

### Task 3: Remove `SenderMode` and update post-open initialization

**Files:**
- Modify: `src/main.cpp:1242-1254` (remove `SenderMode` enum and constants)
- Modify: `src/main.cpp:1327` (replace `seenEpoch` initialization)

**Step 1: Remove `SenderMode` enum, threshold constants, and the mode variable**

Lines 1242–1254 currently read:

```cpp
                    enum class SenderMode { kRecovery, kSteady };
                    constexpr float  STEADY_ENTER              = 0.75f;
                    constexpr float  STEADY_EXIT               = 0.40f;
                    constexpr float  STEADY_CEILING            = 0.65f;
                    constexpr float  STEADY_CHUNK_MS           = 2.0f;
                    constexpr size_t STEADY_CHUNK_MIN_FRAMES   = 128;
                    constexpr size_t STEADY_CHUNK_MAX_FRAMES   = 512;
                    constexpr float  DEEP_RECOVERY_THRESHOLD   = 0.20f;
                    constexpr float  RECOVERY_CHUNK_MS         = 5.0f;
                    constexpr float  DEEP_RECOVERY_CHUNK_MS    = 10.0f;
                    constexpr size_t RECOVERY_CHUNK_MIN_FRAMES = 128;
                    constexpr size_t RECOVERY_CHUNK_MAX_FRAMES = 1024;
                    SenderMode senderMode = SenderMode::kRecovery;
```

Replace all 13 lines with the two recovery constants used in the new loop:

```cpp
                    constexpr float  RECOVERY_SEND_MAX_MS     = 10.0f;
                    constexpr size_t RECOVERY_SEND_MIN_FRAMES = 256;
```

**Step 2: Replace the post-open initialization**

Line 1327 currently reads:

```cpp
                        uint64_t seenEpoch = direttaPtr->getPopEpoch();
```

Replace with:

```cpp
                        uint64_t seenEpoch, seenPoppedFramesTotal;
                        do {
                            seenEpoch             = direttaPtr->getPopEpoch();
                            seenPoppedFramesTotal = direttaPtr->getPoppedFramesTotal();
                        } while (direttaPtr->getPopEpoch() != seenEpoch);
```

`open()` calls `play()` before returning, so pops can race the two loads. If a pop lands
between them, `seenEpoch` holds epoch N while `seenPoppedFramesTotal` already includes pop
N+1. The first sender iteration wakes on the epoch change but computes `poppedFramesDelta == 0`,
permanently dropping one credit. The retry loop re-reads until the epoch is unchanged across
both loads, guaranteeing a consistent pair. Typically executes once; at most a handful of
retries even under load.

`firstPopReceived` is not needed: the recovery condition uses `!direttaPtr->isPrefillComplete()`
which covers both the initial startup window and the post-resume re-prefill.

**Step 3: Verify it compiles** (will fail until Task 4 removes the `senderMode` uses)

```bash
make -j$(nproc) > /tmp/slim2d_build.log 2>&1 \
    && echo "Build OK" \
    || { grep "error:" /tmp/slim2d_build.log | head -20; echo "Build FAILED"; exit 1; }
```

Expected: errors referencing `senderMode`, `STEADY_ENTER`, etc. — fixed in Task 4.

---

### Task 4: Replace the sender loop body with the credit-first controller

**Files:**
- Modify: `src/main.cpp:1425-1468` (post-wait block inside the sender `while` loop)

**Step 1: Remove the blind `seenEpoch` overwrite and the threshold controller**

Lines 1425–1468 currently read (the block starting with `seenEpoch = direttaPtr->getPopEpoch();` through the closing `}` of the `else` branch). Replace the entire block with:

```cpp
                            uint64_t currentEpoch       = direttaPtr->getPopEpoch();
                            uint64_t currentFramesTotal = direttaPtr->getPoppedFramesTotal();

                            // Always advance seenEpoch so the condvar predicate stays fresh
                            // for the next wait. This is independent of credit accounting.
                            seenEpoch = currentEpoch;

                            if (direttaPtr->isPaused()) {
                                // Discard credits accumulated during the paused window.
                                // resumePlayback() clears the ring but m_poppedFramesTotal is
                                // monotonic — without this sync the first post-resume wakeup
                                // repays pre-pause pops into a fresh ring, over-injecting.
                                seenPoppedFramesTotal = currentFramesTotal;
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                continue;
                            }

                            // Call before the credit/recovery branch so it is reached on every
                            // non-paused wakeup. The credit path's early continue would otherwise
                            // skip it in the steady-state case, freezing track progress reporting.
                            if (audioFmtReady.load(std::memory_order_relaxed)) {
                                updateElapsed();
                            }

                            uint64_t poppedFramesDelta = currentFramesTotal - seenPoppedFramesTotal;

                            if (poppedFramesDelta > 0) {
                                // Credit path: frame credits are format-independent.
                                // No byte-to-frame conversion needed.
                                size_t targetCacheFrames = static_cast<size_t>(poppedFramesDelta);

                                size_t contiguous   = cacheContiguousFrames();
                                size_t framesToSend = std::min(targetCacheFrames, contiguous);
                                size_t actualSent   = 0;
                                if (framesToSend > 0) {
                                    actualSent = sendChunk(framesToSend);
                                }

                                // Advance only by frames actually accepted by sendAudio().
                                // sendChunk() can accept fewer than requested if the ring is
                                // temporarily overfilled (e.g. from a recovery burst).
                                // Advancing by the full request would discard the shortfall,
                                // reintroducing drift. Residual persists and is repaid next wakeup.
                                seenPoppedFramesTotal += actualSent;
                                continue;

                            } else if (!direttaPtr->isPrefillComplete() ||
                                       direttaPtr->isRebuffering() ||
                                       producerDone.load(std::memory_order_acquire)) {
                                // Fallback recovery: consumer is deliberately withholding pops.
                                // !isPrefillComplete() covers both initial startup and post-resume:
                                // resumePlayback() clears the ring and resets m_prefillComplete,
                                // so the sender correctly re-enters recovery after every unpause.

                                // Gate on threshold only during genuine steady-state rebuffering
                                // (isRebuffering() true AND isPrefillComplete() true). Two cases
                                // where this gate must NOT fire:
                                //   1. Startup / post-resume (!isPrefillComplete()): FLAC prefill
                                //      target is ~40% of ring; stopping at 20% deadlocks startup.
                                //   2. Pause-during-rebuffer: resumePlayback() resets
                                //      m_prefillComplete but does NOT clear m_rebuffering, so
                                //      isRebuffering() can still be true post-resume. Without
                                //      isPrefillComplete() in the guard, post-resume recovery
                                //      stops at 20% and getNewStream() keeps returning silence
                                //      until m_prefillTarget (potentially 40%) is reached —
                                //      permanently stalling playback.
                                // The threshold gate does NOT apply to the producer-done drain
                                // case — when draining, credits have already stopped and the ring
                                // must be filled to let natural-end detection fire.
                                if (!producerDone.load(std::memory_order_acquire) &&
                                        direttaPtr->isRebuffering() &&
                                        direttaPtr->isPrefillComplete() &&
                                        direttaPtr->getBufferLevel() >=
                                        DirettaBuffer::REBUFFER_THRESHOLD_PCT) {
                                    continue;
                                }

                                // Cap per-iteration send to bound ring fill per wakeup.
                                // Applied uniformly to startup, rebuffering, AND producer-done drain.
                                // The uncapped drain of the original design could bulk-fill the ring
                                // in one 2ms wakeup when producerDone fires with a sizeable cache
                                // tail, delaying natural-end detection and gapless handoff by up to
                                // a full ring duration. With the cap, the drain completes across
                                // several wakeups at consumer pace — no end-of-track latency added.
                                size_t recoveryCapFrames = (rate > 0)
                                    ? static_cast<size_t>(rate * RECOVERY_SEND_MAX_MS / 1000.0f)
                                    : RECOVERY_SEND_MIN_FRAMES;

                                size_t contiguous = cacheContiguousFrames();
                                if (contiguous > 0) {
                                    sendChunk(std::min(contiguous, recoveryCapFrames));
                                }

                                // Exit recovery immediately on first resumed real pop.
                                if (direttaPtr->getPopEpoch() != seenEpoch) {
                                    continue;
                                }
                            }
                            // else: no credits, not in an exceptional state — wait for next pop.
```

**Step 2: Remove the now-duplicate `updateElapsed()` call at line 1471**

The replacement block in Step 1 adds `updateElapsed()` before the credit/recovery branch.
The original code has a second call at lines 1470–1472, immediately after the replaced block:

```cpp
                            if (audioFmtReady.load(std::memory_order_relaxed)) {
                                updateElapsed();
                            }
```

This line is **outside** the replaced range (1425–1468), so it stays unless explicitly removed.
With both calls present, `updateElapsed()` would fire twice per non-paused wakeup (once in
the new block, once from this leftover), doubling the progress-reporting cadence.

Delete those three lines (the `if (audioFmtReady...) { updateElapsed(); }` block at 1470–1472).

**Step 3: Verify it compiles cleanly**

```bash
make -j$(nproc) > /tmp/slim2d_build.log 2>&1 \
    && echo "Build OK" \
    || { grep "error:" /tmp/slim2d_build.log | head -20; echo "Build FAILED"; exit 1; }
```

Expected: "Build OK", zero new warnings. The `senderMode`, `STEADY_ENTER`, etc. references from the old controller were the only consumers of the removed declarations.

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "feat(sender): replace kRecovery/kSteady with pop-credit pacing controller"
```

---

### Task 5: Verify the condvar pop-wait predicate still compiles correctly

**Files:**
- Inspect: `src/main.cpp:1415-1420`

The condvar fallback wait predicate (non-EVL path) currently references `seenEpoch`:

```cpp
                                    direttaPtr->waitForSpace(lock,
                                        [&]() {
                                            return direttaPtr->getPopEpoch() != seenEpoch ||
                                                   !audioTestRunning.load(std::memory_order_acquire);
                                        },
                                        std::chrono::milliseconds(2));
```

In the new design `seenEpoch` is advanced unconditionally at the top of each iteration (step 3 in the per-wakeup sequence), after the wait returns. While waiting, `seenEpoch` holds the value written at the end of the previous iteration, so the predicate correctly detects pops that arrive during the current sleep. Semantics are unchanged. **No edit needed here.**

Verify by inspection that the predicate compiles and that `seenEpoch` still refers to the local declared in Task 3 Step 2:

```bash
make -j$(nproc) 2>&1 | grep -i "seenEpoch\|error"
```

Expected: no errors.

---

### Task 6: Runtime verification

No automated test harness exists (see `CLAUDE.md`). Verify manually against the six criteria from the design doc.

**Criterion 1 — 44.1 kHz steady-state: no ring occupancy leak**

Run a multi-minute 44.1 kHz FLAC track with `--verbose`. Observe `[Sender]` log lines (emitted every 500 iterations). Confirm `direttaBytes` does not trend monotonically downward over 5+ minutes.

```
[Sender] iter=500 ... direttaBytes=<N>
[Sender] iter=1000 ... direttaBytes=<N ± small variance>
```

**Criterion 2 — Pause/resume: no deadlock, playback restarts**

Pause via LMS, wait 5 seconds, resume. Confirm:
- Playback restarts without hanging
- `[Sender]` recovery log entries appear after resume (ring cleared, `!isPrefillComplete()` fires)
- Credit mode resumes after the first real pop following re-prefill

**Criterion 3 — Rebuffer episode: recovery injects correctly, threshold is respected**

Simulate a network stall. Confirm:
- `[DirettaSync] Buffer underrun — entering rebuffering mode` appears
- Recovery injects in bounded chunks (not one bulk fill)
- `[DirettaSync] Rebuffering complete` appears
- Playback continues without permanent stall
- `m_poppedFramesTotal` does not advance during silence returns (credit mode does not fire during the stall)

**Criterion 4 — Startup: no spurious initial credit**

Start a track with verbose logging. Confirm the credit path fires only after the first `[DirettaSync] getNewStream #N` with a real pop. No credit-path entries during the stabilization silence period.

**Criterion 5 — Producer-done drain: natural end fires**

Play a short track to completion. Confirm:
- `[Sender] Natural stream end declared` appears
- `STMd` and `STMu` stats are sent (visible as LMS track advance)
- No hang at end of track

**Criterion 6 — Non-32-bit sink: no ring drift**

If a 16-bit or 24-bit output target is available, play a 44.1 kHz track for several minutes. Confirm `direttaBytes` stays flat (same as Criterion 1). The frame credit unit is format-independent: this verifies no under-repayment on non-32-bit sinks.

**Step: Final commit if any runtime fixups were made**

```bash
git add -p
git commit -m "fix(sender): <describe any runtime fixup>"
```
