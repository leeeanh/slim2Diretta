# RT Jitter Cleanup Phase 0 Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Remove three owned hot-path jitter sources from the Diretta playback callback path: on-demand scratch-buffer allocation, `ostringstream` heap formatting, and direct iostream I/O inside the worker.

**Architecture:** Three independent commits in `diretta/DirettaSync.cpp` and `diretta/DirettaSync.h`. No behavioral change at any log level. Each commit is buildable and testable independently. No automated test suite exists — verification is build success plus manual playback spot-check.

**Tech Stack:** C++17, Linux, libFLAC, Diretta Host SDK v147/v148. Build via `cmake .. && make` in a `build/` subdirectory. Requires root to run.

**Design doc:** `docs/plans/2026-03-16-rt-jitter-cleanup-phase0-design.md`

---

## Task 1: A1 — Preallocate `m_streamData` to max callback size

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Background

`getNewStream()` (~line 1605) currently resizes `m_streamData` on every callback where the
buffer size changes. For 44.1kHz-family PCM the size varies every ~22 callbacks due to a
fractional-frame accumulator, so the resize fires regularly during steady-state playback.

The fix: preallocate `m_streamData` to the maximum possible callback size in the config
path, then remove the resize guard from `getNewStream()`.

**Maximum callback sizes:**
- PCM: `bytesPerBuffer + bytesPerFrame` when `framesRemainder != 0` (44.1kHz family),
  else `bytesPerBuffer`. Both are local variables at the end of `configureRingPCM()`.
- DSD: exact `bytesPerBuffer` from `configureRingDSD()` — no fractional accumulator.

**Ordering constraint:** `m_consumerStateGen.fetch_add()` currently sits near the top of
each `configureRing*()` function, before `m_bytesPerBuffer` is written. `getNewStream()`
uses this counter to decide whether to reload cached format values. Move it to the very end
of each function — after `m_streamData.resize(...)` — so the worker cannot observe a new
generation before the buffer is fully prepared.

---

### Step 1: Move `m_consumerStateGen.fetch_add()` and add `m_streamData.resize()` in `configureRingPCM()`

Find the two lines currently near the top of `configureRingPCM()` (~line 1238):

```cpp
    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);
```

Remove only the `m_consumerStateGen` line from that position.

Then find the `DIRETTA_LOG(...)` call at the very end of `configureRingPCM()`:

```cpp
    DIRETTA_LOG("Ring PCM: " << rate << "Hz " << channels << "ch "
                << direttaBps << "bps, buffer=" << ringSize
                << ", prefill=" << m_prefillTargetBuffers << " buffers ("
                << m_prefillTarget << " bytes, "
                << (isCompressed ? "compressed" : "uncompressed") << ")");
```

Insert the following two lines immediately before that `DIRETTA_LOG` call:

```cpp
    // A1: Preallocate to max callback size; move generation bump to after all state is written
    m_streamData.resize(bytesPerBuffer + (framesRemainder != 0 ? static_cast<size_t>(bytesPerFrame) : 0));
    m_consumerStateGen.fetch_add(1, std::memory_order_release);
```

`bytesPerBuffer`, `bytesPerFrame`, and `framesRemainder` are all local variables in scope
at that point (computed earlier in the same function).

---

### Step 2: Move `m_consumerStateGen.fetch_add()` and add `m_streamData.resize()` in `configureRingDSD()`

Find the two lines currently near the top of `configureRingDSD()` (~line 1302):

```cpp
    // Increment format generation to invalidate cached values in sendAudio
    m_formatGeneration.fetch_add(1, std::memory_order_release);
    // C1: Also increment consumer generation for getNewStream
    m_consumerStateGen.fetch_add(1, std::memory_order_release);
```

Remove only the `m_consumerStateGen` line from that position.

Then find the `DIRETTA_LOG(...)` call at the very end of `configureRingDSD()`:

```cpp
    DIRETTA_LOG("Ring DSD: byteRate=" << byteRate << " ch=" << channels
                << " buffer=" << ringSize << " prefill=" << m_prefillTargetBuffers
                << " buffers (" << m_prefillTarget << " bytes)");
```

Insert the following two lines immediately before that `DIRETTA_LOG` call:

```cpp
    // A1: Preallocate to exact DSD callback size; move generation bump to after all state is written
    m_streamData.resize(bytesPerBuffer);
    m_consumerStateGen.fetch_add(1, std::memory_order_release);
```

