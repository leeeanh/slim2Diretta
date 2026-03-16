# RT Jitter Cleanup — Phase 0 Design

**Date**: 2026-03-16
**Status**: Approved
**Scope**: `diretta/DirettaSync.h`, `diretta/DirettaSync.cpp`
**Relates to**: `2026-03-16-rt-jitter-cleanup-evl-roadmap-design.md` (full roadmap)

---

## Problem

The current Diretta playback path uses Linux `SCHED_FIFO` real-time scheduling but still has
three owned hot-path jitter sources in code we control:

1. On-demand scratch-buffer resize in `getNewStream()` — can allocate or touch fresh pages
   during a callback-side execution path, especially on format transitions.
2. `std::ostringstream` formatting in `DIRETTA_LOG_ASYNC` — heap allocation with
   unpredictable execution time on every hot-path trace call.
3. `std::cout` prefill diagnostics in `getNewStream()` (~line 1644) — direct stream I/O
   inside the worker callback, gated by `g_verbose`.

These are low-risk and can be addressed independently of any EVL work.

---

## Goals

1. Remove on-demand allocation from `getNewStream()`.
2. Replace `ostringstream` with bounded stack formatting in `DIRETTA_LOG_ASYNC`.
3. Unify the hot-path trace gate and remove direct iostream I/O from the callback path.

## Non-Goals

- EVL integration or scheduler changes.
- Replacing `std::mutex` / `std::condition_variable` flow control (Track B).
- Changing codec, network, or target-discovery behavior.

---

## Implementation

Three commits, one per change, each independently reviewable and measurable.

---

### Commit 1 — A1: Preallocate `m_streamData`

**Files**: `diretta/DirettaSync.cpp` only.

**Problem**: `getNewStream()` (~line 1605) currently resizes `m_streamData` on every
callback where the buffer size has changed:

```cpp
if (m_streamData.size() != static_cast<size_t>(currentBytesPerBuffer)) {
    m_streamData.resize(currentBytesPerBuffer);
}
```

The buffer size can vary call-to-call for 44.1kHz-family PCM due to the fractional-frame
accumulator (see ~line 1591), so the guard fires on some fraction of callbacks during
steady-state playback, not only on format transitions.

**Fix**: preallocate `m_streamData` to the maximum possible callback size at configuration
time, and remove the resize guard from `getNewStream()`.

Maximum callback sizes:
- **PCM**: `bytesPerBuffer + bytesPerFrame` when `framesRemainder != 0` (44.1kHz family),
  otherwise `bytesPerBuffer`. Both values are available at the end of `configureRingPCM()`.
- **DSD**: exact `bytesPerBuffer` from `configureRingDSD()` — no fractional accumulator.

**Ordering constraint**: `m_consumerStateGen.fetch_add()` is currently called near the top
of each `configureRing*()` function, before `m_bytesPerBuffer` is written and well before
`m_streamData` would be resized. `getNewStream()` checks the consumer generation to decide
whether to reload its cached format values; if it can observe the new generation before the
buffer is resized, it will cache the new `m_bytesPerBuffer` while the buffer is still sized
for the old format.

Fix: move `m_consumerStateGen.fetch_add()` to the end of each `configureRing*()` function,
after all worker-visible state is written — including `m_bytesPerBuffer`, `m_bytesPerFrame`,
`m_framesPerBufferRemainder`, ring and silence state, and `m_streamData.resize(...)`.

**Concurrency safety**:
- First open: worker thread is not yet started — no race possible.
- Format-change reopen paths: worker is joined (pattern at ~line 524) before
  `configureRing*()` is called — no concurrent `getNewStream()` can observe a
  partially-prepared buffer.
- `m_consumerStateGen` moved to the end ensures the worker cannot observe a
  partially-published new callback configuration; actual resize safety still relies on the
  worker not running during `configureRing*()`.

**Expected outcome**: allocation and page-fault risk removed from steady-state callback
execution; format-transition latency spikes reduced.

---

### Commit 2 — A2: Replace `ostringstream` with `snprintf` in `DIRETTA_LOG_ASYNC`

**Files**: `diretta/DirettaSync.h` (macro), `diretta/DirettaSync.cpp` (2 call sites).

**Problem**: `DIRETTA_LOG_ASYNC` currently uses `std::ostringstream` to format a message
before pushing it to `g_logRing`:

```cpp
#define DIRETTA_LOG_ASYNC(msg) do { \
    if (g_logRing && g_logLevel >= LogLevel::DEBUG) { \
        std::ostringstream _oss; \
        _oss << msg; \
        g_logRing->push(_oss.str().c_str()); \
    } \
} while(0)
```

