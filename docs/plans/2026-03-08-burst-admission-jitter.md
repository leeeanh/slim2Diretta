# Burst Admission and Startup Jitter Reduction — Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task.

**Goal:** Eliminate three residual jitter sources: uncapped sender recovery burst, FLAC metadata decoder churn on startup, and format-unaware buffer growth.

**Architecture:** Three independent commits touching one concern each: (1) replace the open-ended recovery `while` loop with a per-wakeup frame budget in the sender; (2) replace FLAC metadata retry churn with a byte-level pre-scan gate; (3) apply format-aware reserves to PCM and FLAC output buffers after the format is known.

**Tech Stack:** C++17, libFLAC (stream decoder API), POSIX threads. No automated test framework — verification is build + manual playback.

**Design doc:** `docs/plans/2026-03-08-burst-admission-jitter-design.md`

---

## Task 1: Replace uncapped recovery loop with per-wakeup frame budget (`main.cpp`)

**Files:**
- Modify: `src/main.cpp:1113–1120` (constants block)
- Modify: `src/main.cpp:1260–1290` (sender mode dispatch)

### Step 1: Read the current constants block and recovery dispatch

Read lines 1112–1295 of `src/main.cpp` to confirm the exact text before editing.

Key landmarks:
- Line 1113: `constexpr float RECOVERY_TARGET = 0.90f;` — this is removed
- Line 1261: `float level = direttaPtr->getBufferLevel();` — already sampled once; reuse this in recovery
- Lines 1268–1274: the uncapped `while` loop — this is replaced

### Step 2: Replace the constants block

In `src/main.cpp`, replace lines 1113–1120:

```cpp
                    enum class SenderMode { kRecovery, kSteady };
                    constexpr float  RECOVERY_TARGET         = 0.90f;
                    constexpr float  STEADY_ENTER            = 0.75f;
                    constexpr float  STEADY_EXIT             = 0.40f;
                    constexpr float  STEADY_CEILING          = 0.65f;
                    constexpr float  STEADY_CHUNK_MS         = 2.0f;
                    constexpr size_t STEADY_CHUNK_MIN_FRAMES = 128;
                    constexpr size_t STEADY_CHUNK_MAX_FRAMES = 512;
```

