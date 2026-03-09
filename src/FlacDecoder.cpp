/**
 * @file FlacDecoder.cpp
 * @brief FLAC stream decoder implementation using libFLAC
 *
 * Key design: two-phase decoding to handle large metadata blocks (album art).
 *
 * Phase 1 (metadata): Uses process_until_end_of_metadata(). If ABORT happens
 * (not enough data for all metadata), the decoder is deleted and recreated
 * on the next attempt with more accumulated data.
 *
 * Phase 2 (audio): Uses process_single() per frame. On ABORT (incomplete frame),
 * we rollback to the last confirmed frame boundary using get_decode_position()
 * and flush(). This avoids losing the read-ahead bytes that libFLAC keeps in
 * its internal buffer — we only compact up to the confirmed position, so those
 * bytes remain in our buffer and get re-provided on the next call.
 */

#include "FlacDecoder.h"
#include "LogLevel.h"

#include <cstring>
#include <algorithm>
#include <cstddef>

FlacDecoder::FlacDecoder() {
    m_inputBuffer.reserve(131072);  // 128KB — enough for most metadata blocks
    m_outputBuffer.reserve(16384);
}

FlacDecoder::~FlacDecoder() {
    if (m_decoder) {
        FLAC__stream_decoder_delete(m_decoder);
    }
}

bool FlacDecoder::initDecoder() {
    m_decoder = FLAC__stream_decoder_new();
    if (!m_decoder) {
        LOG_ERROR("[FLAC] Failed to create decoder");
        m_error = true;
        return false;
    }

    FLAC__StreamDecoderInitStatus status = FLAC__stream_decoder_init_stream(
        m_decoder,
        readCallback,
        nullptr,        // seek
        tellCallback,   // tell — needed for get_decode_position()
        nullptr,        // length
        nullptr,        // eof
        writeCallback,
        metadataCallback,
        errorCallback,
        this
    );

    if (status != FLAC__STREAM_DECODER_INIT_STATUS_OK) {
        LOG_ERROR("[FLAC] Init failed: " << FLAC__StreamDecoderInitStatusString[status]);
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = nullptr;
        m_error = true;
        return false;
    }

    m_initialized = true;
    return true;
}

size_t FlacDecoder::feed(const uint8_t* data, size_t len) {
    m_inputBuffer.insert(m_inputBuffer.end(), data, data + len);
    return len;
}

void FlacDecoder::setEof() {
    m_eof = true;
}

