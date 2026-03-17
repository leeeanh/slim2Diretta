# EVL Pop-Epoch Semaphore — Design

**Date**: 2026-03-17
**Status**: Approved
**Scope**: `diretta/DirettaSync.h`, `diretta/DirettaSync.cpp`, `src/main.cpp`

---

## Problem

The pop-epoch notify path in `getNewStream()` (DirettaSync.cpp:1793–1800) calls
`m_flowMutex.try_lock()` before `m_spaceAvailable.notify_one()`. If the sender holds
`m_flowMutex` at that instant — which it does during any `waitForSpace()` call —
`try_lock()` fails silently and the notification is dropped. The sender falls back to the
2 ms `condition_variable::wait_for` timeout, adding up to 2 ms of avoidable wake latency
on every such miss.

## Goal

Replace the pop-epoch notify/wait pair with an `evl_sem` so that:

1. `getNewStream()` posts the semaphore unconditionally with no mutex dependency.
2. The sender parks in OOB stage and is woken by EVL's Dovetail layer with lower latency.
3. Stop/flush control-plane unblocks also post the semaphore so the sender is not delayed
   up to 2 ms on shutdown.
4. A runtime fallback to the existing condvar path is preserved for `HAVE_EVL` builds
   where `evl_new_sem()` fails at runtime.

## Non-Goals

- Replacing the drain-stall wait at `src/main.cpp:1474` (buffer >95% full —
  not time-critical, stays on Linux condvar).
- Replacing `m_flowMutex` / `m_spaceAvailable` for any other purpose.
- Converting any thread to sustained OOB operation; the Diretta SDK does not support OOB.
- Replacing the `m_popEpoch` atomic (retained for epoch tracking).

## Constraint: SDK stays in-band

The Diretta SDK does not operate OOB. `getNewStream()` is called from within
`syncWorker()`, which makes Linux-side blocking calls. The SDK worker thread is
EVL-attached (via `attachEvlThread("sdk-worker")`) but in-band when `getNewStream()`
fires. `evl_post_sem()` is callable from in-band context, so the notify side works
correctly. The sender parks in OOB stage at `evl_timedwait_sem()` and is promoted to OOB
by EVL's Dovetail layer when the post arrives.

---

## Semaphore Lifecycle

**Owner**: `m_popSem` (`struct evl_sem`) is a member of `DirettaSync`, not a property of
either thread. It is a shared synchronization object between the SDK worker and the sender.
Thread attach (`attachEvlThread()`) is a thread-local operation and is the wrong place
for its lifecycle.

**Creation**: `ensurePopSemCreated()` — a private method called at `open()` entry, before
the fast-path / slow-path branches. It is idempotent: a no-op if `m_popSemReady` is
already true. On first call it calls `evl_new_sem()` with initial count 0. If
`evl_new_sem()` succeeds, sets `m_popSemReady = true`. If it fails, logs a warning,
leaves `m_popSemReady = false`, and all three gated sites fall back to the condvar path
for the rest of the process lifetime.

**Drain after quiesce**: Old playback can still be generating `getNewStream()` posts until
`stop()` is reached inside `open()`. To avoid a race, the sem is not drained at `open()`
entry. Instead, `drainPopSem()` is called after each branch's quiesce point:

| Branch | Quiesce point | Drain after |
|--------|---------------|-------------|
| Fast path (same format) | `stop()` at line 497 | after `stop()` |
| DSD→PCM / DSD rate change | `m_workerThread.join()` at line 575 | after join |
| PCM rate change | `m_workerThread.join()` at line 625 | after join |
| PCM→DSD full reset | corresponding `m_workerThread.join()` | after join |

`drainPopSem()`: calls `evl_timedwait_sem()` in a zero-timeout loop until it returns
`-EAGAIN`. No sender is running at these points, so there is no concurrent wait.

**Destruction**: `~DirettaSync()` calls `evl_close_sem(&m_popSem)` under
`#ifdef HAVE_EVL` when `m_popSemReady` is true, after `disable()`.

---

## Change Map

### `diretta/DirettaSync.h`

**New member under `#ifdef HAVE_EVL`:**
```cpp
#ifdef HAVE_EVL
#include <evl/sem.h>
    struct evl_sem m_popSem {};
    bool m_popSemReady = false;
#endif
```

**New private method declarations:**
```cpp
bool ensurePopSemCreated();   // idempotent; returns false on evl_new_sem() failure
void drainPopSem();           // empties stale count after quiesce; no-op if !HAVE_EVL
```

