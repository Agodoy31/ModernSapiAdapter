#include "pch.h"
#include <gtest/gtest.h>
#include "../CoreEngine/SpeechStatePolicy.h"

using namespace SpeechStatePolicy;

class SpeechStatePolicyTests : public ::testing::Test
{
protected:
    RequestContext CreateIdleContext()
    {
        RequestContext ctx;
        ctx.Reset();
        return ctx;
    }
};

TEST_F(SpeechStatePolicyTests, StartAcceptsOnlyQuiescentNonFaultPendingState)
{
    RequestContext ctx = CreateIdleContext();
    auto dec = EvaluateStart(ctx, 42, 1);
    EXPECT_EQ(dec.action, StartAction::Accept);
    EXPECT_EQ(dec.speakId, 42ULL);
    EXPECT_EQ(dec.generation, 1ULL);

    ctx.faultPending = true;
    dec = EvaluateStart(ctx, 42, 1);
    EXPECT_EQ(dec.action, StartAction::Reject);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.generation, 0ULL);

    ctx.faultPending = false;
    ctx.upstreamState = UpstreamState::Active;
    dec = EvaluateStart(ctx, 42, 1);
    EXPECT_EQ(dec.action, StartAction::Reject);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.generation, 0ULL);

    ctx.upstreamState = UpstreamState::Idle;
    ctx.downstreamState = DownstreamState::Speaking;
    dec = EvaluateStart(ctx, 42, 1);
    EXPECT_EQ(dec.action, StartAction::Reject);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.generation, 0ULL);

    ctx.downstreamState = DownstreamState::Cancelling;
    dec = EvaluateStart(ctx, 42, 1);
    EXPECT_EQ(dec.action, StartAction::Reject);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.generation, 0ULL);
}

TEST_F(SpeechStatePolicyTests, StartDecisionOwnsAcceptedIdentityWithoutMutatingInput)
{
    RequestContext ctx = CreateIdleContext();
    RequestContext original = ctx;
    auto dec = EvaluateStart(ctx, 42, 5);
    EXPECT_EQ(dec.action, StartAction::Accept);
    EXPECT_EQ(dec.speakId, 42ULL);
    EXPECT_EQ(dec.generation, 5ULL);
    EXPECT_EQ(ctx.upstreamState, original.upstreamState);
}

TEST_F(SpeechStatePolicyTests, UpstreamCompleteAppliesValidatedTerminalFact)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 100, true, true);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::Apply);
    EXPECT_EQ(dec.targetState, UpstreamState::Completed);
    EXPECT_EQ(dec.terminalAudioBytes, 100ULL);
}

TEST_F(SpeechStatePolicyTests, UpstreamCancelledAppliesValidatedTerminalFact)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Cancelled, 50, true, true);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::Apply);
    EXPECT_EQ(dec.targetState, UpstreamState::Cancelled);
    EXPECT_EQ(dec.terminalAudioBytes, 50ULL);
}

TEST_F(SpeechStatePolicyTests, DuplicateUpstreamTerminalRequestsTwoStageFaultPath)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 100, true, true);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::DuplicateFault);
    EXPECT_EQ(dec.targetState, UpstreamState::Faulted);
    EXPECT_EQ(dec.terminalAudioBytes, 0ULL);
}

TEST_F(SpeechStatePolicyTests, InvalidUpstreamTerminalBytesRequestFault)
{
    RequestContext ctx = CreateIdleContext();
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 100, false, true);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::InvalidBytesFault);
    EXPECT_EQ(dec.targetState, UpstreamState::Faulted);
    EXPECT_EQ(dec.terminalAudioBytes, 0ULL);
}

TEST_F(SpeechStatePolicyTests, MisalignedUpstreamTerminalBytesRequestFault)
{
    RequestContext ctx = CreateIdleContext();
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 101, true, false);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::MisalignedBytesFault);
    EXPECT_EQ(dec.targetState, UpstreamState::Faulted);
    EXPECT_EQ(dec.terminalAudioBytes, 0ULL);
}

TEST_F(SpeechStatePolicyTests, DuplicateMalformedTerminalKeepsDuplicateFirstPrecedence)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    auto dec = EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 101, false, false);
    EXPECT_EQ(dec.action, UpstreamTerminalAction::DuplicateFault);
    EXPECT_EQ(dec.targetState, UpstreamState::Faulted);
    EXPECT_EQ(dec.terminalAudioBytes, 0ULL);
}

