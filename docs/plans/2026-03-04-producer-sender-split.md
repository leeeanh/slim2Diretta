# Producer/Sender Split — PCM Pipeline Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Decouple HTTP/decode from DirettaSync pushes in the PCM path by splitting the single audio loop into a producer thread (HTTP + decode → SPSC ring) and a sender thread (SPSC ring → sendAudio, woken by hardware epoch signal).

**Architecture:** Two SPSC monotonic counters (`writeSeq` / `readSeq`, frame units, `alignas(64)`) replace the current three shared cache variables. The sender blocks on a predicate-based `waitForSpace` driven by `m_popEpoch` incremented in the SDK worker pop callback — giving hardware-clock-rate wakeups with no software timer jitter. DSD path is untouched.

**Tech Stack:** C++17, pthreads, existing `DirettaSync` flow-control API (`getFlowMutex`, `waitForSpace`, `notifySpaceAvailable`), libFLAC

---

## Context: What changes and what doesn't

- **Untouched:** DSD path (`FORMAT_DSD`, `main.cpp:534–738`), `HttpStreamClient`, all decoders, `SlimprotoClient`, `DirettaRingBuffer`.
- **Changed:** `DirettaSync.h` / `.cpp` (epoch), PCM branch of audio lambda (`main.cpp:745–1192`).
- **Key constants:**
  - `DECODE_CACHE_MAX_SAMPLES = 3072000` — stays as allocated size (samples)
  - `DECODE_CACHE_CAPACITY_FRAMES = 3072000 / 2 = 1536000` — max stereo frames in ring
  - `MAX_DECODE_FRAMES = 1024` — max frames per push/pop chunk
  - `PREBUFFER_MS = 500`

---

## Task 1: Add `m_popEpoch` to DirettaSync

**Files:**
- Modify: `diretta/DirettaSync.h`
- Modify: `diretta/DirettaSync.cpp`

**Step 1: Add epoch atomic and accessor to DirettaSync.h**

Find the block containing `m_flowMutex` (around line 553). Add immediately after `getFlowMutex()` / `waitForSpace` / `notifySpaceAvailable` declarations (around line 455):

```cpp
/**
 * @brief Epoch counter incremented on each SDK worker pop.
 * Allows sender thread to detect pops without relying solely on
 * try_lock + notify_one (which can miss when sender holds the mutex).
 */
alignas(64) std::atomic<uint64_t> m_popEpoch{0};

uint64_t getPopEpoch() const {
    return m_popEpoch.load(std::memory_order_acquire);
}
```

**Step 2: Increment epoch in SDK pop callback (DirettaSync.cpp:1727)**

Find the comment `// G1: Signal producer that space is now available` (line ~1729). Add one line immediately after `m_ringBuffer.pop(dest, currentBytesPerBuffer)`:

```cpp
// Existing:
m_ringBuffer.pop(dest, currentBytesPerBuffer);

// Add this line — epoch incremented unconditionally before try_lock:
m_popEpoch.fetch_add(1, std::memory_order_release);

// Existing try_lock block follows unchanged:
if (m_flowMutex.try_lock()) {
    m_flowMutex.unlock();
    m_spaceAvailable.notify_one();
}
```

**Step 3: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Expected: clean build, zero warnings about new code.

**Step 4: Commit**

```bash
git add diretta/DirettaSync.h diretta/DirettaSync.cpp
git commit -m "feat(diretta): add m_popEpoch epoch counter for lost-wakeup-free sender sync"
```

---

## Task 2: Add predicate overload of `waitForSpace`

**Files:**
- Modify: `diretta/DirettaSync.h` (around line 444)

**Step 1: Add predicate template overload**

The existing `waitForSpace` is a simple timeout wait. Add a predicate overload immediately after it:

```cpp
/**
 * @brief Wait for buffer space with a stop predicate.
 * Wakes when predicate returns true OR timeout expires.
 * Use this to avoid sleeping past a stop/flush signal.
 *
 * @param pred  Callable returning bool; checked on each wakeup.
 *              Returns true = stop waiting immediately.
 */
template<typename Rep, typename Period, typename Pred>
bool waitForSpace(std::unique_lock<std::mutex>& lock,
                  Pred pred,
                  std::chrono::duration<Rep, Period> timeout) {
    return m_spaceAvailable.wait_for(lock, timeout, pred);
}
```

