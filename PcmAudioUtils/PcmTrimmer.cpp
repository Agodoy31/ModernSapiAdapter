/**
 * @file PcmTrimmer.cpp
 * @brief Implementation of ultra-low latency PCM silence trimming algorithms.
 */

#include "pch.h"
#include "PcmTrimmer.h"
#include <cmath>

size_t PcmTrimmer::FindLeadingAudioOffset(const uint8_t* pcmData, size_t byteCount, int16_t noiseFloorThreshold) {
    if (!pcmData || byteCount < 2) return 0;

    const int16_t* samples = reinterpret_cast<const int16_t*>(pcmData);
    size_t numSamples = byteCount / 2;

    for (size_t i = 0; i < numSamples; ++i) {
        if (std::abs(samples[i]) > noiseFloorThreshold) {
            return i * 2;
        }
    }

    return byteCount;
}

size_t PcmTrimmer::TrimTrailingSilence(const uint8_t* pcmData, size_t byteCount, int16_t noiseFloorThreshold) {
    if (!pcmData || byteCount < 2) return 0;

    const int16_t* samples = reinterpret_cast<const int16_t*>(pcmData);
    size_t numSamples = byteCount / 2;

    for (size_t i = numSamples; i > 0; --i) {
        if (std::abs(samples[i - 1]) > noiseFloorThreshold) {
            return i * 2;
        }
    }

    return 0;
}
