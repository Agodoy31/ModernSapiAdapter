/**
 * @file SapiSsmlParser.h
 * @brief Public interface for translating SAPI 5 speech fragments into W3C SSML.
 */

#ifndef SAPI_SSML_PARSER_H
#define SAPI_SSML_PARSER_H
#pragma once

/**
 * @brief Result of parsing SAPI speech fragments into an SSML document.
 */
struct SsmlParseResult {
    std::u16string SsmlString;
    std::vector<std::pair<uint32_t, uint32_t>> OffsetMap; // Pair of <SSML Char Index, Original Offset>
};

/**
 * @brief Parses SAPI speech fragments and builds an SSML document according to the blueprint.
 */
class SapiSsmlParser {
public:
    static SsmlParseResult Parse(const ProviderSpeechFragment* fragments, uint32_t count, const std::wstring& localeName);
};

#endif // SAPI_SSML_PARSER_H
