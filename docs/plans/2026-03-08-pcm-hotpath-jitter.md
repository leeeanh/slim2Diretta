# PCM Hot-Path Jitter Reduction Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Reduce scheduling burstiness in the PCM pipeline by fixing O(n) decoder compaction, adaptive HTTP read sizing, and a two-mode sender refill policy.

**Architecture:** Three independent, localized changes. Each fix lives where the problem is. The existing SPSC ring buffer and producer/sender thread model are unchanged. See design doc: `docs/plans/2026-03-08-pcm-hotpath-jitter-design.md`.

**Tech Stack:** C++17, pthreads, libFLAC, Diretta Host SDK v147/v148. Build with `cmake .. && make` from `build/`. Run with `sudo ./slim2diretta -s <lms-ip> --target 1 -v`.

---

### Task 1: PcmDecoder — add m_readPos to header

**Files:**
- Modify: `src/PcmDecoder.h`

**Step 1: Add the read-offset member and compact threshold**

In `PcmDecoder.h`, inside the `private:` section, after the `m_dataBuf` declaration (line 49), add:

```cpp
size_t m_readPos = 0;  // Offset into m_dataBuf: unread data starts here
static constexpr size_t COMPACT_THRESHOLD = 65536;
```

The member list should look like:

```cpp
// PCM data buffer (raw bytes before conversion)
std::vector<uint8_t> m_dataBuf;
size_t m_readPos = 0;  // Offset into m_dataBuf: unread data starts here
static constexpr size_t COMPACT_THRESHOLD = 65536;
```

**Step 2: Build to verify header compiles**

```bash
cd build && make 2>&1 | head -30
```