size_t FlacDecoder::readDecoded(int32_t* out, size_t maxFrames) {
    if (m_error || m_finished) return 0;

    // Lazy init on first decode attempt
    if (!m_initialized) {
        if (!initDecoder()) return 0;
    }

    // ================================================================
    // Phase 1: Process all metadata blocks
    // ================================================================
    // FLAC files can have large metadata (album art = 100KB+).
    // We pre-scan raw bytes and call libFLAC metadata processing only
    // once when all metadata bytes are confirmed in-buffer.

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

                    bool isLast = (buf[pos] & 0x80) != 0;
                    size_t blockLen = (static_cast<size_t>(buf[pos + 1]) << 16) |
                                      (static_cast<size_t>(buf[pos + 2]) << 8) |
                                      static_cast<size_t>(buf[pos + 3]);
                    size_t blockEnd = pos + 4 + blockLen;
                    if (blockEnd > FLAC_METADATA_PRESCAN_MAX_BYTES) {
                        LOG_ERROR("[FLAC] Metadata section exceeds "
                                  << FLAC_METADATA_PRESCAN_MAX_BYTES
                                  << " bytes hard cap");
                        m_error = true;
                        return 0;
                    }
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
                    if (isLast) {
                        foundLast = true;
                        break;
                    }
                }

                if (!foundLast) return 0;

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
                m_outputReserved = true;
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

    // ================================================================
    // Phase 2: Decode audio frames
    // ================================================================
    // On ABORT (incomplete frame), we rollback m_inputPos to the last
    // confirmed frame boundary (from get_decode_position), not to
    // savedInputPos. This ensures read-ahead bytes from the previous
    // successful frame are re-provided to libFLAC after flush().

    size_t outputAvailable = (m_outputBuffer.size() - m_outputPos) /
                              std::max(m_format.channels, 1u);

    while (outputAvailable < maxFrames) {
        size_t inputAvailable = m_inputBuffer.size() - m_inputPos;
        if (inputAvailable == 0 && !m_eof) break;

        if (!FLAC__stream_decoder_process_single(m_decoder)) {
            FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);

            if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
                m_finished = true;
                break;
            }

            if (state == FLAC__STREAM_DECODER_ABORTED) {
                // Rollback to last confirmed frame boundary
                size_t confirmedBufPos = static_cast<size_t>(
                    m_confirmedAbsolutePos - m_tellOffset);
                m_inputPos = confirmedBufPos;

                if (!m_eof && !m_error) {
                    FLAC__stream_decoder_flush(m_decoder);
                    break;  // Wait for more input
                }
                // At EOF with incomplete frame — stream truncated
                LOG_WARN("[FLAC] Stream ended with incomplete frame");
                m_finished = true;
                break;
            }

            // Other error
            LOG_ERROR("[FLAC] Decoder error state: "
                      << FLAC__StreamDecoderStateString[state]);
            m_error = true;
            break;
        }

        // Check for end of stream after successful process
        FLAC__StreamDecoderState state = FLAC__stream_decoder_get_state(m_decoder);
        if (state == FLAC__STREAM_DECODER_END_OF_STREAM) {
            m_finished = true;
            break;
        }

        // Update confirmed position after successful frame decode
        FLAC__uint64 absPos;
        if (FLAC__stream_decoder_get_decode_position(m_decoder, &absPos)) {
            m_confirmedAbsolutePos = absPos;
        }

        outputAvailable = (m_outputBuffer.size() - m_outputPos) /
                           std::max(m_format.channels, 1u);
    }

    // Compact input buffer: only remove confirmed-consumed bytes.
    // Read-ahead bytes (between confirmed pos and m_inputPos) stay in the buffer.
    // Invariant: confirmedBufPos <= m_inputPos <= m_inputBuffer.size().
    // m_tellOffset advances only when bytes are physically erased — keep the
    // three assignments together inside the gate.
    size_t confirmedBufPos = static_cast<size_t>(m_confirmedAbsolutePos - m_tellOffset);
    size_t inputUnread = m_inputBuffer.size() - confirmedBufPos;
    if (confirmedBufPos >= INPUT_COMPACT_THRESHOLD_BYTES &&
        (confirmedBufPos >= m_inputBuffer.size() / 2 ||
         inputUnread < INPUT_COMPACT_THRESHOLD_BYTES)) {
        m_inputBuffer.erase(m_inputBuffer.begin(),
                            m_inputBuffer.begin() +
                                static_cast<std::ptrdiff_t>(confirmedBufPos));
        m_inputPos   -= confirmedBufPos;
        m_tellOffset += confirmedBufPos;
    }

    // Copy available output frames
    if (!m_formatReady || m_format.channels == 0) return 0;

    size_t framesAvailable = (m_outputBuffer.size() - m_outputPos) / m_format.channels;
    size_t framesToCopy = std::min(framesAvailable, maxFrames);

    if (framesToCopy > 0) {
        size_t samplesToCopy = framesToCopy * m_format.channels;
        std::memcpy(out, m_outputBuffer.data() + m_outputPos,
                    samplesToCopy * sizeof(int32_t));
        m_outputPos += samplesToCopy;
        m_decodedSamples += framesToCopy;
    }

    // Compact output buffer only when the consumed prefix is large enough and
    // reclaiming it is worthwhile: either the majority is consumed, or the
    // remaining tail is itself below the threshold.
    size_t outputUnread = m_outputBuffer.size() - m_outputPos;
    if (m_outputPos >= OUTPUT_COMPACT_THRESHOLD_SAMPLES &&
        (m_outputPos >= m_outputBuffer.size() / 2 ||
         outputUnread < OUTPUT_COMPACT_THRESHOLD_SAMPLES)) {
        m_outputBuffer.erase(m_outputBuffer.begin(),
                             m_outputBuffer.begin() +
                                 static_cast<std::ptrdiff_t>(m_outputPos));
        m_outputPos = 0;
    }

    return framesToCopy;
}

void FlacDecoder::flush() {
    if (m_decoder) {
        FLAC__stream_decoder_delete(m_decoder);
        m_decoder = nullptr;
    }
    m_inputBuffer.clear();
    m_inputPos = 0;
    m_outputBuffer.clear();
    m_outputPos = 0;
    m_format = {};
    m_formatReady = false;
    m_shift = 0;
    m_maxBlocksize = 0;
    m_outputReserved = false;
    m_tellOffset = 0;
    m_confirmedAbsolutePos = 0;
    m_initialized = false;
    m_metadataDone = false;
    m_metadataPrescanDone = false;
    m_error = false;
    m_finished = false;
    m_eof = false;
    m_decodedSamples = 0;
}

// ============================================
// libFLAC Callbacks
// ============================================

