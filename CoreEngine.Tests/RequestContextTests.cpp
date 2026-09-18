#include "pch.h"
#include "../CoreEngine/SpeechProtocolUtils.h"
#include "../CoreEngine/SpeechWorkerTypes.h"
#include <limits>
#include <vector>

using namespace SpeechProtocolUtils;

TEST(SpeechProtocolUtilsTests, TimeoutAndAbortPredicatesBehaveCorrectly)
{
    EXPECT_TRUE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 400ULL));
    EXPECT_TRUE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 500ULL));
    EXPECT_FALSE(HasSynthesisInactivityTimedOut(1000ULL, 500ULL, 600ULL));
    EXPECT_FALSE(HasSynthesisInactivityTimedOut(500ULL, 1000ULL, 100ULL));

    EXPECT_FALSE(HasCancellationTimedOut(1000ULL, 0ULL));
    EXPECT_TRUE(HasCancellationTimedOut(1000ULL, 1000ULL));
    EXPECT_TRUE(HasCancellationTimedOut(1001ULL, 1000ULL));
    EXPECT_FALSE(HasCancellationTimedOut(999ULL, 1000ULL));

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

    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Speaking;
    EXPECT_TRUE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    ctx.upstreamState = UpstreamState::Completed;
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_TRUE(ctx.IsAwaitingTerminalAudio());
    EXPECT_FALSE(ctx.IsDrainingCancellation());

    ctx.TransitionToCancelling(1234ULL);
    EXPECT_FALSE(ctx.IsActivelySynthesizing());
    EXPECT_FALSE(ctx.IsAwaitingTerminalAudio());
    EXPECT_TRUE(ctx.IsDrainingCancellation());
}

TEST(SpeechProtocolUtilsTests, RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum)
{
    RequestContext ctx{};
    ctx.token.speakId = 42;
    ctx.token.generation = 7;
    ctx.upstreamState = UpstreamState::Completed;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.rawAudioBytesRead = 1000;
    ctx.deliveredAudioBytes = 500;
    ctx.upstreamTerminalBytes = 2000;
    ctx.upstreamFinished = true;
    ctx.faultPending = false;
    ctx.cancellationDeadlineTick = 0;
    ctx.completionHr = S_OK;

    ctx.TransitionToCancelling(5000ULL);

    EXPECT_EQ(ctx.upstreamState, UpstreamState::Completed);
    EXPECT_EQ(ctx.downstreamState, DownstreamState::Cancelling);
    EXPECT_FALSE(ctx.upstreamFinished);
    EXPECT_EQ(ctx.upstreamTerminalBytes, 0ULL);
    EXPECT_EQ(ctx.cancellationDeadlineTick, 5000ULL);
    EXPECT_EQ(ctx.token.speakId, 42ULL);
    EXPECT_EQ(ctx.token.generation, 7ULL);
    EXPECT_EQ(ctx.rawAudioBytesRead, 1000ULL);
    EXPECT_EQ(ctx.deliveredAudioBytes, 500ULL);
    EXPECT_EQ(ctx.completionHr, S_OK);
    EXPECT_TRUE(ctx.IsDrainingCancellation());
}
