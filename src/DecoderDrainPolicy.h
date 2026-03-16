#ifndef SLIM2DIRETTA_DECODER_DRAIN_POLICY_H
#define SLIM2DIRETTA_DECODER_DRAIN_POLICY_H

#include <cstddef>

bool shouldContinuePostEofDrain(size_t framesRead,
                                bool decoderFinished,
                                bool decoderError);

#endif // SLIM2DIRETTA_DECODER_DRAIN_POLICY_H