FLAC__StreamDecoderReadStatus FlacDecoder::readCallback(
    const FLAC__StreamDecoder*, FLAC__byte buffer[],
    size_t* bytes, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    size_t available = self->m_inputBuffer.size() - self->m_inputPos;

    if (available == 0) {
        *bytes = 0;
        if (self->m_eof) {
            return FLAC__STREAM_DECODER_READ_STATUS_END_OF_STREAM;
        }
        return FLAC__STREAM_DECODER_READ_STATUS_ABORT;
    }

    size_t toRead = std::min(available, *bytes);
    std::memcpy(buffer, self->m_inputBuffer.data() + self->m_inputPos, toRead);
    self->m_inputPos += toRead;
    *bytes = toRead;

    return FLAC__STREAM_DECODER_READ_STATUS_CONTINUE;
}

FLAC__StreamDecoderWriteStatus FlacDecoder::writeCallback(
    const FLAC__StreamDecoder*, const FLAC__Frame* frame,
    const FLAC__int32* const buffer[], void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    uint32_t channels = frame->header.channels;
    uint32_t blocksize = frame->header.blocksize;

    // Fallback format detection from frame header (seek streams without STREAMINFO)
    if (!self->m_formatReady && channels > 0) {
        self->m_format.sampleRate = frame->header.sample_rate;
        self->m_format.bitDepth = frame->header.bits_per_sample;
        self->m_format.channels = channels;
        self->m_format.totalSamples = 0;  // Unknown after seek
        self->m_shift = 32 - static_cast<int>(frame->header.bits_per_sample);
        self->m_formatReady = true;

        LOG_INFO("[FLAC] Format (from frame): " << frame->header.sample_rate << " Hz, "
                 << frame->header.bits_per_sample << "-bit, "
                 << channels << " ch");
    }

    // First-frame fallback reserve (only fires when STREAMINFO had no max_blocksize)
    if (!self->m_outputReserved) {
        size_t target = std::min(
            static_cast<size_t>(blocksize) * channels * 4,
            size_t(65536));
        if (self->m_outputBuffer.capacity() < target) self->m_outputBuffer.reserve(target);
        self->m_outputReserved = true;
    }

    int shift = self->m_shift;

    // Reserve space for interleaved output
    size_t prevSize = self->m_outputBuffer.size();
    self->m_outputBuffer.resize(prevSize + blocksize * channels);
    int32_t* dst = self->m_outputBuffer.data() + prevSize;

    // Interleave and MSB-align samples
    for (uint32_t i = 0; i < blocksize; i++) {
        for (uint32_t ch = 0; ch < channels; ch++) {
            *dst++ = buffer[ch][i] << shift;
        }
    }

    return FLAC__STREAM_DECODER_WRITE_STATUS_CONTINUE;
}

void FlacDecoder::metadataCallback(
    const FLAC__StreamDecoder*,
    const FLAC__StreamMetadata* metadata, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    if (metadata->type == FLAC__METADATA_TYPE_STREAMINFO) {
        const auto& info = metadata->data.stream_info;
        self->m_format.sampleRate = info.sample_rate;
        self->m_format.bitDepth = info.bits_per_sample;
        self->m_format.channels = info.channels;
        self->m_format.totalSamples = info.total_samples;
        self->m_maxBlocksize = info.max_blocksize;

        self->m_shift = 32 - static_cast<int>(info.bits_per_sample);
        self->m_formatReady = true;

        LOG_INFO("[FLAC] Format: " << info.sample_rate << " Hz, "
                 << info.bits_per_sample << "-bit, "
                 << info.channels << " ch"
                 << (info.total_samples > 0
                     ? ", " + std::to_string(info.total_samples) + " samples"
                     : ""));
    }
}

void FlacDecoder::errorCallback(
    const FLAC__StreamDecoder*,
    FLAC__StreamDecoderErrorStatus status, void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);

    switch (status) {
        case FLAC__STREAM_DECODER_ERROR_STATUS_LOST_SYNC:
            // Normal during sync acquisition — silently ignore
            return;

        case FLAC__STREAM_DECODER_ERROR_STATUS_BAD_HEADER:
        case FLAC__STREAM_DECODER_ERROR_STATUS_FRAME_CRC_MISMATCH:
            // Recoverable — decoder will search for next valid frame
            LOG_DEBUG("[FLAC] " << FLAC__StreamDecoderErrorStatusString[status]);
            return;

        default:
            // UNPARSEABLE_STREAM or unknown — fatal
            LOG_ERROR("[FLAC] Decode error: " << FLAC__StreamDecoderErrorStatusString[status]);
            self->m_error = true;
            break;
    }
}

FLAC__StreamDecoderTellStatus FlacDecoder::tellCallback(
    const FLAC__StreamDecoder*, FLAC__uint64* absolute_byte_offset,
    void* clientData)
{
    auto* self = static_cast<FlacDecoder*>(clientData);
    *absolute_byte_offset = self->m_tellOffset + self->m_inputPos;
    return FLAC__STREAM_DECODER_TELL_STATUS_OK;
}
