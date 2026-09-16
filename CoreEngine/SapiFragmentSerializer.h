/**
 * @file SapiFragmentSerializer.h
 * @brief Stateless serializer converting SAPI 5 text fragments into JSON arrays.
 */

#pragma once

#include <nlohmann/json_fwd.hpp>

struct SPVTEXTFRAG;

namespace SapiFragmentSerializer
{
    /**
     * @brief Serializes a linked list of SAPI text fragments into a JSON array.
     *
     * Traverses the linked list of fragments starting at @p fragmentList and emits one
     * JSON value into the resulting array for each input fragment node:
     * - Supported fragments (e.g., speech text, non-empty bookmarks, silence) produce JSON objects.
     * - Unsupported actions and empty bookmarks intentionally produce JSON null entries.
     *
     * @param fragmentList Pointer to the head of the SPVTEXTFRAG linked list, or nullptr.
     * @return nlohmann::json JSON array containing one value per input fragment node. Returns
     *         an empty array if @p fragmentList is nullptr.
     *
     * @note Memory allocation (std::bad_alloc, nlohmann::json exceptions) and UTF conversion
     *       exceptions are intentionally not caught or contained here; they propagate directly
     *       to the immediate caller. In production, CSapiEngine::Speak serves as the exception
     *       boundary that catches and translates these failures.
     */
    [[nodiscard]] nlohmann::json Serialize(const SPVTEXTFRAG* fragmentList);
}