**Step 2: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Expected: clean build.

**Step 3: Commit**

```bash
git add diretta/DirettaSync.h
git commit -m "feat(diretta): add predicate overload of waitForSpace for epoch-driven wakeup"
```

---

## Task 3: Replace cache index variables with SPSC monotonic counters

**Files:**
- Modify: `src/main.cpp` (PCM branch only, lines ~777–831)

This task restructures the accounting without splitting threads yet. Behavior must be identical after this step — the producer and sender run in the same loop, just using the new counter design.

**Step 1: Replace cache state variables (main.cpp ~779–781)**

Replace:
```cpp
size_t cacheReadPos = 0;
size_t cacheWritePos = 0;
size_t cacheSamplesAvail = 0;
```

With:
```cpp
// SPSC ring: sample-unit monotonic counters.
// Counting in samples (not frames) preserves the original DECODE_CACHE_MAX_SAMPLES
// depth for all channel counts. Frame-unit helpers convert at the boundary.
// Producer owns w (local). Sender owns r (local).
// Only published via atomic store on each commit.
alignas(64) std::atomic<size_t> writeSeq{0};  // producer publishes (samples)
alignas(64) std::atomic<size_t> readSeq{0};   // sender  publishes (samples)
size_t w = 0;  // producer-local sample counter
size_t r = 0;  // sender-local  sample counter (same thread for now)
```

**Step 2: Replace cache helper lambdas (main.cpp ~794–831)**

Remove all six existing lambdas (`cacheFrames`, `cacheFreeSamples`, `cacheContiguousSamples`, `cacheContiguousFrames`, `cacheReadPtr`, `cachePushSamples`, `cachePopFrames`) and replace with:

```cpp
// --- Producer-side helpers (use local w, read readSeq) ---
// cacheFreeFrames: before format detection uses detectedChannels default (2).
// Same behaviour as original code which also defaulted to 2.
auto cacheFreeFrames = [&]() -> size_t {
    int ch = std::max(detectedChannels, 1);
    size_t r_snap = readSeq.load(std::memory_order_acquire);
    return (DECODE_CACHE_MAX_SAMPLES - (w - r_snap)) / static_cast<size_t>(ch);
};
// Push decoded frames into SPSC ring. Returns frames actually written.
auto cachePushFrames = [&](const int32_t* src, size_t frames) -> size_t {
    int ch = std::max(detectedChannels, 1);
    size_t samples = frames * static_cast<size_t>(ch);
    size_t r_snap  = readSeq.load(std::memory_order_acquire);
    size_t free    = DECODE_CACHE_MAX_SAMPLES - (w - r_snap);
    size_t toWrite = std::min(samples, free);
    if (toWrite == 0) return 0;
    size_t pos  = w % DECODE_CACHE_MAX_SAMPLES;
    size_t toEnd = DECODE_CACHE_MAX_SAMPLES - pos;
    size_t first = std::min(toWrite, toEnd);
    std::memcpy(decodeCache.data() + pos,  src,         first   * sizeof(int32_t));
    if (toWrite > first)
        std::memcpy(decodeCache.data(),    src + first, (toWrite - first) * sizeof(int32_t));
    writeSeq.store(w += toWrite, std::memory_order_release);
    return toWrite / static_cast<size_t>(ch);  // frames written
};

// --- Sender-side helpers (use local r, read writeSeq) ---
auto cacheAvailFrames = [&]() -> size_t {
    int ch = std::max(detectedChannels, 1);
    return (writeSeq.load(std::memory_order_acquire) - r) / static_cast<size_t>(ch);
};
auto cacheContiguousFrames = [&]() -> size_t {
    int ch    = std::max(detectedChannels, 1);
    size_t pos = r % DECODE_CACHE_MAX_SAMPLES;
    size_t toEnd = (DECODE_CACHE_MAX_SAMPLES - pos) / static_cast<size_t>(ch);
    return std::min(cacheAvailFrames(), toEnd);
};
auto cacheReadPtr = [&]() -> const int32_t* {
    return decodeCache.data() + (r % DECODE_CACHE_MAX_SAMPLES);
};
auto cachePopFrames = [&](size_t frames) {
    int ch = std::max(detectedChannels, 1);
    readSeq.store(r += frames * static_cast<size_t>(ch), std::memory_order_release);
};
```

**Step 3: Update all call sites in the main loop and drain**