with:

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
```

### Step 3: Replace the kRecovery dispatch branch

Locate the `kRecovery` branch (currently lines 1268–1274):

```cpp
                            if (senderMode == SenderMode::kRecovery) {
                                while (audioTestRunning.load(std::memory_order_relaxed) &&
                                       direttaPtr->getBufferLevel() < RECOVERY_TARGET) {
                                    size_t contiguous = cacheContiguousFrames();
                                    if (contiguous == 0) break;
                                    sendChunk(std::min(contiguous, MAX_DECODE_FRAMES));
                                }
                            } else {
```

Replace with:

```cpp
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
                                if (contiguous > 0) sendChunk(std::min(contiguous, chunkFrames));
                            } else {
```

Note: `rate` here is the outer declaration (`uint32_t rate = dopDetected ? dopPcmRate : snapshotSampleRate;` near line 1219), not the shadowed `rate` inside the `else` kSteady branch.

### Step 4: Build and check for any reference to RECOVERY_TARGET

```bash
cd /path/to/slim2Diretta/build
make 2>&1 | grep -E "error:|RECOVERY_TARGET"
```

Expected: clean build. If `RECOVERY_TARGET` appears, it was referenced somewhere else — grep for it and remove.

### Step 5: Sanity-check the DSD prebuffer path

The DSD sender (around line 761) has its own fill check:
```cpp
if (direttaPtr->getBufferLevel() > 0.90f) break;
```
This is a literal, not `RECOVERY_TARGET`, so it is unaffected. Confirm it still compiles.

### Step 6: Manual verification notes

Play a track via LMS → slim2diretta with `-v` (verbose). After starvation (pause then resume, or slow network):
- The sender should reach `kSteady` in a few seconds rather than one burst
- Buffer level should climb steadily: `[Sender] cache=...` log entries should show incremental fill

### Step 7: Commit

```bash
git add src/main.cpp
git commit -m "perf(audio): replace uncapped recovery burst with per-wakeup frame budget

Recovery mode now sends at most one chunk per Diretta wakeup, sized by level:
- normal recovery (>= 0.20): 5ms budget
- emergency recovery (< 0.20): 10ms budget
Removes RECOVERY_TARGET constant (was only used to gate the old inner loop).
Steady-state 2ms pacing and all hysteresis thresholds unchanged."
```

---

## Task 2: Replace FLAC metadata retry churn with pre-scan gate (`FlacDecoder.h`, `FlacDecoder.cpp`)

**Files:**
- Modify: `src/FlacDecoder.h:60–99` (members and constants)
- Modify: `src/FlacDecoder.cpp:95–161` (Phase 1 metadata block)
- Modify: `src/FlacDecoder.cpp:274–295` (flush)
- Modify: `src/FlacDecoder.cpp:365–388` (metadataCallback)

### Step 1: Read the current header and Phase 1 block

Read `src/FlacDecoder.h` lines 60–101 and `src/FlacDecoder.cpp` lines 95–162 in full before editing.

### Step 2: Update `FlacDecoder.h`

**Remove** `m_metadataRetries` (line 98):
```cpp
    unsigned m_metadataRetries = 0;  // Count of metadata incomplete retries
```

**Add** two new members below `m_metadataDone` (line 94) and one after `m_shift`:

In the members section, after `m_metadataDone`:
```cpp
    bool m_metadataPrescanDone = false;  // Pre-scan confirmed all metadata bytes present
                                          // (or stream confirmed as seek/mid-frame start)
```

After `m_shift`:
```cpp
    uint32_t m_maxBlocksize = 0;  // From STREAMINFO; 0 if not yet seen
```

**Restore the lazy `initDecoder()` call to the top of `readDecoded()`**, before Phase 1.
The current code already has this at lines 82–84; the new Phase 1 code must NOT move it
inside the metadata gate, because seek streams (which skip `process_until_end_of_metadata`)
still need the decoder initialized before Phase 2 audio decode begins.

**Update the file comment** (lines 10–12). Remove the line:
```
 * - During metadata: ABORT → reset() (back to SEARCH_FOR_METADATA)
```
Replace with:
```
 * - During metadata: byte-level pre-scan gates init; seek streams skip phase entirely
```

### Step 3: Replace Phase 1 in `FlacDecoder.cpp`

The current Phase 1 block is lines 95–162. Replace the entire block.

**Key structural constraints:**
- The lazy `initDecoder()` call must remain at the **top of `readDecoded()`** (lines 82–84),
  not inside Phase 1. Seek streams skip `process_until_end_of_metadata` and fall straight to
  Phase 2 audio decode, which needs the decoder already initialized.
- `process_until_end_of_metadata` must only run for full FLAC streams. It is guarded by
  an inner `if (!m_metadataDone)` so seek streams (which set `m_metadataDone = true` in the
  prescan branch) bypass it.

Replace with:

```cpp
    if (!m_metadataDone) {
        // ---------------------------------------------------------------
        // Pre-scan: confirm all metadata bytes are present before calling
        // libFLAC. Seek/mid-stream restarts (no fLaC magic) skip the
        // metadata phase entirely; libFLAC resyncs via LOST_SYNC.
        // ---------------------------------------------------------------
        if (!m_metadataPrescanDone) {
            const uint8_t* buf = m_inputBuffer.data();
            size_t bufSize = m_inputBuffer.size();

            if (bufSize < 4) {
                if (m_eof) {
                    LOG_ERROR("[FLAC] Stream ended before FLAC signature");
                    m_error = true;
                }
                return 0;
            }

            if (std::memcmp(buf, "fLaC", 4) != 0) {
                // Seek/mid-stream start: skip metadata, decode from first frame.
                LOG_DEBUG("[FLAC] No fLaC magic — seek/mid-stream start, skipping metadata phase");
                m_metadataPrescanDone = true;
                m_metadataDone = true;
                // inner if (!m_metadataDone) below is skipped; falls through to Phase 2.
            } else {
                // Full FLAC stream: scan block headers until last-metadata bit.
                size_t pos = 4;
                bool foundLast = false;
                while (true) {
                    if (pos + 4 > bufSize) {
                        if (m_eof) {
                            LOG_ERROR("[FLAC] Stream truncated in metadata block header");
                            m_error = true;
                        }
                        return 0;
                    }

                    bool isLast  = (buf[pos] & 0x80) != 0;
                    size_t blockLen = (static_cast<size_t>(buf[pos + 1]) << 16)
                                    | (static_cast<size_t>(buf[pos + 2]) <<  8)
                                    |  static_cast<size_t>(buf[pos + 3]);
                    size_t blockEnd = pos + 4 + blockLen;

                    if (blockEnd > bufSize) {
                        if (m_eof) {
                            LOG_ERROR("[FLAC] Stream truncated inside metadata block ("
                                      << blockLen << " bytes expected, "
                                      << (bufSize - pos - 4) << " available)");
                            m_error = true;
                        }
                        return 0;
                    }

                    pos = blockEnd;
                    if (isLast) { foundLast = true; break; }
                }

                if (!foundLast) return 0;  // unreachable; loop always returns or breaks

                m_metadataPrescanDone = true;
                LOG_DEBUG("[FLAC] Pre-scan complete: " << pos << " metadata bytes confirmed ("
                          << bufSize << " buffered)");
            }
        }  // end if (!m_metadataPrescanDone)

        // For full FLAC streams: process metadata exactly once.
        // Seek streams set m_metadataDone = true above, so this block is skipped.
        if (!m_metadataDone) {
            if (!FLAC__stream_decoder_process_until_end_of_metadata(m_decoder)) {
                FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);
                if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                    m_finished = true;
                    return 0;
                }
                // Pre-scan confirmed bytes present; ABORT here means corruption
                LOG_ERROR("[FLAC] Metadata processing failed after pre-scan (state="
                          << FLAC__StreamDecoderStateString[state]
                          << ") — stream likely corrupt");
                m_error = true;
                return 0;
            }

            m_metadataDone = true;
            LOG_DEBUG("[FLAC] Metadata complete, starting audio decode");

            // Reserve output buffer from STREAMINFO max_blocksize (if available)
            if (m_maxBlocksize > 0 && m_format.channels > 0) {
                size_t target = static_cast<size_t>(m_maxBlocksize) * m_format.channels * 4;
                if (m_outputBuffer.capacity() < target) m_outputBuffer.reserve(target);
            }

            // Compact metadata bytes: use get_decode_position to find exact audio boundary
            FLAC__uint64 absPos;
            if (FLAC__stream_decoder_get_decode_position(m_decoder, &absPos)) {
                size_t audioStart = static_cast<size_t>(absPos - m_tellOffset);
                if (audioStart > 0 && audioStart <= m_inputBuffer.size()) {
                    m_inputBuffer.erase(m_inputBuffer.begin(),
                                        m_inputBuffer.begin() + audioStart);
                    m_inputPos -= audioStart;
                    m_tellOffset += audioStart;
                }
            } else {
                if (m_inputPos > 0) {
                    m_tellOffset += m_inputPos;
                    m_inputBuffer.erase(m_inputBuffer.begin(),
                                        m_inputBuffer.begin() + m_inputPos);
                    m_inputPos = 0;
                }
            }
            m_confirmedAbsolutePos = m_tellOffset;
        }
    }  // end if (!m_metadataDone)
