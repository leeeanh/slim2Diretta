# Pop-Credit Pacing Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the threshold-based `kRecovery`/`kSteady` PCM sender controller in `src/main.cpp` with a credit-first loop driven by exact consumed-byte accounting from `DirettaSync`.

**Architecture:** `DirettaSync` exposes a new monotonic `m_poppedBytesTotal` counter incremented only at the real-pop site in `getNewStream()`. The sender computes `poppedBytesDelta` per wakeup and repays that exact number of decoded frames. Fallback recovery handles the three exceptional cases where the consumer deliberately withholds pops: startup, rebuffering, and producer-done drain.

**Design doc:** `docs/plans/2026-03-20-pop-credit-pacing-design.md`

**Tech Stack:** C++17, `std::atomic`, existing `acceptedFramesFromConsumedPcmBytes()` from `src/PcmSenderPolicy.h`

---

### Task 1: Add `m_poppedBytesTotal` and accessors to `DirettaSync`

**Files:**
- Modify: `diretta/DirettaSync.h:525-529` (Flow Control section, after `getPopEpoch`)

**Step 1: Add the credit counter and three accessors**

In `diretta/DirettaSync.h`, after the existing `getPopEpoch()` block (line 529), insert:

```cpp
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

The exact insertion point is the blank line after the closing `}` of `getPopEpoch()` at line 529, before the `//=== Target Management ===` comment at line 531.

`m_poppedBytesTotal` uses `alignas(64)` to give it its own cache line, matching the pattern of `m_popEpoch` at line 525.

`getPoppedBytesTotal()` uses `memory_order_acquire` to match `getPopEpoch()` semantics. The sender reads `m_popEpoch` first (acquire), then `m_poppedBytesTotal` (acquire); both orderings are required for the `epochDelta > 0 && poppedBytesDelta > 0` guard to be coherent.

**Step 2: Verify it compiles**

```bash
cd /path/to/slim2Diretta/build && make -j$(nproc) 2>&1 | head -40
```

Expected: clean build, no new warnings.

**Step 3: Commit**

```bash
git add diretta/DirettaSync.h
git commit -m "feat(diretta): add m_poppedBytesTotal credit counter and accessors"
```

---

### Task 2: Increment `m_poppedBytesTotal` at the real-pop site

**Files:**
- Modify: `diretta/DirettaSync.cpp:1837-1841` (real-pop site in `getNewStream()`)

**Step 1: Insert the credit increment before the epoch release**

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

    // Credit accounting: publish exact consumed bytes before the epoch release.
    // The release fence on m_popEpoch makes this relaxed store visible to any
    // thread that acquires m_popEpoch. Order is mandatory — do not swap.
    m_poppedBytesTotal.fetch_add(currentBytesPerBuffer, std::memory_order_relaxed);
    m_popEpoch.fetch_add(1, std::memory_order_release);
```

`m_poppedBytesTotal` is incremented here and **nowhere else**. It must not advance on silence, underrun, or rebuffering returns — all of which return before reaching this line.

**Step 2: Verify it compiles**

```bash
make -j$(nproc) 2>&1 | head -40
```

Expected: clean build.

**Step 3: Commit**

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(diretta): increment m_poppedBytesTotal at real-pop site in getNewStream"
```

---

### Task 3: Remove `SenderMode` and update post-open initialization

**Files:**
- Modify: `src/main.cpp:1242-1254` (remove `SenderMode` enum and constants)
- Modify: `src/main.cpp:1327` (expand `seenEpoch` initialization)

**Step 1: Remove `SenderMode` enum, all threshold constants, and the mode variable**

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

Delete all 13 lines. The blank line at 1255 can stay.

**Step 2: Expand the post-open initialization**

Line 1327 currently reads:

```cpp
                        uint64_t seenEpoch = direttaPtr->getPopEpoch();
```

Replace with:

```cpp
                        uint64_t seenEpoch            = direttaPtr->getPopEpoch();
                        uint64_t seenPoppedBytesTotal = direttaPtr->getPoppedBytesTotal();
                        bool     firstPopReceived     = false;
```

