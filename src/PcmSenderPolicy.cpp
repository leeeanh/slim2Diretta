#include "PcmSenderPolicy.h"

#include <cstdint>

size_t acceptedFramesFromConsumedPcmBytes(size_t consumedBytes, int channels) {
    if (channels <= 0) {
        return 0;
    }

    size_t bytesPerFrame = static_cast<size_t>(channels) * sizeof(int32_t);
    if (bytesPerFrame == 0) {
        return 0;
    }

    return consumedBytes / bytesPerFrame;
}

bool shouldDeclareNaturalPcmEnd(bool producerDone,
                                size_t cacheFrames,
                                size_t direttaBufferedBytes) {
    return producerDone && cacheFrames == 0 && direttaBufferedBytes == 0;
}
