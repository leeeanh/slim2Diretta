#include "FfmpegEofPolicy.h"

FfmpegEofAction decideFfmpegEofAction(bool hasParser,
                                      size_t available,
                                      bool eof,
                                      bool parserDrained,
                                      bool codecDrained) {
    if (!eof || available != 0 || codecDrained) {
        return FfmpegEofAction::None;
    }

    if (hasParser && !parserDrained) {
        return FfmpegEofAction::DrainParser;
    }

    return FfmpegEofAction::DrainCodec;
}
