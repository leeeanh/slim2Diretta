# EVL Pop-Epoch Semaphore Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Replace the `try_lock + notify_one` pop-epoch wake path with an `evl_sem` owned by `DirettaSync`, eliminating the silent miss that adds up to 2 ms latency to every sender wakeup when the mutex is held.

**Architecture:** Six independent commits touching three files. `DirettaSync` owns the semaphore lifecycle (create at `open()`, drain after each quiesce point, close in destructor). The notify side posts the sem unconditionally from `getNewStream()`; the sender waits on it instead of the condvar. A `m_popSemReady` runtime flag gates all three EVL sites so that a failed `evl_new_sem()` silently falls back to the existing condvar path. No automated test suite — verification is build success + manual playback spot-check after each commit.

**Tech Stack:** C++17, Linux, libevl (`HAVE_EVL` build flag, pkg-config `evl`), Diretta Host SDK v147/v148. Build: `cmake .. && make -j$(nproc)` in a `build/` subdirectory. Requires root to run. Design doc: `docs/plans/2026-03-17-evl-pop-sem-design.md`.

---

## Task 1: Add `evl_sem` member and method declarations to `DirettaSync.h`

**Files:**
- Modify: `diretta/DirettaSync.h`

### Step 1: Add the `evl_sem` member and `m_popSemReady` flag

Find the `HAVE_EVL` include for `evl/thread.h` that was added in the last commit. It lives in `diretta/DirettaSync.cpp`. In `DirettaSync.h` there is no EVL include yet. Find the block of includes near the top of `diretta/DirettaSync.h` (around lines 1–15) and add:

```cpp
#ifdef HAVE_EVL
#include <evl/sem.h>
#endif
```

Then find the private member section of the `DirettaSync` class (near line 595, alongside `m_flowMutex` and `m_spaceAvailable`). Add:

```cpp
#ifdef HAVE_EVL
    struct evl_sem m_popSem {};
    bool m_popSemReady = false;
#endif
```

### Step 2: Add private method declarations

In the private methods section of `DirettaSync` (near the other private helpers), add:

```cpp
bool ensurePopSemCreated();  // idempotent; returns false on evl_new_sem() failure
void drainPopSem();          // empties stale count after quiesce; no-op without HAVE_EVL
```

### Step 3: Add public method declarations

In the public section (alongside `waitForSpace`, `notifySpaceAvailable`), add:

```cpp
bool isPopSemReady() const {
#ifdef HAVE_EVL
    return m_popSemReady;
#else
    return false;
#endif
}

void waitForPop(std::chrono::milliseconds timeout) {
#ifdef HAVE_EVL
    if (m_popSemReady) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += timeout.count() * 1'000'000LL;
        if (ts.tv_nsec >= 1'000'000'000LL) {
            ts.tv_sec++;
            ts.tv_nsec -= 1'000'000'000LL;
        }
        evl_timedwait_sem(&m_popSem, &ts);
        // return value intentionally ignored — seenEpoch re-read after return
        // covers both a real pop wake and a timeout/spurious wake
        return;
    }
#endif
    // EVL unavailable or sem creation failed: condvar fallback handled by caller
}
```

### Step 4: Update `notifySpaceAvailable()` to also post the sem

Find `notifySpaceAvailable()` in the public section (around line 469):

```cpp
void notifySpaceAvailable() {
    m_spaceAvailable.notify_one();
}
```

Replace with:

```cpp
void notifySpaceAvailable() {
    m_spaceAvailable.notify_one();
#ifdef HAVE_EVL
    if (m_popSemReady)
        evl_post_sem(&m_popSem);
#endif
}
```

This ensures the four stop/flush call sites (`src/main.cpp:1318`, `1572`, `1602`, `1624`) unblock the sender from `evl_timedwait_sem()` immediately without touching those call sites.

### Step 5: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

Expected: clean build. If the compiler complains about `evl_sem` being incomplete, verify the `#include <evl/sem.h>` is inside `#ifdef HAVE_EVL` and that the build was configured with `ENABLE_EVL=1` (check `make info` output or CMakeCache).

### Step 6: Commit

```bash
git add diretta/DirettaSync.h
git commit -m "feat(evl): add evl_sem member and pop-epoch method declarations to DirettaSync

Add m_popSem / m_popSemReady under HAVE_EVL guard. Declare
ensurePopSemCreated() and drainPopSem() private helpers. Add
waitForPop() and isPopSemReady() public methods. Update
notifySpaceAvailable() to also post sem so stop/flush unblocks
sender from evl_timedwait_sem() without touching call sites."
```

