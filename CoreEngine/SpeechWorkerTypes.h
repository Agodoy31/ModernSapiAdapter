/**
 * @file SpeechWorkerTypes.h
 * @brief Pure Level-0 domain leaf header for speech synthesis worker state and event types.
 */

#pragma once

#include <cstdint>
#include <string_view>
#include <unknwn.h>

enum class UpstreamState : uint8_t
{
    Idle = 0,
    Active = 1,       // Request dispatched; provider synthesizing
    Completed = 2,    // Provider sent synthesis_complete with total_audio_bytes
    Cancelled = 3,    // Provider sent synthesis_cancelled with audio_bytes_written
    Failed = 4,       // Provider sent severity="error" for this speak_id
    Faulted = 5       // Fatal provider crash or broken pipe
};

enum class DownstreamState : uint8_t
{
    Idle = 0,
    Speaking = 1,     // Delivering audio frames to SAPI pOutputSite->Write()
    Cancelling = 2,   // SAPI requested SPVES_ABORT; draining pipe without Write()
    Faulted = 3       // Fatal engine fault
};

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

enum class SapiSpeechEventType : uint8_t
{
    WordBoundary = 0,
    SentenceBoundary = 1,
    BookmarkReached = 2,
    Log = 3,
    Unknown = 4
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
        return IsSpeechBoundary() || IsTerminal() || type == ProviderEventType::Log;
    }
};

struct RequestToken
{
    uint64_t speakId = 0;
    uint64_t generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return speakId != 0 && generation != 0;
    }

    [[nodiscard]] constexpr bool Matches(const RequestToken& other) const noexcept
    {
        return speakId == other.speakId && generation == other.generation;
    }
};

struct RequestContext
{
    RequestToken token{};
    UpstreamState upstreamState = UpstreamState::Idle;
    DownstreamState downstreamState = DownstreamState::Idle;
    uint64_t rawAudioBytesRead = 0;
    uint64_t deliveredAudioBytes = 0;
    uint64_t upstreamTerminalBytes = 0;
    bool upstreamFinished = false;
    bool faultPending = false;
    ULONGLONG cancellationDeadlineTick = 0;
    HRESULT completionHr = S_OK;

    void Reset() noexcept
    {
        token.speakId = 0;
        upstreamState = UpstreamState::Idle;
        downstreamState = DownstreamState::Idle;
        rawAudioBytesRead = 0;
        deliveredAudioBytes = 0;
        upstreamTerminalBytes = 0;
        upstreamFinished = false;
        faultPending = false;
        cancellationDeadlineTick = 0;
        completionHr = S_OK;
    }

    void TransitionToCancelling(ULONGLONG deadlineTick) noexcept
    {
        upstreamFinished = false;
        upstreamTerminalBytes = 0;
        cancellationDeadlineTick = deadlineTick;
        downstreamState = DownstreamState::Cancelling;
    }

    [[nodiscard]] constexpr bool IsAwaitingTerminalAudio() const noexcept
    {
        return upstreamFinished && downstreamState == DownstreamState::Speaking;
    }

    [[nodiscard]] constexpr bool IsActivelySynthesizing() const noexcept
    {
        return upstreamState == UpstreamState::Active;
    }

    [[nodiscard]] constexpr bool IsDrainingCancellation() const noexcept
    {
        return downstreamState == DownstreamState::Cancelling;
    }
};
