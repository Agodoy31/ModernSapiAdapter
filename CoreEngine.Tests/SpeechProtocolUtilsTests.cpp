#include "pch.h"
#include "../CoreEngine/SpeechProtocolUtils.h"
#include "../CoreEngine/SpeechWorkerTypes.h"
#include <limits>
#include <vector>

using namespace SpeechProtocolUtils;

TEST(SpeechProtocolUtilsTests, ParseProviderEventTypeMapsAllKnownStrings)
{
    EXPECT_EQ(ParseProviderEventType("word_boundary"), ProviderEventType::WordBoundary);
    EXPECT_EQ(ParseProviderEventType("sentence_boundary"), ProviderEventType::SentenceBoundary);
    EXPECT_EQ(ParseProviderEventType("bookmark_reached"), ProviderEventType::Bookmark);
    EXPECT_EQ(ParseProviderEventType("synthesis_complete"), ProviderEventType::SynthesisComplete);
    EXPECT_EQ(ParseProviderEventType("synthesis_cancelled"), ProviderEventType::SynthesisCancelled);
    EXPECT_EQ(ParseProviderEventType("log"), ProviderEventType::Log);
    EXPECT_EQ(ParseProviderEventType("completed"), ProviderEventType::LegacyCompleted);
    EXPECT_EQ(ParseProviderEventType("unknown_event"), ProviderEventType::Unknown);
    EXPECT_EQ(ParseProviderEventType(""), ProviderEventType::Unknown);
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_WordBoundaryWithValidOffsets)
{
    const nlohmann::json json = {
        {"event", "word_boundary"},
        {"speak_id", 42},
        {"audio_offset_ms", 150},
        {"text_offset", 5},
        {"text_length", 4}
    };

    const ProviderControlEvent event = ParseControlEvent(json);

    EXPECT_EQ(event.type, ProviderEventType::WordBoundary);
    EXPECT_EQ(event.rawEventName, "word_boundary");
    EXPECT_EQ(event.speakId, 42ULL);
    EXPECT_TRUE(event.hasValidSpeechOffsets);
    EXPECT_EQ(event.speechOffsets.audioOffsetMs, 150U);
    EXPECT_EQ(event.speechOffsets.textOffset, 5U);
    EXPECT_EQ(event.speechOffsets.textLength, 4U);
    EXPECT_TRUE(event.IsSpeechBoundary());
    EXPECT_FALSE(event.IsTerminal());
    EXPECT_TRUE(event.IsProgress());
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_SentenceBoundaryWithValidOffsets)
{
    const nlohmann::json json = {
        {"event", "sentence_boundary"},
        {"speak_id", 42},
        {"audio_offset_ms", 300},
        {"text_offset", 0},
        {"text_length", 25}
    };

    const ProviderControlEvent event = ParseControlEvent(json);

    EXPECT_EQ(event.type, ProviderEventType::SentenceBoundary);
    EXPECT_EQ(event.rawEventName, "sentence_boundary");
    EXPECT_EQ(event.speakId, 42ULL);
    EXPECT_TRUE(event.hasValidSpeechOffsets);
    EXPECT_EQ(event.speechOffsets.audioOffsetMs, 300U);
    EXPECT_EQ(event.speechOffsets.textOffset, 0U);
    EXPECT_EQ(event.speechOffsets.textLength, 25U);
    EXPECT_TRUE(event.IsSpeechBoundary());
    EXPECT_FALSE(event.IsTerminal());
    EXPECT_TRUE(event.IsProgress());
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_BookmarkWithValidOffsets)
{
    const nlohmann::json json = {
        {"event", "bookmark_reached"},
        {"speak_id", 42},
        {"audio_offset_ms", 250},
        {"bookmark", "mark1"}
    };

    const ProviderControlEvent event = ParseControlEvent(json);

    EXPECT_EQ(event.type, ProviderEventType::Bookmark);
    EXPECT_EQ(event.rawEventName, "bookmark_reached");
    EXPECT_EQ(event.speakId, 42ULL);
    EXPECT_TRUE(event.hasValidSpeechOffsets);
    EXPECT_EQ(event.speechOffsets.audioOffsetMs, 250U);
    EXPECT_TRUE(event.IsSpeechBoundary());
    EXPECT_FALSE(event.IsTerminal());
    EXPECT_TRUE(event.IsProgress());
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_SynthesisComplete_ValidIntegerAndFloat)
{
    const nlohmann::json intJson = {
        {"event", "synthesis_complete"},
        {"speak_id", 100},
        {"total_audio_bytes", 4096}
    };

    const ProviderControlEvent intEvent = ParseControlEvent(intJson);

    EXPECT_EQ(intEvent.type, ProviderEventType::SynthesisComplete);
    EXPECT_EQ(intEvent.rawEventName, "synthesis_complete");
    EXPECT_EQ(intEvent.speakId, 100ULL);
    EXPECT_TRUE(intEvent.hasValidTerminalBytes);
    EXPECT_EQ(intEvent.terminalAudioBytes, 4096ULL);
    EXPECT_TRUE(intEvent.IsTerminal());
    EXPECT_FALSE(intEvent.IsSpeechBoundary());
    EXPECT_TRUE(intEvent.IsProgress());

    const nlohmann::json floatJson = {
        {"event", "synthesis_complete"},
        {"speak_id", 100},
        {"total_audio_bytes", 8192.0}
    };

    const ProviderControlEvent floatEvent = ParseControlEvent(floatJson);

    EXPECT_EQ(floatEvent.type, ProviderEventType::SynthesisComplete);
    EXPECT_TRUE(floatEvent.hasValidTerminalBytes);
    EXPECT_EQ(floatEvent.terminalAudioBytes, 8192ULL);
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_SynthesisCancelled_ValidBytes)
{
    const nlohmann::json json = {
        {"event", "synthesis_cancelled"},
        {"speak_id", 101},
        {"audio_bytes_written", 2048}
    };

    const ProviderControlEvent event = ParseControlEvent(json);

    EXPECT_EQ(event.type, ProviderEventType::SynthesisCancelled);
    EXPECT_EQ(event.rawEventName, "synthesis_cancelled");
    EXPECT_EQ(event.speakId, 101ULL);
    EXPECT_TRUE(event.hasValidTerminalBytes);
    EXPECT_EQ(event.terminalAudioBytes, 2048ULL);
    EXPECT_TRUE(event.IsTerminal());
    EXPECT_FALSE(event.IsSpeechBoundary());
    EXPECT_TRUE(event.IsProgress());
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_LogEventWithSeverityAndMessage)
{
    const nlohmann::json json = {
        {"event", "log"},
        {"speak_id", 10},
        {"severity", "warning"},
        {"message", "Low buffer warning"}
    };

    const ProviderControlEvent event = ParseControlEvent(json);

    EXPECT_EQ(event.type, ProviderEventType::Log);
    EXPECT_EQ(event.rawEventName, "log");
    EXPECT_EQ(event.speakId, 10ULL);
    EXPECT_EQ(event.logSeverity, "warning");
    EXPECT_EQ(event.logMessage, "Low buffer warning");
    EXPECT_TRUE(event.IsProgress());
    EXPECT_FALSE(event.IsSpeechBoundary());
    EXPECT_FALSE(event.IsTerminal());
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_MalformedOrMissingFieldsHandledSafely)
{
    // Missing event
    {
        const nlohmann::json json = {{"speak_id", 42}};
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_EQ(event.type, ProviderEventType::Unknown);
        EXPECT_TRUE(event.rawEventName.empty());
        EXPECT_EQ(event.speakId, 42ULL);
    }

    // Non-string event
    {
        const nlohmann::json json = {{"event", 123}, {"speak_id", 42}};
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_EQ(event.type, ProviderEventType::Unknown);
    }

    // Missing speak_id
    {
        const nlohmann::json json = {{"event", "word_boundary"}};
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_EQ(event.type, ProviderEventType::WordBoundary);
        EXPECT_EQ(event.speakId, 0ULL);
    }

    // Invalid negative speak_id
    {
        const nlohmann::json json = {{"event", "word_boundary"}, {"speak_id", -5}};
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_EQ(event.speakId, 0ULL);
    }

    // Word boundary missing audio_offset_ms
    {
        const nlohmann::json json = {
            {"event", "word_boundary"},
            {"speak_id", 1},
            {"text_offset", 0},
            {"text_length", 5}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidSpeechOffsets);
    }

    // Word boundary missing text_offset
    {
        const nlohmann::json json = {
            {"event", "word_boundary"},
            {"speak_id", 1},
            {"audio_offset_ms", 100},
            {"text_length", 5}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidSpeechOffsets);
    }

    // Word boundary missing text_length
    {
        const nlohmann::json json = {
            {"event", "word_boundary"},
            {"speak_id", 1},
            {"audio_offset_ms", 100},
            {"text_offset", 0}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidSpeechOffsets);
    }

    // SynthesisComplete missing total_audio_bytes
    {
        const nlohmann::json json = {
            {"event", "synthesis_complete"},
            {"speak_id", 1}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidTerminalBytes);
    }

    // SynthesisCancelled missing audio_bytes_written
    {
        const nlohmann::json json = {
            {"event", "synthesis_cancelled"},
            {"speak_id", 1}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidTerminalBytes);
    }

    // Bookmark missing audio_offset_ms
    {
        const nlohmann::json json = {
            {"event", "bookmark_reached"},
            {"speak_id", 1}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_FALSE(event.hasValidSpeechOffsets);
    }

    // Log missing severity/message
    {
        const nlohmann::json json = {
            {"event", "log"},
            {"speak_id", 1}
        };
        const ProviderControlEvent event = ParseControlEvent(json);
        EXPECT_EQ(event.type, ProviderEventType::Log);
        EXPECT_TRUE(event.logSeverity.empty());
        EXPECT_TRUE(event.logMessage.empty());
    }
}

TEST(SpeechProtocolUtilsTests, ParseControlEvent_NonObjectJsonReturnsUnknown)
{
    const std::vector<nlohmann::json> nonObjects = {
        nlohmann::json::array(),
        nlohmann::json("just a string"),
        nlohmann::json(nullptr),
        nlohmann::json(123),
        nlohmann::json(true)
    };

    for (const auto& nonObj : nonObjects)
    {
        const ProviderControlEvent event = ParseControlEvent(nonObj);
        EXPECT_EQ(event.type, ProviderEventType::Unknown);
        EXPECT_EQ(event.speakId, 0ULL);
        EXPECT_FALSE(event.hasValidSpeechOffsets);
        EXPECT_FALSE(event.hasValidTerminalBytes);
        EXPECT_TRUE(event.rawEventName.empty());
    }
}

TEST(SpeechProtocolUtilsTests, TimeoutAndAbortPredicatesBehaveCorrectly)
{
    // HasSynthesisInactivityTimedOut
    EXPECT_TRUE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 400ULL));
    EXPECT_TRUE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 500ULL));
    EXPECT_FALSE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 600ULL));
    EXPECT_FALSE(HasSynthesisInactivityTimedOut(500ULL, 1000ULL, 100ULL));

    // HasCancellationTimedOut
    EXPECT_FALSE(HasCancellationTimedOut(1000ULL, 0ULL));
    EXPECT_TRUE(HasCancellationTimedOut(1000ULL, 1000ULL));
    EXPECT_TRUE(HasCancellationTimedOut(1001ULL, 1000ULL));
    EXPECT_FALSE(HasCancellationTimedOut(999ULL, 1000ULL));

    // IsAbortRequested
    EXPECT_TRUE(IsAbortRequested(SPVES_ABORT));
    EXPECT_TRUE(IsAbortRequested(SPVES_ABORT | 0x02));
    EXPECT_FALSE(IsAbortRequested(0));
    EXPECT_FALSE(IsAbortRequested(0x02));
}