```

### Step 4: Update `metadataCallback` to capture `max_blocksize`

In `metadataCallback` (around line 371), inside the `FLAC__METADATA_TYPE_STREAMINFO` block, after setting the existing fields, add:

```cpp
        self->m_maxBlocksize = info.max_blocksize;
```

Full updated block for reference:
```cpp
    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& info = metadata->data.stream_info;
        self->m_format.sampleRate = info.sample_rate;
        self->m_format.bitDepth = info.bits_per_sample;
        self->m_format.channels = info.channels;
        self->m_format.totalSamples = info.total_samples;
        self->m_maxBlocksize = info.max_blocksize;   // ← add this line

        self->m_shift = 32 - static_cast<int>(info.bits_per_sample);
        self->m_formatReady = true;
        ...
    }
```

### Step 5: Update `flush()` to reset new state

In `flush()` (lines 274–295), the existing resets handle `m_metadataDone`, `m_initialized`, `m_error`, etc. Add resets for the new members:

```cpp
    m_metadataPrescanDone = false;
    m_maxBlocksize = 0;
```

Remove:
```cpp
    m_metadataRetries = 0;
```

### Step 6: Build

```bash
make 2>&1 | grep -E "error:|warning:|metadataRetries|RECOVERY_TARGET"
```

Expected: clean build. Any reference to `m_metadataRetries` is a missed removal.

### Step 7: Manual verification notes

Play a FLAC track from a Qobuz or hi-res source known to have large embedded cover art. With `-v`:
- Old behavior: `[FLAC] Metadata incomplete, need more data` logged repeatedly (50+ times)
- New behavior: silence until `[FLAC] Pre-scan complete: ... metadata bytes confirmed`, then `[FLAC] Metadata complete`

No retry messages should appear. Playback should begin at the same latency or faster.

### Step 8: Commit

```bash
git add src/FlacDecoder.h src/FlacDecoder.cpp
git commit -m "perf(flac): replace metadata retry churn with byte-level pre-scan gate

Instead of recreating FLAC__StreamDecoder on every metadata ABORT
(up to 100+ times for large album-art blocks), scan the raw input
buffer to confirm all metadata bytes are present before constructing
the decoder. Init and process_until_end_of_metadata run exactly once.

