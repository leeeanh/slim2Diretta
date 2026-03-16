#include "DecoderDrainPolicy.h"

bool shouldContinuePostEofDrain(size_t framesRead,
                                bool decoderFinished,
                                bool decoderError) {
    if (decoderError) return false;
    if (decoderFinished && framesRead == 0) return false;
    return true;
}