TEST(SpeechProtocolUtilsTests, RequestContext_TransitionToCancellingResetsFlagsAndSetsDeadline)
{
    RequestContext ctx{};
    ctx.upstreamFinished = true;
    ctx.upstreamTerminalBytes = 4096;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.cancellationDeadlineTick = 0;

    ctx.TransitionToCancelling(5000ULL);

    EXPECT_FALSE(ctx.upstreamFinished);
    EXPECT_EQ(ctx.upstreamTerminalBytes, 0ULL);
    EXPECT_EQ(ctx.cancellationDeadlineTick, 5000ULL);
    EXPECT_EQ(ctx.downstreamState, DownstreamState::Cancelling);
    EXPECT_TRUE(ctx.IsDrainingCancellation());
}

TEST(SpeechProtocolUtilsTests, RequestContext_SemanticPredicatesReflectDiscreteStates)
{
    RequestContext ctx{};

    // Idle initial state
    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    // Active synthesis
    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Speaking;
    EXPECT_TRUE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    // Awaiting terminal audio (upstream completed/cancelled, downstream still delivering)
    ctx.upstreamState = UpstreamState::Completed;
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_TRUE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    // Draining cancellation
    ctx.TransitionToCancelling(1234ULL);
    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_TRUE(ctx.IsDrainingCancellation());
}