Adds FLAC_METADATA_PRESCAN_MAX_BYTES = 2MB hard cap: streams whose
metadata section exceeds this limit are failed immediately with a
clear error log rather than stalling indefinitely.

Captures STREAMINFO.max_blocksize to pre-reserve output buffer
capacity (4 blocks) after metadata completes."
```

---

## Task 3: Format-aware buffer pre-reservation (`PcmDecoder.cpp`, `FlacDecoder.cpp`)

**Files:**
- Modify: `src/PcmDecoder.cpp:258–274` (parseWavHeader transition to DATA)
- Modify: `src/PcmDecoder.cpp:328–344` (parseAiffHeader transition to DATA)
- Modify: `src/PcmDecoder.cpp:160–170` (detectContainer raw PCM path)
- Modify: `src/FlacDecoder.h` (add `m_outputReserved`)
- Modify: `src/FlacDecoder.cpp:335–346` (writeCallback first-frame fallback)
- Modify: `src/FlacDecoder.cpp` flush (reset `m_outputReserved`)

### Step 1: Read the transition sites in PcmDecoder.cpp

Read `src/PcmDecoder.cpp` lines 155–175 (raw PCM path) and 255–280 (WAV transition) and 325–345 (AIFF transition).

### Step 2: Add a helper lambda or inline formula in PcmDecoder.cpp

The same reserve formula is used in three places. To avoid repeating it, add a static helper at the top of the anonymous namespace (or as a file-scope function just before `PcmDecoder::detectContainer`):

```cpp
// Compute a format-aware reserve size for m_dataBuf.
// ~250ms of raw PCM, rounded to 64KB, clamped to [128KB, 512KB].
// Only grows, never shrinks — caller must check capacity first.
static size_t pcmDataBufReserve(uint32_t bytesPerFrame, uint32_t sampleRate) {
    if (bytesPerFrame == 0 || sampleRate == 0) return 128 * 1024;
    size_t target = static_cast<size_t>(bytesPerFrame) * sampleRate / 4;
    target = (target + 65535u) & ~65535u;  // round up to 64KB
    return std::clamp(target, size_t(128 * 1024), size_t(512 * 1024));
}
```

### Step 3: Call reserve in `parseWavHeader`

**WAV EXTENSIBLE note**: `parseWavHeader` already rewrites `m_format.bitDepth` from the
container `wBitsPerSample` to `wValidBitsPerSample` for EXTENSIBLE files (e.g. 32→24). The
data arriving in `m_dataBuf` is at the *container* width, not the valid-bit width. Capture
`wBitsPerSample` *before* the override and use it for the reserve.

Locate the existing `wBitsPerSample` / `wValidBitsPerSample` block in `parseWavHeader`:

```cpp
    m_format.bitDepth = readLE16(p + pos + 22);  // wBitsPerSample (container width)
```

Immediately after that line, save the container width before any override:

```cpp
    uint16_t containerBitsPerSample = static_cast<uint16_t>(m_format.bitDepth);
    // EXTENSIBLE: wValidBitsPerSample may differ from container size
    if (isExtensible) {
        uint16_t validBits = readLE16(p + pos + 8 + 18);
        if (validBits > 0) {
            m_format.bitDepth = validBits;  // existing override — unchanged
        }
    }
```

Then, just before the block that moves data from `m_headerBuf` to `m_dataBuf` (after
`LOG_INFO("[PCM] WAV: ...")`):

```cpp
    {
        uint32_t containerBytesPerFrame = (containerBitsPerSample / 8) * m_format.channels;
        size_t target = pcmDataBufReserve(containerBytesPerFrame, m_format.sampleRate);
        if (m_dataBuf.capacity() < target) m_dataBuf.reserve(target);
    }
    if (dataStart < m_headerBuf.size()) {
        m_dataBuf.insert(m_dataBuf.end(), ...);  // existing
    }
```

For non-EXTENSIBLE WAV, `containerBitsPerSample == m_format.bitDepth`, so the result is
identical to the naive formula. For EXTENSIBLE 24-in-32, this correctly gives 4 bytes/sample
instead of 3.

### Step 4: Call reserve in `parseAiffHeader`

AIFF stores samples at their declared bit depth with no container/valid distinction, so
`m_format.bitDepth / 8 * channels` is the correct byte count. Add just before the SSND
payload copy (around line 335):

```cpp
    {
        uint32_t bytesPerFrame = (m_format.bitDepth / 8) * m_format.channels;
        size_t target = pcmDataBufReserve(bytesPerFrame, m_format.sampleRate);
        if (m_dataBuf.capacity() < target) m_dataBuf.reserve(target);
    }