---

## Task 2: Implement `ensurePopSemCreated()` and `drainPopSem()` in `DirettaSync.cpp`

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Step 1: Verify `evl/thread.h` is already included

The existing `attachEvlThread()` added `#include <evl/thread.h>` at line 14–16 of `DirettaSync.cpp` under `HAVE_EVL`. Verify it is there:

```bash
grep -n "evl/thread\|HAVE_EVL" diretta/DirettaSync.cpp | head -5
```

Expected: lines showing `#ifdef HAVE_EVL` and `#include <evl/thread.h>`.

### Step 2: Add `ensurePopSemCreated()` implementation

Add after the existing `attachEvlThread()` function body (around line 80 in `DirettaSync.cpp`):

```cpp
bool DirettaSync::ensurePopSemCreated() {
#ifdef HAVE_EVL
    if (m_popSemReady) return true;
    int ret = evl_new_sem(&m_popSem, "/slim2diretta-pop-%d", getpid());
    if (ret < 0) {
        std::cerr << "[DirettaSync] Warning: evl_new_sem failed (ret=" << ret
                  << ") — falling back to condvar for pop-epoch wait" << std::endl;
        return false;
    }
    m_popSemReady = true;
    DIRETTA_LOG("EVL pop semaphore created");
    return true;
#else
    return false;
#endif
}
```

### Step 3: Add `drainPopSem()` implementation

Immediately after `ensurePopSemCreated()`:

```cpp
void DirettaSync::drainPopSem() {
#ifdef HAVE_EVL
    if (!m_popSemReady) return;
    struct timespec zero = {0, 0};
    while (evl_timedwait_sem(&m_popSem, &zero) == 0) {}
    // loop exits when evl_timedwait_sem returns -EAGAIN (sem count == 0)
#endif
}
```

### Step 4: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

Expected: clean build, no undefined reference errors.

### Step 5: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(evl): implement ensurePopSemCreated() and drainPopSem()

ensurePopSemCreated(): calls evl_new_sem() once, sets m_popSemReady,
logs warning and returns false on failure. drainPopSem(): zero-timeout
spin until sem count reaches 0 (-EAGAIN). Both no-ops without HAVE_EVL."
```

---

## Task 3: Wire semaphore lifecycle into `open()`

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Background

`open()` starts at line 434. It has two top-level branches:
- **Fast path** (same format, line 461): calls `stop()` at line 497 — quiesce point.
- **Slow path** (format change, line 530): three sub-branches, each calls `stop()` then joins the worker thread — quiesce point is after `m_workerThread.join()`.

There is also a "first open" path (no previous format) that falls through to `needFullConnect = true` without any prior `stop()`. No drain is needed there because no prior session was running.

### Step 1: Call `ensurePopSemCreated()` and entry drain at `open()` entry

Find the first two lines of `DirettaSync::open()` (line 434):

```cpp
bool DirettaSync::open(const AudioFormat& format) {

    std::cout << "[DirettaSync] ========== OPEN ==========" << std::endl;
```

Add the calls after the opening log but before the `m_enabled` check:

```cpp
bool DirettaSync::open(const AudioFormat& format) {

    std::cout << "[DirettaSync] ========== OPEN ==========" << std::endl;
    std::cout << "[DirettaSync] Format: " << format.sampleRate << "Hz/"
              << format.bitDepth << "bit/" << format.channels << "ch "
              << (format.isDSD ? "DSD" : "PCM") << std::endl;

    ensurePopSemCreated();  // idempotent; no-op if already created or HAVE_EVL absent
    // If SDK is not open, no getNewStream() is running; drain stale posts from the
    // prior session's stop/flush before the new session starts.
    if (!m_open) drainPopSem();

    if (!m_enabled) {
```

### Step 2: Drain after `stop()` in the fast path

Find `stop()` in the fast path (line 497). The lines immediately after are:

```cpp
            stop();

            // Clear buffer and reset flags
            m_ringBuffer.clear();
```

Add the drain between them:

```cpp
            stop();
            drainPopSem();  // old getNewStream() can no longer post after stop()

            // Clear buffer and reset flags
            m_ringBuffer.clear();
```

### Step 3: Drain after worker join in the DSD→PCM / DSD rate-change branch

Find the `m_workerThread.join()` in the DSD→PCM branch (around line 573–575):

```cpp
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }

                // Now safe to close SDK - worker thread is stopped
                DIRETTA::Sync::close();
```

Add drain immediately after the closing brace of the `lock_guard` block:

```cpp
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }
                drainPopSem();  // worker joined; no further getNewStream() posts possible

                // Now safe to close SDK - worker thread is stopped
                DIRETTA::Sync::close();
```

### Step 4: Drain after worker join in the PCM rate-change branch

Same pattern as Step 3, at line ~623–625:

```cpp
                    if (m_workerThread.joinable()) {
                        m_workerThread.join();
                    }
                }
                drainPopSem();

                // Now safe to close SDK
                DIRETTA::Sync::close();
