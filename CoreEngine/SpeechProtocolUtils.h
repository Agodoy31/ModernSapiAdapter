/**
 * @file SpeechProtocolUtils.h
 * @brief Pure Level-0 utility header for parsing provider speech control protocol events and predicates.
 */

#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#pragma warning(push)
#pragma warning(disable: 4996)
#include <sapi.h>
#pragma warning(pop)

#include <cstdint>
#include <string_view>
#include <nlohmann/json.hpp>

#include "SpeechEventTypes.h"
#include "SpeechWorkerTypes.h"
#include "JsonValue.h"

namespace SpeechProtocolUtils
{
    [[nodiscard]] constexpr ProviderEventType ParseProviderEventType(std::string_view name) noexcept
    {
        if (name == "word_boundary")
        {
            return ProviderEventType::WordBoundary;
        }
        if (name == "sentence_boundary")
        {
            return ProviderEventType::SentenceBoundary;
        }
        if (name == "bookmark_reached")
        {
            return ProviderEventType::Bookmark;
        }
        if (name == "synthesis_complete")
        {
            return ProviderEventType::SynthesisComplete;
        }
        if (name == "synthesis_cancelled")
        {
            return ProviderEventType::SynthesisCancelled;
        }
        if (name == "log")
        {
            return ProviderEventType::Log;
        }
        if (name == "completed")
        {
            return ProviderEventType::LegacyCompleted;
        }
        return ProviderEventType::Unknown;
    }

    [[nodiscard]] inline bool TryParseSpeechOffsets(
        const nlohmann::json& json,
        ProviderEventType type,
        SpeechEventOffsets& outOffsets) noexcept
    {
        if (!json.is_object())
        {
            return false;
        }

        if (!json.contains("audio_offset_ms") || !TryGetJsonUnsignedInteger(json["audio_offset_ms"], outOffsets.audioOffsetMs))
        {
            return false;
        }

        if (type == ProviderEventType::Bookmark)
        {
            return true;
        }

        if (!json.contains("text_offset") || !TryGetJsonUnsignedInteger(json["text_offset"], outOffsets.textOffset))
        {
            return false;
        }

        if (!json.contains("text_length") || !TryGetJsonUnsignedInteger(json["text_length"], outOffsets.textLength))
        {
            return false;
        }

        return true;
    }

    [[nodiscard]] inline bool TryParseTerminalBytes(
        const nlohmann::json& json,
        ProviderEventType type,
        uint64_t& outBytes) noexcept
    {
        outBytes = 0;
        if (!json.is_object())
        {
            return false;
        }

        if (type == ProviderEventType::SynthesisComplete)
        {
            if (json.contains("total_audio_bytes"))
            {
                return TryGetJsonUnsignedInteger(json["total_audio_bytes"], outBytes);
            }
            return false;
        }

        if (type == ProviderEventType::SynthesisCancelled)
        {
            if (json.contains("audio_bytes_written"))
            {
                return TryGetJsonUnsignedInteger(json["audio_bytes_written"], outBytes);
            }
            return false;
        }

        return false;
    }

    [[nodiscard]] constexpr bool HasSynthesisInactivityTimedOut(
        ULONGLONG now,
        ULONGLONG lastProgressTick,
        ULONGLONG timeoutMs) noexcept
    {
        return now >= lastProgressTick && (now - lastProgressTick) >= timeoutMs;
    }

    [[nodiscard]] constexpr bool HasCancellationTimedOut(
        ULONGLONG now,
        ULONGLONG deadlineTick) noexcept
    {
        return deadlineTick != 0 && now >= deadlineTick;
    }

    [[nodiscard]] constexpr bool IsAbortRequested(DWORD actionFlags) noexcept
    {
        return (actionFlags & SPVES_ABORT) != 0;
    }

    [[nodiscard]] inline ProviderControlEvent ParseControlEvent(const nlohmann::json& json) noexcept
    {
        ProviderControlEvent event{};
        if (!json.is_object())
        {
            return event;
        }

        try
        {
            if (json.contains("event") && json["event"].is_string())
            {
                event.rawEventName = json["event"].get_ref<const std::string&>();
                event.type = ParseProviderEventType(event.rawEventName);
            }

            if (json.contains("speak_id"))
            {
                TryGetJsonUnsignedInteger(json["speak_id"], event.speakId);
            }

            if (event.IsSpeechBoundary())
            {
                event.hasValidSpeechOffsets = TryParseSpeechOffsets(json, event.type, event.speechOffsets);
                if (event.type == ProviderEventType::Bookmark)
                {
                    if (json.contains("bookmark_name") && json["bookmark_name"].is_string())
                    {
                        event.bookmarkName = json["bookmark_name"].get_ref<const std::string&>();
                    }
                }
            }
            else if (event.IsTerminal())
            {
                event.hasValidTerminalBytes = TryParseTerminalBytes(json, event.type, event.terminalAudioBytes);
            }
            else if (event.type == ProviderEventType::Log)
            {
                if (json.contains("severity") && json["severity"].is_string())
                {
                    event.logSeverity = json["severity"].get_ref<const std::string&>();
                }
                if (json.contains("message") && json["message"].is_string())
                {
                    event.logMessage = json["message"].get_ref<const std::string&>();
                }
                if (json.contains("friendly_text") && json["friendly_text"].is_string())
                {
                    event.logFriendlyText = json["friendly_text"].get_ref<const std::string&>();
                }
            }
        }
        catch (...)
        {
            return ProviderControlEvent{};
        }

        return event;
    }
}