Replace every call to the old lambdas with the new ones. Mapping:

| Old call | New call |
|---|---|
| `cacheFrames()` | `cacheAvailFrames()` |
| `cacheFreeSamples()` | `cacheFreeFrames() * ch` — no longer needed; use `cacheFreeFrames()` directly |
| `cachePushSamples(decodeBuf, frames * ch)` | `cachePushFrames(decodeBuf, frames)` |
| `cachePopFrames(chunk)` | unchanged (still takes frames) |
| `cacheReadPtr()` | unchanged signature |
| `cacheContiguousFrames()` | unchanged signature |

Specifically, in Phase 1b (decoder drain, ~line 864):
```cpp
// Old:
size_t maxFrames = std::min(MAX_DECODE_FRAMES, cacheFreeSamples() / channels);
...
size_t written = cachePushSamples(decodeBuf, samplesToWrite);
if (written != samplesToWrite) { ... }

// New:
size_t maxFrames = std::min(MAX_DECODE_FRAMES, cacheFreeFrames());
...
size_t written = cachePushFrames(decodeBuf, frames);
if (written != frames) { ... }
```

And in the drain section (~line 1100):
```cpp
// Old:
size_t maxFrames = std::min(MAX_DECODE_FRAMES, cacheFreeSamples() / channels);
size_t written = cachePushSamples(decodeBuf, samplesToWrite);
if (written != samplesToWrite) { ... }

// New:
size_t maxFrames = std::min(MAX_DECODE_FRAMES, cacheFreeFrames());
size_t written = cachePushFrames(decodeBuf, frames);
if (written != frames) { ... }
```

Remove `cachedSamples` local variable at line 840 (no longer needed — use `cacheAvailFrames()` directly in the condition check).

**Step 4: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Expected: clean build, no warnings.

**Step 5: Smoke test — play a track end-to-end**

```bash
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Play one track. Verify: stream starts, plays to completion, STMd/STMu sent, no crashes.

**Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "refactor(audio): replace shared cache index vars with SPSC writeSeq/readSeq frame counters"
```

---

## Task 4: Add shared-state atomics for inter-thread signalling

**Files:**
- Modify: `src/main.cpp` (PCM branch, after decoder setup ~line 763)

These atomics are the handshake between producer and sender. Add them immediately before `slimproto->sendStat(StatEvent::STMs)` (line 765):

**Step 1: Add shared state declarations**

```cpp
// --- Shared state: producer writes once, sender reads after flag ---
// Classic C++ publication idiom: non-atomic writes are safe when sequenced
// before the release store (audioFmtReady), and non-atomic reads happen
// after the corresponding acquire load in the sender.
std::atomic<bool> audioFmtReady{false};
std::atomic<bool> prebufferReady{false};
std::atomic<bool> producerDone{false};      // producer sets when HTTP+decode finished
std::atomic<bool> senderOpenedDiretta{false}; // sender sets after direttaPtr->open() succeeds

// Snapshots published by producer before audioFmtReady.store(true, release).
// Sender reads these after seeing audioFmtReady — no decoder access needed.
uint32_t snapshotSampleRate = 0;   // audioFmt.sampleRate, published via audioFmtReady
uint32_t snapshotTotalSec   = 0;   // fmt.totalSamples / sampleRate (0 if unknown)

// detectedChannels: producer writes once before audioFmtReady, sender reads after.
// Plain int is safe via the audioFmtReady release/acquire pair.
// (dopDetected, dopPcmRate, dopBuf, audioFmt already declared below — keep as-is.)
```

**Step 2: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Expected: clean build (variables unused until Task 5/6, compiler may warn — suppress with `(void)` if needed, or just verify no errors).

**Step 3: Commit**

```bash
git add src/main.cpp
git commit -m "refactor(audio): add inter-thread signalling atomics for producer/sender split"
```

---

## Task 5: Extract producer sub-thread

**Files:**
- Modify: `src/main.cpp` (PCM branch, lines ~833–1089 + drain ~1091–1176)

The producer thread owns: Phase 1a (HTTP read), Phase 1b (decoder drain → `cachePushFrames`), Phase 2 (format detection), and the post-EOF decoder drain. It does NOT call `sendAudio`.

**Step 1: Wrap Phase 1a + 1b + 2 + drain in a producer thread**

Inside the PCM lambda (after the shared state declarations, before the existing main while-loop), add:

```cpp
std::thread producerThread([&]() {
    // Loop exits as soon as HTTP EOF is reached.
    // Does NOT wait for the sender to drain — that is sender's responsibility.
    bool httpEof = false;
    while (audioTestRunning.load(std::memory_order_acquire) && !httpEof) {

        // ---- Phase 1a: HTTP read ----
        bool gotData = false;
        if (cacheFreeFrames() > 0) {
            if (httpStream->isConnected()) {
                ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 2);
                if (n > 0) {
                    gotData = true;
                    totalBytes += n;
                    slimproto->updateStreamBytes(totalBytes);
                    decoder->feed(httpBuf, static_cast<size_t>(n));
                } else if (n < 0 || !httpStream->isConnected()) {
                    httpEof = true;
                    decoder->setEof();
                }
            } else {
                httpEof = true;
                decoder->setEof();
            }
        }

        // ---- Phase 1b: Drain decoder → SPSC ring ----
        while (true) {
            size_t free = cacheFreeFrames();
            if (free == 0) break;
            size_t maxFrames = std::min(MAX_DECODE_FRAMES, free);
            size_t frames = decoder->readDecoded(decodeBuf, maxFrames);
            if (frames == 0) break;
            size_t written = cachePushFrames(decodeBuf, frames);
            if (written != frames) {
                LOG_WARN("[Producer] Cache full while draining decoder");
                break;
            }
        }

        // ---- Phase 2: Format detection (one-shot) ----
        if (!formatLogged && decoder->isFormatReady()) {
            formatLogged = true;
            auto fmt = decoder->getFormat();
            LOG_INFO("[Audio] Decoding: " << fmt.sampleRate << " Hz, "
                     << fmt.bitDepth << "-bit, " << fmt.channels << " ch");
            detectedChannels  = fmt.channels;
            audioFmt.sampleRate   = fmt.sampleRate;
            audioFmt.bitDepth     = 32;
            audioFmt.channels     = fmt.channels;
            audioFmt.isCompressed = (formatCode == FORMAT_FLAC ||
                                     formatCode == FORMAT_MP3  ||
                                     formatCode == FORMAT_OGG  ||
                                     formatCode == FORMAT_AAC);
            // Publish snapshots for sender (no decoder access after this point)
            snapshotSampleRate = fmt.sampleRate;
            snapshotTotalSec   = (fmt.totalSamples > 0 && fmt.sampleRate > 0)
                                 ? static_cast<uint32_t>(fmt.totalSamples / fmt.sampleRate)
                                 : 0;
            audioFmtReady.store(true, std::memory_order_release);
        }

        // ---- Prebuffer threshold: signal sender when enough frames ready ----
        // Force prebufferReady on httpEof so sender is never blocked by a
        // short stream that never reaches the target frame count.
        if (audioFmtReady.load(std::memory_order_relaxed) &&
            !prebufferReady.load(std::memory_order_relaxed)) {
            size_t targetFrames =
                static_cast<size_t>(snapshotSampleRate) * PREBUFFER_MS / 1000;
            if (cacheAvailFrames() >= targetFrames || httpEof) {
                prebufferReady.store(true, std::memory_order_release);
            }
        }

        // Anti-busy-loop: only when genuinely idle (no data, waiting for HTTP)
        if (!gotData && !httpEof) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }

        if (decoder->hasError()) {
            LOG_ERROR("[Producer] Decoder error");
            break;
        }
    }

    // Post-EOF decoder drain: push remaining decoder frames into ring.
    // Ring may be full — yield 1ms and retry rather than spinning.
    // Does NOT wait for sender to consume; exits as soon as decoder is empty.
    decoder->setEof();
    while (!decoder->isFinished() && !decoder->hasError() &&
           audioTestRunning.load(std::memory_order_acquire)) {
        size_t free = cacheFreeFrames();
        if (free == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        size_t frames = decoder->readDecoded(decodeBuf,
                            std::min(MAX_DECODE_FRAMES, free));
        if (frames == 0) break;
        cachePushFrames(decodeBuf, frames);
    }

    // If format was never detected (very short or empty stream), force
    // prebufferReady so sender does not hang waiting for it.
    if (!prebufferReady.load(std::memory_order_relaxed)) {
        prebufferReady.store(true, std::memory_order_release);
    }

    producerDone.store(true, std::memory_order_release);
    LOG_DEBUG("[Producer] Done. totalBytes=" << totalBytes);
});
```