TEST_F(SpeechStatePolicyTests, UtteranceFailureCapturesCurrentRawByteBoundary)
{
    RequestContext ctx = CreateIdleContext();
    ctx.rawAudioBytesRead = 200;
    auto dec = EvaluateUtteranceFailure(ctx);
    EXPECT_EQ(dec.action, RequestFailureAction::ApplyUtteranceFailure);
    EXPECT_EQ(dec.terminalAudioBytes, 200ULL);
}

TEST_F(SpeechStatePolicyTests, FailedUpstreamTakesBoundaryPrecedence)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Failed;
    TerminalBoundaryFacts facts{true, true, true};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::UtteranceFailedReset);
}

TEST_F(SpeechStatePolicyTests, UnfinishedUpstreamContinuesDespiteRetainedCompletedEnum)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Completed;
    ctx.upstreamFinished = false; // Important: retained enum but not finished
    TerminalBoundaryFacts facts{false, true, false};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::Continue);
}

TEST_F(SpeechStatePolicyTests, UnfinishedUpstreamContinuesDespiteRetainedCancelledEnum)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Cancelled;
    ctx.upstreamFinished = false;
    TerminalBoundaryFacts facts{false, false, true};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::Continue);
}

TEST_F(SpeechStatePolicyTests, SpeakingReachedResetsOnlySpeakingLifecycle)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    TerminalBoundaryFacts facts{false, true, false};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::NormalCompleteReset);
}

TEST_F(SpeechStatePolicyTests, CancellationReachedResetsOnlyCancellingLifecycle)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Cancelling;
    TerminalBoundaryFacts facts{false, false, true};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::CancelDrainReset);
}

TEST_F(SpeechStatePolicyTests, SpeakingOverrunRequestsFault)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    TerminalBoundaryFacts facts{true, false, false};
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::OverrunFault);
}

TEST_F(SpeechStatePolicyTests, IrrelevantBoundaryFactsAreIgnoredForCurrentState)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    TerminalBoundaryFacts facts{false, false, true}; // cancellation reached but state is speaking
    auto dec = EvaluateTerminalBoundary(ctx, facts);
    EXPECT_EQ(dec.action, TerminalBoundaryAction::Continue);
}

TEST_F(SpeechStatePolicyTests, CancellationFromActiveSpeakingTransitions)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.token.speakId = 42;
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::TransitionToCancelling);
    EXPECT_EQ(dec.speakId, 42ULL);
    EXPECT_EQ(dec.deadlineTick, 1000ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationFromCompletedSpeakingRetainsCompletedEnumContract)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Completed;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.token.speakId = 42;
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::TransitionToCancelling);
    EXPECT_EQ(dec.speakId, 42ULL);
    EXPECT_EQ(dec.deadlineTick, 1000ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationFromCancelledSpeakingRetainsCancelledEnumContract)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Cancelled;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.token.speakId = 42;
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::TransitionToCancelling);
    EXPECT_EQ(dec.speakId, 42ULL);
    EXPECT_EQ(dec.deadlineTick, 1000ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationAlreadyIdleMapsToAlreadyIdle)
{
    RequestContext ctx = CreateIdleContext();
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::AlreadyIdle);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.deadlineTick, 0ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationAlreadyDrainingMapsToAlreadyCancelling)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Cancelling;
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::AlreadyCancelling);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.deadlineTick, 0ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationFaultedMapsToFaulted)
{
    RequestContext ctx = CreateIdleContext();
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.upstreamState = UpstreamState::Faulted;
    auto dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::Faulted);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.deadlineTick, 0ULL);

    ctx.downstreamState = DownstreamState::Faulted;
    ctx.upstreamState = UpstreamState::Active;
    dec = EvaluateBeginCancellation(ctx, 1000);
    EXPECT_EQ(dec.action, BeginCancellationAction::Faulted);
    EXPECT_EQ(dec.speakId, 0ULL);
    EXPECT_EQ(dec.deadlineTick, 0ULL);
}

TEST_F(SpeechStatePolicyTests, StopActiveRequestCapturesSpeakId)
{
    RequestContext ctx = CreateIdleContext();
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.upstreamState = UpstreamState::Active;
    ctx.token.speakId = 42;
    auto dec = EvaluateStop(ctx);
    EXPECT_EQ(dec.action, StopAction::ResetAndCancel);
    EXPECT_EQ(dec.speakId, 42ULL);
}

