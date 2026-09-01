#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <mmreg.h>

#include <string>
#include <cstring>

/**
 * @struct ProviderSessionConfig
 * @brief Encapsulates provider endpoint metadata, voice ID, and negotiated audio format.
 */
struct ProviderSessionConfig
{
    std::wstring executablePath;
    std::wstring pipeName;
    std::wstring voiceId;
    WAVEFORMATEX audioFormat{};
    bool hasOutputFormat{false};

    [[nodiscard]] bool IsConfigured() const noexcept
    {
        return !pipeName.empty();
    }

    [[nodiscard]] bool IsFormatCompatible(const WAVEFORMATEX& candidate) const noexcept
    {
        if (!hasOutputFormat)
        {
            return true;
        }

        return candidate.wFormatTag == audioFormat.wFormatTag &&
               candidate.nChannels == audioFormat.nChannels &&
               candidate.nSamplesPerSec == audioFormat.nSamplesPerSec &&
               candidate.nAvgBytesPerSec == audioFormat.nAvgBytesPerSec &&
               candidate.nBlockAlign == audioFormat.nBlockAlign &&
               candidate.wBitsPerSample == audioFormat.wBitsPerSample &&
               candidate.cbSize == audioFormat.cbSize;
    }

    void Reset() noexcept
    {
        executablePath.clear();
        pipeName.clear();
        voiceId.clear();
        audioFormat = WAVEFORMATEX{};
        hasOutputFormat = false;
    }
};