**Step 2: Remove the old Phase 1a, 1b, 2 code and drain from the outer loop**

The outer while-loop (lines ~834–1089) and drain section (lines ~1091–1176) now belong to the sender (Task 6). For now, remove the producer-side phases from the outer loop so they don't run twice. Leave Phase 3, 4, 5, 6 in place temporarily.

Also remove the `bool httpEof = false;` declaration at line 833 from the outer scope (it's now local to producerThread).

**Step 3: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Fix any compile errors (missing captures, variable scope). The producer thread lambda needs to capture: `[&]` works since all variables are in the same enclosing lambda scope.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(audio): extract producer thread for HTTP+decode → SPSC ring"
```

---

## Task 6: Extract sender sub-thread

**Files:**
- Modify: `src/main.cpp` (replace outer main loop + drain with sender thread)

The sender thread owns: wait for prebuffer → open Diretta → epoch wait loop → pop frames → sendAudio → elapsed update → drain remaining cache after producerDone.

**Step 1: Replace outer main loop (Phase 3–6) and drain with sender thread**

Remove the existing while-loop (lines ~834–1089) and drain (lines ~1091–1176). Replace with:

```cpp
std::thread senderThread([&]() {
    // Wait until producer has buffered enough — OR producer finished early
    // (decode error, very short stream). Both cases set prebufferReady.
    while (audioTestRunning.load(std::memory_order_acquire) &&
           !prebufferReady.load(std::memory_order_acquire) &&
           !producerDone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!audioTestRunning.load(std::memory_order_acquire)) return;

    // No format means nothing valid to open — exit regardless of cache contents.
    // Any cached frames without a format are garbage (could be mid-header bytes).
    if (!audioFmtReady.load(std::memory_order_acquire)) {
        LOG_WARN("[Sender] No valid format detected — cannot open Diretta");
        return;
    }

    // ---- DoP detection (needs >= 32 contiguous frames in ring) ----
    // Probe only if ring has enough data; escape if producer finishes before that.
    while (audioTestRunning.load(std::memory_order_acquire) &&
           cacheContiguousFrames() < 32 &&
           !producerDone.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    // Only attempt DoP probe if we actually have 32 contiguous frames.
    // If producerDone before 32 frames, skip probe and treat as PCM.
    if (audioTestRunning.load(std::memory_order_acquire) &&
        cacheContiguousFrames() >= 32) {
        // Copy existing DoP probe logic verbatim from Phase 3 (~line 907–946)
        const int32_t* samples = cacheReadPtr();
        // [DoP detection block — copy from original Phase 3 unchanged]
    }

    // ---- Open Diretta ----
    size_t prebufFrames = cacheAvailFrames();
    uint32_t prebufMs = static_cast<uint32_t>(
        prebufFrames * 1000 / (dopDetected ? dopPcmRate : audioFmt.sampleRate));
    LOG_INFO("[Sender] Pre-buffered " << prebufFrames
             << " frames (" << prebufMs << "ms)");

    if (!direttaPtr->open(audioFmt)) {
        LOG_ERROR("[Sender] Failed to open Diretta output");
        slimproto->sendStat(StatEvent::STMn);
        // Signal producer to stop so producerThread.join() does not hang.
        audioTestRunning.store(false, std::memory_order_release);
        direttaPtr->notifySpaceAvailable();  // wake any blocked waits
        return;
        // senderOpenedDiretta remains false — join block skips STMd/STMu.
    }
    if (!dopDetected) {
        direttaPtr->setS24PackModeHint(DirettaRingBuffer::S24PackMode::MsbAligned);
    }
    senderOpenedDiretta.store(true, std::memory_order_release);
    direttaOpened = true;
    slimproto->sendStat(StatEvent::STMl);

    // ---- Main sender loop: epoch wait → pop → sendAudio ----
    uint64_t seenEpoch = direttaPtr->getPopEpoch();

    auto sendChunk = [&](size_t frames) {
        const int32_t* ptr = cacheReadPtr();
        if (dopDetected) {
            DsdProcessor::convertDopToNative(
                reinterpret_cast<const uint8_t*>(ptr),
                dopBuf.data(), frames, detectedChannels);
            size_t dsdBytes = frames * 2 * detectedChannels;
            size_t numDsdSamples = dsdBytes * 8 / detectedChannels;
            direttaPtr->sendAudio(dopBuf.data(), numDsdSamples);
        } else {
            direttaPtr->sendAudio(
                reinterpret_cast<const uint8_t*>(ptr), frames);
        }
        cachePopFrames(frames);
        pushedFrames += frames;
    };

    // Use producer-published snapshots — do NOT access decoder from this thread.
    // snapshotSampleRate and snapshotTotalSec were written before audioFmtReady
    // (release), and sender read audioFmtReady (acquire) above — safe to read.
    uint32_t rate = dopDetected ? dopPcmRate : snapshotSampleRate;

    auto updateElapsed = [&]() {
        if (rate == 0) return;
        uint64_t totalMs = pushedFrames * 1000 / rate;
        uint32_t elapsedSec = static_cast<uint32_t>(totalMs / 1000);
        uint32_t elapsedMs  = static_cast<uint32_t>(totalMs);
        slimproto->updateElapsed(elapsedSec, elapsedMs);
        if (elapsedSec >= lastElapsedLog + 10) {
            lastElapsedLog = elapsedSec;
            LOG_DEBUG("[Sender] Elapsed: " << elapsedSec << "s"
                << (snapshotTotalSec > 0
                    ? " / " + std::to_string(snapshotTotalSec) + "s" : "")
                << " (" << pushedFrames << " pushed)"
                << " cache=" << cacheAvailFrames() << "f");
        }
    };

    while (audioTestRunning.load(std::memory_order_acquire)) {
        // Exit when producer is done and ring is drained
        if (producerDone.load(std::memory_order_acquire) &&
            cacheAvailFrames() == 0) break;

        // Block on hardware epoch signal (SDK worker pop)
        {
            std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
            direttaPtr->waitForSpace(lock,
                [&]{
                    return direttaPtr->getPopEpoch() != seenEpoch
                        || !audioTestRunning.load(std::memory_order_acquire)
                        || (producerDone.load(std::memory_order_acquire)
                            && cacheAvailFrames() > 0);
                },
                std::chrono::milliseconds(2));
        }
        seenEpoch = direttaPtr->getPopEpoch();

        if (direttaPtr->isPaused()) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        // Push available frames up to DirettaRingBuffer capacity
        while (audioTestRunning.load(std::memory_order_relaxed) &&
               direttaPtr->getBufferLevel() <= 0.95f) {
            size_t contiguous = cacheContiguousFrames();
            if (contiguous == 0) break;
            size_t chunk = std::min(contiguous, MAX_DECODE_FRAMES);
            sendChunk(chunk);
        }

        if (audioFmtReady.load(std::memory_order_relaxed)) {
            updateElapsed();
        }
    }

    // ---- Drain: push remaining ring frames after stop/producerDone ----
    if (direttaOpened) {
        while (cacheAvailFrames() > 0 &&
               audioTestRunning.load(std::memory_order_acquire)) {
            if (direttaPtr->isPaused()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
                continue;
            }
            if (direttaPtr->getBufferLevel() > 0.95f) {
                std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
                direttaPtr->waitForSpace(lock, std::chrono::milliseconds(5));
                continue;
            }
            size_t contiguous = cacheContiguousFrames();
            if (contiguous == 0) break;
            size_t chunk = std::min(contiguous, MAX_DECODE_FRAMES);
            sendChunk(chunk);
        }
        updateElapsed();
    }

    LOG_DEBUG("[Sender] Done. pushedFrames=" << pushedFrames);
});
```

**Step 2: Join both threads and send completion stats**

Replace the existing `slimproto->sendStat(STMd)` / `STMu` / `audioThreadDone` block at lines ~1190–1192 with:

```cpp
producerThread.join();
senderThread.join();

// STMd/STMu only if Diretta was successfully opened and stream actually ran.
// If sender returned early (open failure, no format), STMn was already sent —
// sending STMd/STMu afterward would be a contradictory status sequence.
if (senderOpenedDiretta.load(std::memory_order_acquire)) {
    if (decoder->isFormatReady()) {
        auto fmt = decoder->getFormat();
        uint64_t decoded = decoder->getDecodedSamples();
        uint32_t elapsedSec = fmt.sampleRate > 0
            ? static_cast<uint32_t>(decoded / fmt.sampleRate) : 0;
        LOG_INFO("[Audio] Stream complete: " << totalBytes << " bytes, "
                 << decoded << " frames decoded (" << elapsedSec << "s)");
    } else {
        LOG_INFO("[Audio] Stream ended (" << totalBytes << " bytes received)");
    }
    slimproto->sendStat(StatEvent::STMd);
    slimproto->sendStat(StatEvent::STMu);
}
audioThreadDone.store(true, std::memory_order_release);
```

**Step 3: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

Fix compile errors. Watch for: lambda capture issues, use of `httpEof` (now producer-local — sender uses `producerDone` instead), `formatLogged` / `totalBytes` (producer-local), variable shadowing.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "feat(audio): extract sender thread with epoch-driven wakeup from DirettaSync"
```

---

## Task 7: Fix all stop paths — wake sender on shutdown

**Files:**
- Modify: `src/main.cpp` (four sites)

There are four places where `audioTestRunning.store(false)` is set. All need
`direttaPtr->notifySpaceAvailable()` immediately after so the sender exits via
its predicate rather than waiting up to 2ms for the epoch timeout.

**Step 1: Patch all four stop sites**

| Line | Context |
|---|---|
| ~478 | STRM_START handler — stop previous stream before starting new |
| ~1199 | STRM_STOP handler |
| ~1222 | STRM_FLUSH handler |
| ~1243 | `stopAudioThread` helper lambda |

At each site, add `direttaPtr->notifySpaceAvailable()` after the
`audioTestRunning.store(false)` line:

```cpp
// Before (at each site):
audioTestRunning.store(false);
httpStream->disconnect();

// After:
audioTestRunning.store(false, std::memory_order_release);
httpStream->disconnect();
direttaPtr->notifySpaceAvailable();  // wake sender immediately
```

Note: `notifySpaceAvailable()` is safe to call even when no sender is running
(no-op if no thread is waiting on the condition variable).

**Step 2: Build**

```bash
cd build && make -j$(nproc) 2>&1 | tail -20
```

**Step 3: Verify prompt stop**

Start playback, then immediately skip to next track. Sender should log
`[Sender] Done` within ~1ms of stop, not after a 2ms timeout cycle.

**Step 4: Commit**

```bash
git add src/main.cpp
git commit -m "fix(audio): wake sender on all stop/flush/start paths via notifySpaceAvailable"
```

---

## Task 8: Integration test

No automated tests exist. Run these manually with LMS + DAC.

**Test 1 — Full track playback**
```bash
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```
- Play a FLAC track to completion
- Verify: STMs → STMl → STMd → STMu in logs, no crashes, correct elapsed time

**Test 2 — Skip / stop mid-track**
- Start playback, skip to next track within 5 seconds
- Verify: sender exits promptly (within ~10ms, not after 2ms timeout loop)
- Verify: next track starts cleanly, no leftover state

**Test 3 — Pause / resume**
- Pause mid-track, wait 10 seconds, resume
- Verify: audio resumes without glitch, elapsed time continues correctly

**Test 4 — PCM format (if LMS serves raw PCM)**
- Play a PCM track
- Verify: format detection logged, no decode errors

**Test 5 — DoP / DSD (if DAC supports)**
- Play a DSD track via Roon/LMS
- Verify: DoP markers detected, DSD mode opened in Diretta logs

**Final commit (if no issues)**

```bash
git add -p  # stage any last fixups
git commit -m "test(audio): verify producer/sender split across playback scenarios"
```

---

## Reference: Key line numbers

| Location | Line range | Notes |
|---|---|---|
| Audio thread spawn | `main.cpp:529` | outer lambda |
| DSD branch (untouched) | `main.cpp:534–738` | |
| PCM branch entry | `main.cpp:745` | decoder creation |
| Cache var declarations | `main.cpp:777–781` | replaced in Task 3 |
| Cache lambdas | `main.cpp:794–831` | replaced in Task 3 |
| Old main loop | `main.cpp:833–1089` | replaced by producer+sender in Tasks 5–6 |
| Old drain section | `main.cpp:1091–1176` | moved to sender in Task 6 |
| STMd/STMu/audioThreadDone | `main.cpp:1190–1192` | updated in Task 6 |
| Stop path | `main.cpp:473–479` | updated in Task 7 |
| DirettaSync pop callback | `DirettaSync.cpp:1727–1735` | epoch added in Task 1 |
| waitForSpace declaration | `DirettaSync.h:444` | overload added in Task 2 |
