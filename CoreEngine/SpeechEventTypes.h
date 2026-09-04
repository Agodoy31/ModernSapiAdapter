/**
 * @file SpeechEventTypes.h
 * @brief Pure Level-0 domain leaf header for provider speech event types.
 */

#pragma once

#include <cstdint>
#include <string_view>

enum class ProviderEventType : uint8_t
{
    WordBoundary = 0,
    SentenceBoundary = 1,
    Bookmark = 2,
    SynthesisComplete = 3,
    SynthesisCancelled = 4,
    Log = 5,
    LegacyCompleted = 6,
    Unknown = 7
};

struct SpeechEventOffsets
{
    uint32_t audioOffsetMs = 0;
    uint32_t textOffset = 0;
    uint32_t textLength = 0;
};

struct ProviderControlEvent
{
    ProviderEventType type = ProviderEventType::Unknown;
    uint64_t speakId = 0;
    SpeechEventOffsets speechOffsets{};
    uint64_t terminalAudioBytes = 0;
    bool hasValidSpeechOffsets = false;
    bool hasValidTerminalBytes = false;
    std::string_view rawEventName;
    std::string_view logSeverity;
    std::string_view logMessage;

    [[nodiscard]] constexpr bool IsSpeechBoundary() const noexcept
    {
        return type == ProviderEventType::WordBoundary ||
               type == ProviderEventType::SentenceBoundary ||
               type == ProviderEventType::Bookmark;
    }

    [[nodiscard]] constexpr bool IsTerminal() const noexcept
    {
        return type == ProviderEventType::SynthesisComplete ||
               type == ProviderEventType::SynthesisCancelled;
    }

    [[nodiscard]] constexpr bool IsProgress() const noexcept
    {
        return IsSpeechBoundary() || IsTerminal();
    }
};