`ostringstream` involves a heap allocation with unpredictable execution time. There are
exactly 2 call sites that use this macro, both in hot paths: `sendAudio` (~line 1509) and
`getNewStream` (~line 1708).

**Fix**:
- Rename macro signature to `DIRETTA_LOG_ASYNC(fmt, ...)`.
- Replace body with a 248-byte stack buffer (`char _buf[248]`) and
  `std::snprintf(_buf, sizeof(_buf), fmt, ##__VA_ARGS__)`, then push `_buf`.
- Buffer size matches `LogEntry::message` (declared at `DirettaSync.h:38`) exactly —
  truncation point is consistent across the macro and the ring entry.
- Use `##__VA_ARGS__` for zero-arg robustness (GNU extension, appropriate for this
  codebase).
- Add `<cstdio>` in `DirettaSync.h`; `<sstream>` can be dropped.
- Convert the 2 call sites to printf-style format strings. Use `%.1f` at the `getNewStream`
  site (~line 1708) to preserve the current `std::fixed << std::setprecision(1)` behavior.

**Framing**: this is a step-down in overhead — `snprintf` into a stack buffer avoids the
heap allocation and unpredictable execution time of `ostringstream`, but formatting work
still occurs in the hot path at `DEBUG` log level. It is not a complete removal of
hot-path formatting.

**Expected outcome**: hot-path trace calls have bounded, predictable stack-only cost.

---

### Commit 3 — A3: Gate unification, stats cleanup, and prefill logging

**Files**: `diretta/DirettaSync.cpp` only.

**Problem**: Three issues in `getNewStream()` and `sendAudio()`:

1. **Redundant double-gate** — the async trace sites at ~lines 1509 and 1708 are wrapped
   in `if (g_verbose)`, but `DIRETTA_LOG_ASYNC` already checks `g_logLevel >= DEBUG`
   internally. Since `--verbose` sets `g_logLevel = DEBUG` (see `main.cpp:425`), the outer
   `g_verbose` guard is redundant. It is gate duplication, not a meaningful extra filter.

2. **Stats inaccuracy** — `m_pushCount.fetch_add()` (~line 1509) is inside the
   `if (g_verbose)` block. `dumpStats()` (~line 1533) reports this counter. Outside verbose
   mode, the counter never advances, so the reported push count is always zero.

3. **Direct iostream in the callback** — the prefill diagnostic block at ~line 1644 uses
   `std::cout` with float division inside the worker callback, gated by `g_verbose`:

   ```cpp
   if (g_verbose) {
       static int prefillLogCount = 0;
       size_t avail = m_ringBuffer.getAvailable();
       if (prefillLogCount++ % 50 == 0) {
           float pct = ...;
           std::cout << "[Prefill] Waiting: " << avail << "/" << m_prefillTarget ...
       }
   }
   ```

   This is the only remaining `g_verbose`-gated hot-path with direct stream I/O inside the
   worker callback after A2.

**Fix**:

1. Remove the `if (g_verbose)` wrappers from the two async trace sites. The
   `DIRETTA_LOG_ASYNC` macro's own `g_logLevel >= DEBUG` check becomes the sole gate.
   This is gate unification — runtime behavior is unchanged.

2. Move `m_pushCount.fetch_add()` outside the trace-only block so `dumpStats()` reports
   accurate counts at all log levels.

3. Replace the `std::cout` prefill block with a `DIRETTA_LOG_ASYNC` call (printf-style,
   as updated in A2).

**Framing**: this commit is gate unification and stats cleanup, not full removal of
verbose-dependent hot-path work. The timing difference between verbose and non-verbose modes
narrows, but async formatting overhead in `DIRETTA_LOG_ASYNC` still runs at `DEBUG` log
level. In the current app flow, the async traces still effectively require `--verbose`
because `main.cpp:425` sets both `g_verbose = true` and `g_logLevel = DEBUG`.

**Expected outcome**: debug-mode timing behavior stays closer to release behavior; prefill
diagnostics no longer use direct stream I/O inside the worker callback; push-count stats
are accurate at all log levels.

---

## Measurement

Measure after each commit independently — jitter baseline, underrun count per session,
prefill completion time. If Phase 0 removes most timing variance, the EVL roadmap (Track B)
may not be needed.

---

## Risks

All three changes are low risk:

- A1: resize moves from a hot callback to a cold config path; safety guaranteed by worker
  join before config runs.
- A2: one-line macro change, 2 call-site conversions; no behavioral change at `DEBUG` level.
- A3: gate unification only; no behavioral change; stats fix is purely additive.