```

### Step 5: Drain after worker join in the PCM→DSD full-reset branch

Find the third `m_workerThread.join()` block (the PCM→DSD `needsFullReset` branch, around line 684–687). Apply the same pattern as Steps 3 and 4.

### Step 6: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

Expected: clean build.

### Step 7: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(evl): wire ensurePopSemCreated/drainPopSem into open()

Call ensurePopSemCreated() at open() entry (idempotent).
Call drainPopSem() after each quiesce point:
  - fast path: after stop() before ring clear
  - DSD->PCM, PCM rate-change, PCM->DSD full-reset: after workerThread.join()
Prevents stale stop/flush sem posts from triggering false wakeups
in the next playback session."
```

---

## Task 4: Replace `try_lock + notify_one` with `evl_post_sem()` in `getNewStream()`

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Background

`getNewStream()` currently does (lines 1793–1800 after previous commits, line numbers may shift slightly):

```cpp
    m_popEpoch.fetch_add(1, std::memory_order_release);

    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op (harmless notification)
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }
```

The `try_lock()` fails silently whenever the sender holds `m_flowMutex`. That is the bug being fixed.

### Step 1: Find the exact location

```bash
grep -n "try_lock\|m_popEpoch.fetch_add" diretta/DirettaSync.cpp
```

Note the line numbers.

### Step 2: Replace the notify block

Find:

```cpp
    m_popEpoch.fetch_add(1, std::memory_order_release);

    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op (harmless notification)
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }
```

Replace with:

```cpp
    m_popEpoch.fetch_add(1, std::memory_order_release);

#ifdef HAVE_EVL
    // P1: Post EVL semaphore — non-blocking, callable from in-band context,
    // no mutex dependency. Eliminates the try_lock miss when sender holds m_flowMutex.
    if (m_popSemReady) {
        evl_post_sem(&m_popSem);
    } else {
#endif
    // G1: Signal producer that space is now available
    // Use try_lock to avoid blocking the time-critical consumer thread
    // If producer isn't waiting, this is a no-op (harmless notification)
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }
#ifdef HAVE_EVL
    }
#endif
```

### Step 3: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

Expected: clean build.

### Step 4: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(evl): replace try_lock+notify_one with evl_post_sem in getNewStream()

When m_popSemReady, post evl_sem unconditionally from in-band context.
No mutex needed; eliminates the silent miss when sender holds m_flowMutex.
Falls back to try_lock+notify_one when EVL is absent or sem creation failed."
```

---

## Task 5: Add `evl_close_sem()` to `~DirettaSync()`

**Files:**
- Modify: `diretta/DirettaSync.cpp`

### Step 1: Find the destructor

```bash
grep -n "DirettaSync::~" diretta/DirettaSync.cpp
```

Currently (line 140):

```cpp
DirettaSync::~DirettaSync() {
    disable();
    DIRETTA_LOG("Destroyed");
}
```

### Step 2: Add sem close

```cpp
DirettaSync::~DirettaSync() {
    disable();
#ifdef HAVE_EVL
    if (m_popSemReady) {
        evl_close_sem(&m_popSem);
        m_popSemReady = false;
    }
#endif
    DIRETTA_LOG("Destroyed");
}
```

### Step 3: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

### Step 4: Commit

```bash
git add diretta/DirettaSync.cpp
git commit -m "feat(evl): close evl_sem in ~DirettaSync()

Call evl_close_sem() after disable() when m_popSemReady.
Prevents resource leak on process exit or DirettaSync teardown."
```

---

## Task 6: Switch sender pop-epoch wait to `waitForPop()` in `src/main.cpp`

**Files:**
- Modify: `src/main.cpp`

### Background

The sender loop's pop-epoch wait is at `src/main.cpp:1408–1416`. Currently:

```cpp
                        {
                            std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
                            direttaPtr->waitForSpace(lock,
                                [&]() {
                                    return direttaPtr->getPopEpoch() != seenEpoch ||
                                           !audioTestRunning.load(std::memory_order_acquire);
                                },
                                std::chrono::milliseconds(2));
                        }
                        seenEpoch = direttaPtr->getPopEpoch();