`bytesPerBuffer` is a local variable in scope at that point.

---

### Step 3: Remove the resize guard from `getNewStream()`

Find these lines in `getNewStream()` (~line 1603):

```cpp
    // SDK 148 WORKAROUND: Use our own buffer instead of Stream::resize()
    // Resize our persistent buffer if needed
    if (m_streamData.size() != static_cast<size_t>(currentBytesPerBuffer)) {
        m_streamData.resize(currentBytesPerBuffer);
    }
```

Replace with:

```cpp
    // SDK 148 WORKAROUND: Use our own buffer instead of Stream::resize()
    // Buffer is preallocated to max callback size in configureRingPCM/DSD
```

The lines that follow (`baseStream.Data.P = m_streamData.data();` and
`baseStream.Size = currentBytesPerBuffer;`) are unchanged — `m_streamData` is already
large enough for any `currentBytesPerBuffer` value the accumulator can produce.

---

### Step 4: Build

```bash
cd /path/to/slim2Diretta
mkdir -p build && cd build
cmake .. && make -j$(nproc)
```

Expected: clean build, zero warnings related to the changed lines.

If the build fails with "use of undeclared identifier" or similar, check that
`bytesPerBuffer`, `bytesPerFrame`, and `framesRemainder` are all in scope at the insertion
point by reading the surrounding function body.

---

### Step 5: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "perf: preallocate m_streamData to max callback size in configureRing*

Move resize from getNewStream() hot path to configuration time.
PCM: bytesPerBuffer + bytesPerFrame when framesRemainder != 0.
DSD: exact bytesPerBuffer.
Move m_consumerStateGen.fetch_add() to after all worker-visible state
is written, including m_streamData, so getNewStream() cannot observe
a new generation before the buffer is fully prepared."
```

---

## Task 2: A2 — Replace `ostringstream` with `snprintf` in `DIRETTA_LOG_ASYNC`

**Files:**
- Modify: `diretta/DirettaSync.h` (macro + includes)
- Modify: `diretta/DirettaSync.cpp` (2 call sites)

### Background

`DIRETTA_LOG_ASYNC` uses `std::ostringstream` to format a message before pushing it to
`g_logRing`. `ostringstream` involves a heap allocation. There are exactly 2 call sites:
`sendAudio` (~line 1509) and `getNewStream` (~line 1708). Both use `<<`-chained expressions.

The fix: change the macro to `snprintf` into a 248-byte stack buffer (matching
`LogEntry::message` exactly), and convert the 2 call sites to printf-style format strings.

---

### Step 1: Update the macro in `DirettaSync.h`

At the top of the file (~line 31), the includes contain `<sstream>`. First verify
`std::ostringstream` is not used anywhere else in `DirettaSync.h` by searching:

```bash
grep -n "ostringstream\|stringstream" diretta/DirettaSync.h
```

Expected: only appears in the `DIRETTA_LOG_ASYNC` macro body. If it appears elsewhere, do
not remove `<sstream>` yet.

Add `<cstdio>` to the includes block (near `<cstring>`):

```cpp
#include <cstdio>
```

Remove `<sstream>` from the includes (assuming the grep above found no other uses).

Find the current `DIRETTA_LOG_ASYNC` macro body (~line 123):

```cpp
// Async logging macro for hot paths (non-blocking)
#define DIRETTA_LOG_ASYNC(msg) do { \
    if (g_logRing && g_logLevel >= LogLevel::DEBUG) { \
        std::ostringstream _oss; \
        _oss << msg; \
        g_logRing->push(_oss.str().c_str()); \
    } \
} while(0)
```

Replace with:

```cpp
// Async logging macro for hot paths (non-blocking, stack-only formatting)
#define DIRETTA_LOG_ASYNC(fmt, ...) do { \
    if (g_logRing && g_logLevel >= LogLevel::DEBUG) { \
        char _buf[248]; \
        std::snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__); \
        g_logRing->push(_buf); \
    } \
} while(0)
```

The buffer is 248 bytes to match `LogEntry::message` (see `DirettaSync.h:38`). `##__VA_ARGS__`
handles zero-argument calls without a trailing comma warning.

---

### Step 2: Update the `sendAudio` call site (~line 1509)

