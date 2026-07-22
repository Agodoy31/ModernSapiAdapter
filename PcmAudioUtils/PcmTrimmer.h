/**
 * @file PcmTrimmer.h
 * @brief Ultra-low latency PCM silence trimming utility.
 */

#pragma once

class PcmTrimmer {
public:
    /**
     * @brief Scans forward to locate the byte offset where audible sound begins.
     * @param pcmData Pointer to 16-bit PCM audio byte buffer.
     * @param byteCount Size of the buffer in bytes.
     * @param noiseFloorThreshold Absolute amplitude threshold below which samples are considered silent.
     * @return Byte offset of the first sample exceeding the threshold.
     */
    static size_t FindLeadingAudioOffset(const uint8_t* pcmData, size_t byteCount, int16_t noiseFloorThreshold = 5);

    /**
     * @brief Scans backward from the end of the buffer to locate where audible sound stops.
     * @param pcmData Pointer to 16-bit PCM audio byte buffer.
     * @param byteCount Size of the buffer in bytes.
     * @param noiseFloorThreshold Absolute amplitude threshold below which samples are considered silent.
     * @return New byte count with trailing silence removed.
     */
    static size_t TrimTrailingSilence(const uint8_t* pcmData, size_t byteCount, int16_t noiseFloorThreshold = 5);
};
