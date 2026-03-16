#ifndef SLIM2DIRETTA_FFMPEG_EOF_POLICY_H
#define SLIM2DIRETTA_FFMPEG_EOF_POLICY_H

#include <cstddef>

enum class FfmpegEofAction {
    None,
    DrainParser,
    DrainCodec,
};

FfmpegEofAction decideFfmpegEofAction(bool hasParser,
                                      size_t available,
                                      bool eof,
                                      bool parserDrained,
                                      bool codecDrained);

#endif // SLIM2DIRETTA_FFMPEG_EOF_POLICY_H