Find:

```cpp
                DIRETTA_LOG_ASYNC("sendAudio #" << count << " in=" << totalBytes
                                  << " out=" << written << " avail=" << m_ringBuffer.getAvailable()
                                  << " [" << formatLabel << "]");
```

Replace with:

```cpp
                DIRETTA_LOG_ASYNC("sendAudio #%d in=%zu out=%zu avail=%zu [%s]",
                                  count, totalBytes, written,
                                  m_ringBuffer.getAvailable(), formatLabel);
```

`formatLabel` is a `const char*` local variable assigned earlier in `sendAudio`.

---

### Step 3: Update the `getNewStream` call site (~line 1708)

Find (the exact text may span a few lines):

```cpp
        DIRETTA_LOG_ASYNC("getNewStream #" << count << " bpb=" << currentBytesPerBuffer
                          << " avail=" << avail << " (" << std::fixed << std::setprecision(1)
                          << fillPct << "%) " << (currentIsDsd ? "[DSD]" : "[PCM]"));
```

Replace with:

```cpp
        DIRETTA_LOG_ASYNC("getNewStream #%d bpb=%d avail=%zu (%.1f%%) %s",
                          count, currentBytesPerBuffer, avail, fillPct,
                          currentIsDsd ? "[DSD]" : "[PCM]");
```

`%.1f` preserves the one-decimal-place behavior of the original `std::setprecision(1)`.
`fillPct` is a `float` local variable already computed immediately before this call.

---

### Step 4: Build

```bash
cd build && make -j$(nproc)
```

Expected: clean build. If the compiler warns about `<sstream>` being needed elsewhere,
add it back — the `<sstream>` removal is optional, the macro change is not.

---

### Step 5: Commit

```bash
git add diretta/DirettaSync.h diretta/DirettaSync.cpp
git commit -m "perf: replace ostringstream with snprintf in DIRETTA_LOG_ASYNC

Eliminates heap allocation from hot-path trace calls. Stack buffer
is 248 bytes, matching LogEntry::message exactly. Two call sites
in sendAudio and getNewStream converted to printf format strings.
Add <cstdio>, drop <sstream>."
```

---

## Task 3: A3 — Gate unification, stats cleanup, and prefill logging

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Background

Three issues remain after A1 and A2:

1. The async trace calls at `sendAudio` (~1509) and `getNewStream` (~1708) are wrapped in
   `if (g_verbose)`, but `DIRETTA_LOG_ASYNC` already checks `g_logLevel >= DEBUG`
   internally. Since `--verbose` sets both flags, the outer guard is redundant.

2. `m_pushCount.fetch_add()` sits inside the `g_verbose` block in `sendAudio`, so
   `dumpStats()` always reports zero pushes when verbose is off.

3. The prefill diagnostic block at ~line 1644 uses direct `std::cout` with float division
   inside the worker callback, gated by `g_verbose`. This is the last remaining hot-path
   iostream call after A2.

---

### Step 1: Fix `sendAudio` — move `m_pushCount` out and remove the `g_verbose` wrapper

Find the block in `sendAudio` (~line 1505, inside `if (written > 0)`):

```cpp
        if (g_verbose) {
            int count = m_pushCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 3 || count % 500 == 0) {
                // A3: Async logging in hot path - avoids cout blocking
                DIRETTA_LOG_ASYNC("sendAudio #%d in=%zu out=%zu avail=%zu [%s]",
                                  count, totalBytes, written,
                                  m_ringBuffer.getAvailable(), formatLabel);
            }
        }
```

Replace with:

```cpp
        {
            int count = m_pushCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count <= 3 || count % 500 == 0) {
                DIRETTA_LOG_ASYNC("sendAudio #%d in=%zu out=%zu avail=%zu [%s]",
                                  count, totalBytes, written,
                                  m_ringBuffer.getAvailable(), formatLabel);
            }
        }
```

`m_pushCount` now increments on every push regardless of log level. `dumpStats()` will
report accurate counts in all modes.

---

### Step 2: Remove the `g_verbose` wrapper from the `getNewStream` trace

Find in `getNewStream` (~line 1706):

```cpp
    if (g_verbose && (count <= 5 || count % 5000 == 0)) {
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        DIRETTA_LOG_ASYNC("getNewStream #%d bpb=%d avail=%zu (%.1f%%) %s",
                          count, currentBytesPerBuffer, avail, fillPct,
                          currentIsDsd ? "[DSD]" : "[PCM]");
    }
```