**New public method `waitForPop()`:**
```cpp
void waitForPop(std::chrono::milliseconds timeout) {
#ifdef HAVE_EVL
    if (m_popSemReady) {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_nsec += timeout.count() * 1'000'000LL;
        if (ts.tv_nsec >= 1'000'000'000LL) { ts.tv_sec++; ts.tv_nsec -= 1'000'000'000LL; }
        evl_timedwait_sem(&m_popSem, &ts);
        return;
        // return value intentionally ignored — seenEpoch re-read after return
        // covers both a real pop wake and a timeout/spurious wake
    }
#endif
    // EVL unavailable or sem creation failed: condvar fallback handled by caller
}
```

**`notifySpaceAvailable()` — add sem post:**
```cpp
void notifySpaceAvailable() {
    m_spaceAvailable.notify_one();
#ifdef HAVE_EVL
    if (m_popSemReady)
        evl_post_sem(&m_popSem);
#endif
}
```

### `diretta/DirettaSync.cpp`

**`ensurePopSemCreated()` implementation:**
```cpp
bool DirettaSync::ensurePopSemCreated() {
#ifdef HAVE_EVL
    if (m_popSemReady) return true;
    int ret = evl_new_sem(&m_popSem, "/slim2diretta-pop");
    if (ret < 0) {
        std::cerr << "[DirettaSync] Warning: evl_new_sem failed (" << ret
                  << ") — falling back to condvar for pop-epoch wait" << std::endl;
        return false;
    }
    m_popSemReady = true;
    return true;
#else
    return false;
#endif
}
```

**`drainPopSem()` implementation:**
```cpp
void DirettaSync::drainPopSem() {
#ifdef HAVE_EVL
    if (!m_popSemReady) return;
    struct timespec zero = {0, 0};
    while (evl_timedwait_sem(&m_popSem, &zero) == 0) {}
#endif
}
```

**`open()` entry** — call `ensurePopSemCreated()` before the first branch, then call
`drainPopSem()` after each branch's quiesce point (as listed in the lifecycle table above).

**`getNewStream()` notify path** (lines 1793–1800) — replace `try_lock` / `notify_one`:
```cpp
m_popEpoch.fetch_add(1, std::memory_order_release);
#ifdef HAVE_EVL
if (m_popSemReady) {
    evl_post_sem(&m_popSem);
} else {
#endif
    if (m_flowMutex.try_lock()) {
        m_flowMutex.unlock();
        m_spaceAvailable.notify_one();
    }
#ifdef HAVE_EVL
}
#endif
```

**`~DirettaSync()`** — add after `disable()`:
```cpp
#ifdef HAVE_EVL
    if (m_popSemReady) {
        evl_close_sem(&m_popSem);
        m_popSemReady = false;
    }
#endif
```

### `src/main.cpp`

**Pop-epoch wait** (lines 1408–1416) — add EVL path before the condvar path:
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

This requires a trivial public accessor on `DirettaSync`:
```cpp
bool isPopSemReady() const {
#ifdef HAVE_EVL
    return m_popSemReady;
#else
    return false;
#endif
}
```

The `seenEpoch` re-read on the line after the wait handles both real-pop and
timeout/spurious wakes without needing a predicate inside the wait itself.

---

## What Does Not Change

- `m_flowMutex` and `m_spaceAvailable` — retained as-is for the drain stall
  (`src/main.cpp:1474`) and all `notifySpaceAvailable()` callers.
- The four `notifySpaceAvailable()` call sites (`src/main.cpp:1318`, `1572`, `1602`,
  `1624`) — no changes at those call sites; the sem post is handled inside
  `notifySpaceAvailable()` itself.
- The drain stall at `src/main.cpp:1474` — stays on `waitForSpace(lock, 5ms)`.
- `m_popEpoch` — retained for epoch tracking and end-of-stream detection.
- `attachEvlThread()` — no changes; sem lifecycle is independent of thread attach.

---

## Build / Runtime Behaviour Matrix

| `HAVE_EVL` | `evl_new_sem()` result | `m_popSemReady` | Active path |
|------------|------------------------|-----------------|-------------|
| not defined | n/a | false | condvar (`try_lock + notify_one`) |
| defined | success | true | `evl_post_sem` / `evl_timedwait_sem` |
| defined | failure | false | condvar (`try_lock + notify_one`) |
