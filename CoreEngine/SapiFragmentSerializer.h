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
     * Traverses the linked list of fragments starting at @p fragmentList and constructs
     * a JSON array containing the structured representation of each fragment action
     * (e.g., speech text, bookmark, or silence).
     *
     * @param fragmentList Pointer to the head of the SPVTEXTFRAG linked list, or nullptr.
     * @return nlohmann::json JSON array of serialized fragment objects. Returns an empty
     *         array if @p fragmentList is nullptr.
     *
     * @note Memory allocation (std::bad_alloc, nlohmann::json exceptions) and UTF conversion
     *       exceptions are intentionally not caught or contained here; they are expected to
     *       propagate directly to the caller's Speak exception boundary.
     */
    [[nodiscard]] nlohmann::json Serialize(const SPVTEXTFRAG* fragmentList);
}