Replace with:

```cpp
    if (count <= 5 || count % 5000 == 0) {
        float fillPct = (currentRingSize > 0) ? (100.0f * avail / currentRingSize) : 0.0f;
        DIRETTA_LOG_ASYNC("getNewStream #%d bpb=%d avail=%zu (%.1f%%) %s",
                          count, currentBytesPerBuffer, avail, fillPct,
                          currentIsDsd ? "[DSD]" : "[PCM]");
    }
```

The `DIRETTA_LOG_ASYNC` macro's own `g_logLevel >= DEBUG` check is the sole gate now.

---

### Step 3: Replace the prefill `std::cout` block

Find in `getNewStream` inside `if (!m_prefillComplete...)` (~line 1643):

```cpp
        // Diagnostic: Log prefill progress periodically (only in verbose mode)
        if (g_verbose) {
            static int prefillLogCount = 0;
            size_t avail = m_ringBuffer.getAvailable();
            if (prefillLogCount++ % 50 == 0) {  // Log every 50th call (~100ms at typical rates)
                float pct = (m_prefillTarget > 0) ? (100.0f * avail / m_prefillTarget) : 0.0f;
                std::cout << "[Prefill] Waiting: " << avail << "/" << m_prefillTarget
                          << " bytes (" << std::fixed << std::setprecision(1) << pct << "%)"
                          << (currentIsDsd ? " [DSD]" : " [PCM]") << std::endl;
            }
        }
```

Replace with:

```cpp
        // Diagnostic: Log prefill progress periodically
        {
            static int prefillLogCount = 0;
            if (prefillLogCount++ % 50 == 0) {
                size_t prefillAvail = m_ringBuffer.getAvailable();
                float pct = (m_prefillTarget > 0) ? (100.0f * prefillAvail / m_prefillTarget) : 0.0f;
                DIRETTA_LOG_ASYNC("[Prefill] Waiting: %zu/%zu bytes (%.1f%%)%s",
                                  prefillAvail, m_prefillTarget, pct,
                                  currentIsDsd ? " [DSD]" : " [PCM]");
            }
        }
```

Note: the local variable is named `prefillAvail` (not `avail`) to avoid shadowing the
`avail` variable declared later in `getNewStream()` after the prefill early-return block.

---

### Step 4: Build

```bash
cd build && make -j$(nproc)
```

Expected: clean build. Verify `std::cout` and `g_verbose` no longer appear in any hot-path
block by checking:

```bash
grep -n "g_verbose\|std::cout" diretta/DirettaSync.cpp | grep -v "^[0-9]*:.*setRealtimePriority\|^[0-9]*:.*listTargets\|^[0-9]*:.*open\|^[0-9]*:.*logSink\|^[0-9]*:.*dumpStats\|^[0-9]*:.*//\|^[0-9]*:.*Enabled"
```

Any remaining hits should be in control-plane functions (`open`, `enable`, `listTargets`,
`dumpStats`, `logSinkCapabilities`, `setRealtimePriority`) — not in `sendAudio` or
`getNewStream`.

---

### Step 5: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "perf: unify log gates, fix push stats, replace prefill cout in hot path

sendAudio: m_pushCount.fetch_add() now unconditional so dumpStats()
reports accurate push counts at all log levels.
sendAudio + getNewStream: remove redundant g_verbose outer guard;
DIRETTA_LOG_ASYNC's g_logLevel check is the sole gate.
getNewStream prefill block: replace std::cout + float division with
DIRETTA_LOG_ASYNC, removing last direct iostream call from callback path."
```

---

## Final Verification

After all three commits, do a full clean build and manual playback check:

```bash
cd build && cmake .. && make -j$(nproc)
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

With `-v` (verbose / DEBUG log level), confirm:
- Playback starts without underruns on a 44.1kHz FLAC track.
- Prefill log messages appear (from the A3 prefill block).
- `sendAudio` and `getNewStream` trace messages appear periodically in the async log.
- No `std::cout` prefill spam visible on stdout during playback.

Optionally run `dumpStats()` (triggered by sending SIGUSR1 or equivalent) and confirm
the push count is non-zero after a few seconds of playback.
