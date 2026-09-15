/**
 * @file SapiFragmentSerializer.h
 * @brief Stateless serializer converting SAPI 5 text fragments into JSON arrays.
 */

#pragma once

#include <sapi.h>
#include <nlohmann/json.hpp>

namespace SapiFragmentSerializer
{
    [[nodiscard]] nlohmann::json Serialize(const SPVTEXTFRAG* fragmentList);
}