```

### Step 1: Find the exact location

```bash
grep -n "waitForSpace\|seenEpoch\|getPopEpoch" src/main.cpp
```

There will be two `waitForSpace` hits: the pop-epoch wait (~1408) and the drain stall (~1474). Only modify the one with the `seenEpoch` predicate.

### Step 2: Replace the pop-epoch wait

Find the block above. Replace with:

```cpp
#ifdef HAVE_EVL
                        if (direttaPtr->isPopSemReady()) {
                            direttaPtr->waitForPop(std::chrono::milliseconds(2));
                        } else {
#endif
                        {
                            std::unique_lock<std::mutex> lock(direttaPtr->getFlowMutex());
                            direttaPtr->waitForSpace(lock,
                                [&]() {
                                    return direttaPtr->getPopEpoch() != seenEpoch ||
                                           !audioTestRunning.load(std::memory_order_acquire);
                                },
                                std::chrono::milliseconds(2));
                        }
#ifdef HAVE_EVL
                        }
#endif
                        seenEpoch = direttaPtr->getPopEpoch();
```

The `seenEpoch` re-read after the `#endif` covers both paths: real pop (epoch changed) and timeout/spurious wake (epoch unchanged, loop continues). No predicate is needed inside `waitForPop()` itself.

The drain stall at line ~1474 (`waitForSpace(lock, 5ms)` without a predicate) is **not** changed.

### Step 3: Build

```bash
cd build && make -j$(nproc) 2>&1 | head -30
```

Expected: clean build.

### Step 4: Smoke test — without EVL (condvar path)

Build without EVL to verify the fallback is intact:

```bash
cd build && cmake -DENABLE_EVL=0 .. && make -j$(nproc) 2>&1 | tail -5
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Expected: playback starts, no underruns on a 44.1 kHz FLAC track. Sender behaves identically to before this change series.

### Step 5: Smoke test — with EVL (sem path)

```bash
cd build && cmake -DENABLE_EVL=1 .. && make -j$(nproc) 2>&1 | tail -5
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Expected:
- `[DirettaSync] EVL pop semaphore created` appears once on the first stream open and not again on subsequent opens (semaphore is created once per process lifetime, then reused).
- Playback starts without underruns on a 44.1 kHz FLAC track.
- No `evl_new_sem failed` warning.
- `sendAudio` and `getNewStream` trace messages appear in the async log (use `-v`).
- Queue two consecutive tracks of the same format (e.g., two 44.1 kHz FLAC files) to exercise the fast-path (same-format quick-resume) drain — no stale wakes, no underrun at the transition.
- Queue a DSD track after PCM (or change to a different sample rate) to exercise the slow-path drain.

### Step 6: Commit

```bash
git add src/main.cpp
git commit -m "feat(evl): switch sender pop-epoch wait to evl_timedwait_sem

When isPopSemReady(), call waitForPop(2ms) instead of the condvar path.
waitForPop() parks sender in OOB stage; EVL Dovetail promotes it when
getNewStream() posts the sem. condvar path retained for HAVE_EVL=0 and
evl_new_sem() failure. Drain stall at >95% buffer unchanged."
```

---

## Final Verification

After all six commits, full clean build + extended playback test:

```bash
cd build && cmake -DENABLE_EVL=1 .. && make -j$(nproc)
sudo ./slim2diretta -s <lms-ip> --target 1 -v --rt-cpu 3
```

Checklist:
- `EVL pop semaphore created` log line appears exactly once per process run, on the first `open()` only — not on subsequent opens.
- No `evl_new_sem failed` warnings.
- Play 30+ minutes of 44.1 kHz FLAC — zero underruns.
- Same-format gapless transition (two tracks at identical rate/depth/channels) — fast path fires, drainPopSem runs after `stop()`, no stale wakes, no underrun.
- Format change (e.g., PCM → DSD or 44.1 kHz → 96 kHz) — slow-path branch fires, drainPopSem runs after worker join, clean open.
- `SIGINT` / stop command — sender exits promptly (not delayed 2 ms by sem timeout).

Verify no regression with EVL disabled:

```bash
cd build && cmake -DENABLE_EVL=0 .. && make -j$(nproc)
sudo ./slim2diretta -s <lms-ip> --target 1 -v
```

Expected: identical playback behavior to the pre-EVL baseline.
