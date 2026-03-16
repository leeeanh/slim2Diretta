#ifndef SLIM2DIRETTA_PCM_SENDER_POLICY_H
#define SLIM2DIRETTA_PCM_SENDER_POLICY_H

#include <cstddef>

size_t acceptedFramesFromConsumedPcmBytes(size_t consumedBytes, int channels);
bool shouldDeclareNaturalPcmEnd(bool producerDone,
                                size_t cacheFrames,
                                size_t direttaBufferedBytes);

#endif // SLIM2DIRETTA_PCM_SENDER_POLICY_H