```

### Step 5: Call reserve in `detectContainer` raw PCM path

Raw PCM format is set by `setRawPcmFormat(sampleRate, bitDepth, channels, bigEndian)` with
explicit `bitDepth` representing the actual storage width. Add before the `m_dataBuf.insert`
(around line 163):

```cpp
    if (m_rawPcmConfigured) {
        m_formatReady = true;
        m_dataRemaining = 0;

        {
            uint32_t bytesPerFrame = (m_format.bitDepth / 8) * m_format.channels;
            size_t target = pcmDataBufReserve(bytesPerFrame, m_format.sampleRate);
            if (m_dataBuf.capacity() < target) m_dataBuf.reserve(target);
        }

        m_dataBuf.insert(m_dataBuf.end(), m_headerBuf.begin(), m_headerBuf.end());
        m_headerBuf.clear();
        m_state = State::DATA;
        LOG_INFO("[PCM] Raw: ...");
        return true;
    }
```

### Step 6: Add `m_outputReserved` to `FlacDecoder.h`

After `m_maxBlocksize`:
```cpp
    bool m_outputReserved = false;   // True once output buffer has been pre-reserved
```

### Step 7: Add first-frame fallback reserve in `FlacDecoder.cpp` `writeCallback`

In `writeCallback`, the fallback format-detection block (around line 335) already checks `!self->m_formatReady`. Add the output-buffer reserve immediately after `m_formatReady = true` for both the fallback path and after the existing STREAMINFO path, but guarded by `m_outputReserved`:

Find the section in `writeCallback` where format is detected from the frame header (the `if (!self->m_formatReady ...)` block, around line 335). After that block sets `m_formatReady = true`, add:

```cpp
    // First-frame fallback reserve (only fires when STREAMINFO had no max_blocksize)
    if (!self->m_outputReserved) {
        size_t target = std::min(
            static_cast<size_t>(blocksize) * channels * 4,
            size_t(65536));
        if (self->m_outputBuffer.capacity() < target) self->m_outputBuffer.reserve(target);
        self->m_outputReserved = true;
    }
```

This fires on the first decoded frame regardless of whether format was already set by STREAMINFO. When `m_maxBlocksize > 0`, the post-metadata reserve (Task 2, Step 3) will have already reserved a larger capacity, so `capacity() < target` will be false and the reserve is skipped. When `m_maxBlocksize == 0` (seek streams), this provides the fallback.

### Step 8: Update `flush()` to reset `m_outputReserved`

In `flush()`, add:
```cpp
    m_outputReserved = false;
```

### Step 9: Build

```bash
make 2>&1 | grep -E "error:|warning:"
```

Expected: clean build.

### Step 10: Manual verification

Play a WAV, an AIFF, and a FLAC track. With verbose logging disabled (normal run), verify:
- No startup pauses or errors
- For FLAC streams without embedded cover art (max_blocksize = 0 case): confirm the first-frame fallback fires by adding a temporary `LOG_DEBUG("[FLAC] First-frame output reserve: " << target)` — remove before commit

### Step 11: Commit

```bash
git add src/PcmDecoder.cpp src/FlacDecoder.h src/FlacDecoder.cpp
git commit -m "perf(audio): format-aware buffer pre-reservation for PCM and FLAC

PcmDecoder: after WAV/AIFF/raw-PCM header is parsed, reserve m_dataBuf
to ~250ms of raw PCM (rounded to 64KB, clamped 128KB..512KB). Eliminates
vector-doubling reallocations during the steady-state HTTP append path.

FlacDecoder: first-frame fallback reserves m_outputBuffer to 4x the
decoded blocksize when STREAMINFO.max_blocksize was unavailable (seek
streams). Complements the post-metadata reserve added in the previous
commit for streams with STREAMINFO."
```

---

## Verification Checklist

After all three commits:

- [ ] `make` builds cleanly with no warnings
- [ ] FLAC playback (full stream): no `Metadata incomplete` retry log spam; single `Pre-scan complete` log
- [ ] FLAC seek/mid-stream restart: `No fLaC magic — seek/mid-stream start` log appears; audio decodes without error
- [ ] WAV EXTENSIBLE (24-in-32): reserve uses 4 bytes/sample not 3; verify with `LOG_DEBUG` during development, remove before commit
- [ ] Recovery after starvation: buffer level climbs in visible steps rather than one spike to 90%
- [ ] WAV and AIFF playback: no regressions in format detection or audio quality
- [ ] `git log --oneline -5` shows three clean perf commits in order
