/**
 * @file AudioFormatUtils.h
 * @brief Utilities for parsing and converting WAVEFORMATEX audio formats to/from JSON.
 */

#pragma once

#include <windows.h>
#include <mmreg.h>
#include <nlohmann/json.hpp>

namespace AudioFormatUtils
{
    /**
     * @brief Parses JSON audio format description into a WAVEFORMATEX structure.
     * @param formatJson JSON object containing audio format fields.
     * @param[out] format Destination WAVEFORMATEX structure.
     * @return True if parsing succeeded and format is valid PCM; false otherwise.
     */
    [[nodiscard]] bool TryParseAudioFormatJson(
        const nlohmann::json& formatJson,
        WAVEFORMATEX& format) noexcept;

    /**
     * @brief Serializes a WAVEFORMATEX structure to JSON.
     * @param format WAVEFORMATEX structure to serialize.
     * @return JSON representation of the audio format.
     */
    [[nodiscard]] nlohmann::json WaveFormatExToJson(
        const WAVEFORMATEX& format);
}