This initialization happens after `direttaPtr->open(audioFmt)` succeeds (line 1314) and must not be moved earlier. Initializing from the live consumer values prevents a spurious large `poppedBytesDelta` on the first wakeup if pre-existing callbacks have already fired.

**Step 3: Verify it compiles** (will fail until Task 4 removes the `senderMode` uses)

```bash
make -j$(nproc) 2>&1 | grep "error:"
```

Expected: errors referencing `senderMode`, `STEADY_ENTER`, etc. — this is expected and will be fixed in Task 4.

---

### Task 4: Replace the sender loop body with the credit-first controller

**Files:**
- Modify: `src/main.cpp:1425-1468` (post-wait block inside the sender `while` loop)

**Step 1: Remove the blind `seenEpoch` overwrite and the threshold controller**

Lines 1425–1468 currently read:

```cpp
                            seenEpoch = direttaPtr->getPopEpoch();

                            if (direttaPtr->isPaused()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                continue;
                            }

                            // State transition once per wakeup with hysteresis to prevent mode flap.
                            float level = direttaPtr->getBufferLevel();
                            if (senderMode == SenderMode::kRecovery && level >= STEADY_ENTER) {
                                senderMode = SenderMode::kSteady;
                            } else if (senderMode == SenderMode::kSteady && level < STEADY_EXIT) {
                                senderMode = SenderMode::kRecovery;
                            }

                            if (senderMode == SenderMode::kRecovery) {
                                // level was already sampled for the mode transition — reuse it.
                                float chunkMs = (level < DEEP_RECOVERY_THRESHOLD)
                                    ? DEEP_RECOVERY_CHUNK_MS : RECOVERY_CHUNK_MS;
                                size_t chunkFrames = (rate > 0)
                                    ? std::clamp(
                                          static_cast<size_t>(rate * chunkMs / 1000.0f),
                                          RECOVERY_CHUNK_MIN_FRAMES,
                                          RECOVERY_CHUNK_MAX_FRAMES)
                                    : RECOVERY_CHUNK_MIN_FRAMES;
                                size_t contiguous = cacheContiguousFrames();
                                if (contiguous > 0) {
                                    sendChunk(std::min(contiguous, chunkFrames));
                                }
                            } else {
                                size_t steadyChunkFrames = (rate > 0)
                                    ? std::clamp(
                                          static_cast<size_t>(rate * STEADY_CHUNK_MS / 1000.0f),
                                          STEADY_CHUNK_MIN_FRAMES,
                                          STEADY_CHUNK_MAX_FRAMES)
                                    : STEADY_CHUNK_MIN_FRAMES;

                                if (direttaPtr->getBufferLevel() < STEADY_CEILING) {
                                    size_t contiguous = cacheContiguousFrames();
                                    if (contiguous > 0) {
                                        sendChunk(std::min(contiguous, steadyChunkFrames));
                                    }
                                }
                            }
```

Replace with:

```cpp
                            uint64_t currentEpoch       = direttaPtr->getPopEpoch();
                            uint64_t currentPoppedBytes = direttaPtr->getPoppedBytesTotal();

                            if (direttaPtr->isPaused()) {
                                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                                continue;
                            }

                            uint64_t epochDelta       = currentEpoch - seenEpoch;
                            uint64_t poppedBytesDelta = currentPoppedBytes - seenPoppedBytesTotal;

                            if (epochDelta > 0 && poppedBytesDelta > 0) {
                                // Credit path: repay exactly what the consumer popped.
                                // poppedBytesDelta is in output-format bytes (post-conversion).
                                // acceptedFramesFromConsumedPcmBytes converts to decoded-cache frames.
                                firstPopReceived = true;

                                size_t targetCacheFrames =
                                    acceptedFramesFromConsumedPcmBytes(
                                        static_cast<size_t>(poppedBytesDelta),
                                        detectedChannels);

                                size_t contiguous   = cacheContiguousFrames();
                                size_t framesToSend = std::min(targetCacheFrames, contiguous);
                                if (framesToSend > 0) {
                                    sendChunk(framesToSend);
                                }

                                seenEpoch            = currentEpoch;
                                seenPoppedBytesTotal = currentPoppedBytes;

                            } else if (!firstPopReceived ||
                                       direttaPtr->isRebuffering() ||
                                       producerDone.load(std::memory_order_acquire)) {
                                // Fallback recovery: consumer is deliberately withholding pops
                                // (startup silence, rebuffering, or producer-done drain).
                                // Inject from cache until the consumer resumes real pops.
                                size_t contiguous = cacheContiguousFrames();
                                if (contiguous > 0) {
                                    sendChunk(contiguous);
                                }

                                // Exit recovery immediately on first real pop.
                                if (direttaPtr->getPopEpoch() != seenEpoch) {
                                    continue;
                                }

                                // Stop injecting once ring is at rebuffer-resume threshold;
                                // consumer will resume real pops at this level.
                                if (direttaPtr->getBufferLevel() >=
                                        DirettaBuffer::REBUFFER_THRESHOLD_PCT) {
                                    continue;
                                }
                            }
                            // else: no credits, not in an exceptional state — wait for next pop.
```

**Step 2: Verify it compiles cleanly**

```bash
make -j$(nproc) 2>&1 | head -40
```

Expected: zero errors and zero new warnings. The `senderMode`, `STEADY_ENTER`, etc. references were the only users of the removed declarations.

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

This predicate uses `seenEpoch` to detect new pops while waiting. In the new design `seenEpoch` is a `uint64_t` local updated only in the credit path — the same variable, same semantics. **No change is needed here.** Verify by inspection that the predicate still compiles and references the same `seenEpoch` declared at line 1327.

```bash
make -j$(nproc) 2>&1 | grep -i "seenEpoch\|error"
```

Expected: no errors; `seenEpoch` resolves to the local from Task 3.

---

### Task 6: Runtime verification

No automated test harness exists (see `CLAUDE.md`). Verify manually against the five criteria from the design doc.

**Criterion 1 — 44.1 kHz steady-state: no ring occupancy leak**

Run a multi-minute 44.1 kHz FLAC track with `--verbose`. Observe `[Sender]` log lines (emitted every 500 iterations). Confirm `direttaBytes` does not trend monotonically downward over 5+ minutes.

```
[Sender] iter=500 ... direttaBytes=<N>
[Sender] iter=1000 ... direttaBytes=<N ± small variance>
```

**Criterion 2 — Pause/resume: 100ms sleep, not 2ms poll**

Pause playback via LMS. Confirm `[Sender]` log lines stop appearing at the 500-iteration cadence (they would appear every ~1s at 2ms poll; they should be absent for the full pause duration at 100ms sleep).

**Criterion 3 — Rebuffer episode: credit resumes cleanly**

Simulate a network stall by pausing the LMS HTTP stream momentarily (or throttle the network). Confirm:
- `[DirettaSync] Buffer underrun — entering rebuffering mode` appears in logs
- After recovery: `[DirettaSync] Rebuffering complete` appears
- Playback continues without a permanent stall
- `m_poppedBytesTotal` does not advance during the silence period (verify by adding a temporary `LOG_DEBUG` after the credit-path entry check: it should not fire during rebuffering)

**Criterion 4 — Startup: no spurious initial credit**

Enable verbose logging and start a track. Confirm the credit path fires only after the first `[DirettaSync] getNewStream #N` with a real pop (not during stabilization silence). There should be no `[Sender]` credit-path log on the very first wakeup before any real pop.

**Criterion 5 — Producer-done drain: natural end fires**

Play a short track to completion. Confirm:
- `[Sender] Natural stream end declared` appears in logs
- `STMd` and `STMu` stats are sent (visible as LMS track advance)
- No hang at end of track

**Step: Final commit if any runtime fixups were made**

```bash
git add -p
git commit -m "fix(sender): <describe any runtime fixup>"
```