TEST_F(SpeechStatePolicyTests, StopIdleDoesNothing)
{
    RequestContext ctx = CreateIdleContext();
    auto dec = EvaluateStop(ctx);
    EXPECT_EQ(dec.action, StopAction::NoAction);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, StopFaultedDoesNothing)
{
    RequestContext ctx = CreateIdleContext();
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.upstreamState = UpstreamState::Faulted;
    auto dec = EvaluateStop(ctx);
    EXPECT_EQ(dec.action, StopAction::NoAction);
    EXPECT_EQ(dec.speakId, 0ULL);

    ctx.downstreamState = DownstreamState::Faulted;
    ctx.upstreamState = UpstreamState::Active;
    dec = EvaluateStop(ctx);
    EXPECT_EQ(dec.action, StopAction::NoAction);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationTimeoutPrecedesInactivityTimeout)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Cancelling;
    ctx.cancellationDeadlineTick = 1000;
    ctx.token.speakId = 42;
    auto dec = EvaluateTimeouts(ctx, 1000, 0, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::CancellationTimeout);
    EXPECT_EQ(dec.speakId, 42ULL);
}

TEST_F(SpeechStatePolicyTests, CancellationDeadlineRequiresNonzeroExpiredTick)
{
    RequestContext ctx = CreateIdleContext();
    ctx.downstreamState = DownstreamState::Cancelling;
    ctx.cancellationDeadlineTick = 0;
    auto dec = EvaluateTimeouts(ctx, 1000, 0, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::None);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, ExpiredRetainedCancellationDeadlineWhileIdleDoesNotTimeout)
{
    RequestContext ctx = CreateIdleContext();
    ctx.cancellationDeadlineTick = 100;
    auto dec = EvaluateTimeouts(ctx, 1000, 0, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::None);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, ActiveSynthesisInactivityExpires)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    ctx.token.speakId = 42;
    auto dec = EvaluateTimeouts(ctx, 1000, 500, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::InactivityTimeout);
    EXPECT_EQ(dec.speakId, 42ULL);
}

TEST_F(SpeechStatePolicyTests, TerminalAudioInactivityExpires)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamFinished = true;
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.token.speakId = 42;
    auto dec = EvaluateTimeouts(ctx, 1000, 500, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::InactivityTimeout);
    EXPECT_EQ(dec.speakId, 42ULL);
}

TEST_F(SpeechStatePolicyTests, ClockRegressionDoesNotExpireInactivity)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    auto dec = EvaluateTimeouts(ctx, 500, 1000, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::None);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, NoEligibleTimeoutReturnsNone)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    auto dec = EvaluateTimeouts(ctx, 900, 500, 500);
    EXPECT_EQ(dec.condition, TimeoutCondition::None);
    EXPECT_EQ(dec.speakId, 0ULL);
}

TEST_F(SpeechStatePolicyTests, WaitTerminalRecognizesIdleFaultAndExit)
{
    RequestContext ctx = CreateIdleContext();
    EXPECT_TRUE(IsWaitTerminal(ctx, false));
    EXPECT_TRUE(IsWaitTerminal(ctx, true));

    ctx.downstreamState = DownstreamState::Speaking;
    EXPECT_FALSE(IsWaitTerminal(ctx, false));
    EXPECT_TRUE(IsWaitTerminal(ctx, true));

    ctx.downstreamState = DownstreamState::Faulted;
    EXPECT_TRUE(IsWaitTerminal(ctx, false));
    EXPECT_TRUE(IsWaitTerminal(ctx, true));

    ctx.downstreamState = DownstreamState::Cancelling;
    EXPECT_FALSE(IsWaitTerminal(ctx, false));
    EXPECT_TRUE(IsWaitTerminal(ctx, true));
}

TEST_F(SpeechStatePolicyTests, PolicyDecisionsDoNotMutateInputContext)
{
    RequestContext ctx = CreateIdleContext();
    ctx.upstreamState = UpstreamState::Active;
    RequestContext original = ctx;

    (void)EvaluateStart(ctx, 1, 1);
    (void)EvaluateUpstreamTerminal(ctx, UpstreamTerminalKind::Completed, 100, true, true);
    (void)EvaluateUtteranceFailure(ctx);
    (void)EvaluateTerminalBoundary(ctx, TerminalBoundaryFacts{});
    (void)EvaluateBeginCancellation(ctx, 100);
    (void)EvaluateStop(ctx);
    (void)EvaluateTimeouts(ctx, 100, 100, 100);
    (void)IsWaitTerminal(ctx, false);

    EXPECT_EQ(ctx.upstreamState, original.upstreamState);
    EXPECT_EQ(ctx.downstreamState, original.downstreamState);
}
