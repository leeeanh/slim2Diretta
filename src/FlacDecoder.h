/**
 * @file FlacDecoder.h
 * @brief FLAC stream decoder using libFLAC
 *
 * Wraps libFLAC's stream decoder API with a push/pull interface:
 * - feed() pushes encoded FLAC data
 * - readDecoded() pulls decoded S32_LE interleaved samples
 *
 * Handles streaming with incomplete data:
 * - During metadata: byte-level pre-scan gates init; seek streams skip phase entirely
 * - During audio: ABORT → flush() (back to SEARCH_FOR_FRAME_SYNC)
 */

#ifndef SLIM2DIRETTA_FLAC_DECODER_H
#define SLIM2DIRETTA_FLAC_DECODER_H

#include "Decoder.h"
#include <FLAC/stream_decoder.h>
#include <vector>

class FlacDecoder : public Decoder {
public:
    FlacDecoder();
    ~FlacDecoder() override;

    size_t feed(const uint8_t* data, size_t len) override;
    void setEof() override;
    size_t readDecoded(int32_t* out, size_t maxFrames) override;
    bool isFormatReady() const override { return m_formatReady; }
    DecodedFormat getFormat() const override { return m_format; }
    bool isFinished() const override { return m_finished; }
    bool hasError() const override { return m_error; }
    uint64_t getDecodedSamples() const override { return m_decodedSamples; }
    void flush() override;

private:
    bool initDecoder();

    // libFLAC callbacks
    static FLAC__StreamDecoderReadStatus readCallback(
        const FLAC__StreamDecoder* decoder, FLAC__byte buffer[],
        size_t* bytes, void* clientData);

    static FLAC__StreamDecoderWriteStatus writeCallback(
        const FLAC__StreamDecoder* decoder, const FLAC__Frame* frame,
        const FLAC__int32* const buffer[], void* clientData);

    static void metadataCallback(
        const FLAC__StreamDecoder* decoder,
        const FLAC__StreamMetadata* metadata, void* clientData);

    static void errorCallback(
        const FLAC__StreamDecoder* decoder,
        FLAC__StreamDecoderErrorStatus status, void* clientData);

    static FLAC__StreamDecoderTellStatus tellCallback(
        const FLAC__StreamDecoder* decoder,
        FLAC__uint64* absolute_byte_offset, void* clientData);

    FLAC__StreamDecoder* m_decoder = nullptr;

    // Input buffer (fed by caller)
    std::vector<uint8_t> m_inputBuffer;
    size_t m_inputPos = 0;
    bool m_eof = false;

    // Output buffer (filled by write callback).
    // m_outputPos is a persistent read offset — not reset after every copy.
    // Compaction is deferred until OUTPUT_COMPACT_THRESHOLD_SAMPLES of consumed
    // samples have accumulated and the move is worthwhile.
    std::vector<int32_t> m_outputBuffer;
    size_t m_outputPos = 0;

    // Compaction thresholds — unit matches the buffer's element unit.
    // Input threshold: bytes (m_inputBuffer is uint8_t).
    // Output threshold: int32_t samples (m_outputBuffer is int32_t).
    static constexpr size_t INPUT_COMPACT_THRESHOLD_BYTES    = 16384;
    static constexpr size_t OUTPUT_COMPACT_THRESHOLD_SAMPLES = 16384;
    static constexpr size_t FLAC_METADATA_PRESCAN_MAX_BYTES  = 2 * 1024 * 1024;

    // Format from STREAMINFO metadata
    DecodedFormat m_format;
    bool m_formatReady = false;
    int m_shift = 0;  // Left shift for MSB alignment (32 - bitDepth)
    uint32_t m_maxBlocksize = 0;  // From STREAMINFO; 0 if not yet seen
    bool m_outputReserved = false;   // True once output buffer has been pre-reserved

    // Stream position tracking for accurate rollback on ABORT
    // libFLAC reads ahead into an internal buffer. On flush(), those bytes
    // are lost. We use get_decode_position() to find the exact frame boundary
    // and rollback/compact to there (not to m_inputPos which includes read-ahead).
    uint64_t m_tellOffset = 0;            // Cumulative bytes removed by compaction
    uint64_t m_confirmedAbsolutePos = 0;  // Last frame boundary (absolute stream pos)

    // State
    bool m_initialized = false;
    bool m_metadataDone = false;
    bool m_metadataPrescanDone = false;  // Pre-scan confirmed all metadata bytes present
                                         // (or stream confirmed as seek/mid-frame start)
    bool m_error = false;
    bool m_finished = false;
    uint64_t m_decodedSamples = 0;
};

#endif // SLIM2DIRETTA_FLAC_DECODER_H