Expected: no errors (m_readPos unused yet — that's fine at this step).

**Step 3: Commit**

```bash
git add src/PcmDecoder.h
git commit -m "refactor(pcm): add m_readPos offset field to PcmDecoder"
```

---

### Task 2: PcmDecoder — replace erase with offset in readDecoded

**Files:**
- Modify: `src/PcmDecoder.cpp`

**Context:** `readDecoded` currently calls `convertSamples(m_dataBuf.data(), ...)` and then erases consumed bytes. After this task, it uses `m_dataBuf.data() + m_readPos` and advances the offset instead.

**Step 1: Fix the available-bytes calculation (line 70)**

Change:
```cpp
size_t availBytes = m_dataBuf.size();
if (m_dataRemaining > 0) {
    availBytes = std::min(availBytes, static_cast<size_t>(m_dataRemaining));
}
```

To:
```cpp
size_t availBytes = m_dataBuf.size() - m_readPos;
if (m_dataRemaining > 0) {
    availBytes = std::min(availBytes, static_cast<size_t>(m_dataRemaining));
}
```

**Step 2: Fix the convertSamples call (line 90)**

Change:
```cpp
convertSamples(m_dataBuf.data(), out, bytesToConvert);
```

To:
```cpp
convertSamples(m_dataBuf.data() + m_readPos, out, bytesToConvert);
```

**Step 3: Replace the erase block (lines 92-99) with offset + conditional compact**

Remove:
```cpp
// Consume from data buffer
m_dataBuf.erase(m_dataBuf.begin(), m_dataBuf.begin() + bytesToConvert);
if (m_dataRemaining > 0) {
    m_dataRemaining -= bytesToConvert;
    if (m_dataRemaining == 0) {
        m_finished = true;
    }
}
```

Replace with:
```cpp
// Consume from data buffer — advance read offset, compact only when worthwhile
m_readPos += bytesToConvert;
if (m_dataRemaining > 0) {
    m_dataRemaining -= bytesToConvert;
    if (m_dataRemaining == 0) {
        m_finished = true;
    }
}

// Compact: only when the consumed front is worth reclaiming.
// Condition: offset is past threshold AND (offset is majority of buffer OR
// unread tail is itself below threshold — avoids large memmoves on small gains).
size_t unread = m_dataBuf.size() - m_readPos;
if (m_readPos >= COMPACT_THRESHOLD &&
    (m_readPos >= m_dataBuf.size() / 2 || unread < COMPACT_THRESHOLD)) {
    m_dataBuf.erase(m_dataBuf.begin(),
                    m_dataBuf.begin() + static_cast<std::ptrdiff_t>(m_readPos));
    m_readPos = 0;
}
```

**Step 4: Build**

```bash
cd build && make 2>&1 | head -30
```

Expected: clean build.

**Step 5: Manual smoke test**

Play a WAV or AIFF track through LMS. Verify:
- Audio plays without glitches
- `-v` log shows no `[PCM]` errors
- No crash or silent stream

**Step 6: Commit**

```bash
git add src/PcmDecoder.cpp
git commit -m "perf(pcm): replace vector::erase with read-offset in PcmDecoder"
```

---

### Task 3: PcmDecoder — reset m_readPos in flush()

**Files:**
- Modify: `src/PcmDecoder.cpp`

**Context:** `flush()` resets all state for a new stream. `m_readPos` must also be reset here, or the offset will be stale on stream reinit. `setEof()` must NOT reset it — `setEof` is an end-of-input signal only and resets there would corrupt buffered unread data.

**Step 1: Add reset to flush() (around line 119)**

In the `flush()` body, after `m_dataBuf.clear();`, add:

```cpp
m_readPos = 0;
```

The full flush body should include, in order:
```cpp
m_state = State::DETECT;
m_headerBuf.clear();
m_dataBuf.clear();
m_readPos = 0;        // ← add this line
m_format = {};
m_formatReady = false;
// ... rest unchanged
```

**Step 2: Build**

```bash
cd build && make 2>&1 | head -20
```

**Step 3: Test track-change path**

Play one track to completion, allow it to transition to the next. Verify no glitch or error on the second track.

**Step 4: Commit**

```bash
git add src/PcmDecoder.cpp
git commit -m "fix(pcm): reset m_readPos in flush() for stream reinit"
```

---

### Task 4: DirettaSync — add getFramesPerBuffer() accessor

**Files:**
- Modify: `diretta/DirettaSync.h`

**Context:** The sender state machine (Task 5) needs the SDK callback frame count to cap steady-state fills. `DirettaSync` computes this as `m_sampleRate / 1000` (base frames, integer division). Adding an accessor avoids leaking format knowledge into `main.cpp` and avoids any dependency on the remainder-accumulator used in the consumer path.

**Step 1: Add the accessor near getBufferLevel() in DirettaSync.h**

Find the `getBufferLevel()` declaration (currently line 394) and add immediately after it:

```cpp
// Returns base callback frame count (sampleRate / 1000).
// Does not include remainder-accumulator adjustment — base count is
// sufficient and stable for sender pacing.
size_t getFramesPerBuffer() const {
    return static_cast<size_t>(
        m_sampleRate.load(std::memory_order_relaxed)) / 1000;
}
```

**Step 2: Build**

```bash
cd build && make 2>&1 | head -20
```

**Step 3: Commit**

```bash
git add diretta/DirettaSync.h
git commit -m "feat(diretta): add getFramesPerBuffer() accessor for sender pacing"
```

---

### Task 5: main.cpp — adaptive HTTP read sizing in producer

**Files:**
- Modify: `src/main.cpp`

**Context:** The producer loop reads from HTTP at line ~995 with `sizeof(httpBuf)` = 65536 bytes unconditionally. After this task, it computes `readSize` from cache fill ratio before each read.

**Step 1: Add constants near the httpBuf declaration (around line 894)**

Find this block:
```cpp
uint8_t httpBuf[65536];
constexpr size_t MAX_DECODE_FRAMES = 1024;
```

Add constants immediately after:
```cpp
uint8_t httpBuf[65536];
constexpr size_t MAX_DECODE_FRAMES = 1024;
constexpr size_t HTTP_READ_MIN    =  4096;
constexpr size_t HTTP_READ_STEADY =  8192;
constexpr size_t HTTP_READ_HIGH   = 16384;
constexpr size_t HTTP_READ_BURST  = 65536;
```

**Step 2: Replace the hard-coded sizeof(httpBuf) in the producer read call (line ~995)**

Find this line in the producer lambda:
```cpp
ssize_t n = httpStream->readWithTimeout(httpBuf, sizeof(httpBuf), 2);
```

Replace with:
```cpp
// Adaptive read size: burst during prebuffer, cache-pressure-banded in steady state
size_t readSize;
if (!prebufferReady.load(std::memory_order_relaxed)) {
    readSize = HTTP_READ_BURST;
} else {
    size_t free  = cacheFreeFrames();
    size_t total = DECODE_CACHE_MAX_SAMPLES
                   / static_cast<size_t>(std::max(detectedChannels, 1));
    float fillRatio = (total > 0)
        ? 1.0f - static_cast<float>(free) / static_cast<float>(total)
        : 1.0f;

    if      (fillRatio > 0.80f) readSize = HTTP_READ_MIN;
    else if (fillRatio > 0.50f) readSize = HTTP_READ_STEADY;
    else                        readSize = HTTP_READ_HIGH;
}
ssize_t n = httpStream->readWithTimeout(httpBuf, readSize, 2);
```

**Step 3: Build**

```bash
cd build && make 2>&1 | head -30
```

**Step 4: Manual test — steady-state read sizing**

Run with `-v` and play a track. Check that log shows normal playback without underruns or stalls. Since reads are smaller in steady state, verify no `[Producer] Cache full` warnings appear more than briefly at startup.

**Step 5: Commit**

```bash
git add src/main.cpp
git commit -m "perf(audio): adaptive HTTP read sizing based on cache fill ratio"
```

---

### Task 6: main.cpp — two-mode sender refill state machine

**Files:**
- Modify: `src/main.cpp`

**Context:** The sender loop currently fills to 95% on every wakeup. After this task it uses a `SenderMode` enum with two states: `kRecovery` (aggressive fill) and `kSteady` (callback-paced fill), with hysteresis watermarks to prevent mode flap.

**Step 1: Add the SenderMode enum and watermark constants near the sender thread definition**

Find the sender thread lambda declaration (around `std::thread senderThread([&]() {`). Just before the lambda body's first statement, add:

```cpp
enum class SenderMode { kRecovery, kSteady };
constexpr float RECOVERY_TARGET = 0.90f;  // fill to here in recovery mode
constexpr float STEADY_ENTER    = 0.75f;  // enter steady once ring is above this
constexpr float STEADY_EXIT     = 0.40f;  // drop back to recovery below this
constexpr int   STEADY_CHUNKS_PER_WAKEUP = 2;
SenderMode senderMode = SenderMode::kRecovery;
```

**Step 2: Replace the inner fill loop in the main sender while-loop**

Find this existing block (lines ~1228-1234):
```cpp
while (audioTestRunning.load(std::memory_order_relaxed) &&
       direttaPtr->getBufferLevel() <= 0.95f) {
    size_t contiguous = cacheContiguousFrames();
    if (contiguous == 0) break;
    size_t chunk = std::min(contiguous, MAX_DECODE_FRAMES);
    sendChunk(chunk);
}
```

Replace entirely with:
```cpp
// State transition — once per wakeup, outside all fill loops
float level = direttaPtr->getBufferLevel();
if (senderMode == SenderMode::kRecovery && level >= STEADY_ENTER)
    senderMode = SenderMode::kSteady;
else if (senderMode == SenderMode::kSteady && level < STEADY_EXIT)
    senderMode = SenderMode::kRecovery;

if (senderMode == SenderMode::kRecovery) {
    // Aggressive fill: run up to RECOVERY_TARGET
    while (audioTestRunning.load(std::memory_order_relaxed) &&
           direttaPtr->getBufferLevel() < RECOVERY_TARGET) {
        size_t contiguous = cacheContiguousFrames();
        if (contiguous == 0) break;
        sendChunk(std::min(contiguous, MAX_DECODE_FRAMES));
    }
} else {
    // Steady: cap each wakeup to STEADY_CHUNKS_PER_WAKEUP callback-sized chunks
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

**Step 3: Build**

```bash
cd build && make 2>&1 | head -30
```

**Step 4: Manual test — full playback**

```bash
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Verify:
- Track starts cleanly (kRecovery fills ring before steady-state engagement)
- Playback runs without audible glitches or underruns
- Track transitions work (new track starts in kRecovery, transitions to kSteady)
- Log does not show excessive mode switching during steady playback

**Step 5: Test edge cases**

- Pause and resume: verify kRecovery re-engages on resume if ring drained below STEADY_EXIT
- Seek: verify kRecovery handles the rebuffer after seek
- DSD/high-rate content (if available): 192 kHz has `getFramesPerBuffer()` = 192, so `frameCap` = 384 frames — verify it tracks correctly

**Step 6: Commit**

```bash
git add src/main.cpp
git commit -m "perf(audio): two-mode sender refill policy with hysteresis watermarks"
```

---

## Verification Checklist

After all tasks complete, run through this sequence:

1. **PCM/WAV playback** — full track, no glitch, no `[PCM]` error in log
2. **AIFF playback** — same verification
3. **FLAC playback** — producer adaptive sizing is codec-agnostic; verify no regression
4. **Track change** — `flush()` resets `m_readPos`; verify second track plays correctly
5. **High sample rate** (88.2/96/192 kHz) — `getFramesPerBuffer()` returns correct base count
6. **Pause/resume** — sender re-enters kRecovery, no underrun
7. **Build clean** — `make` from fresh `build/` with no warnings on changed files
