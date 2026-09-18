#include "pch.h"
#include <gtest/gtest.h>
#include "../CoreEngine/PcmDeliveryPolicy.h"

using namespace PcmDeliveryPolicy;

namespace
{
constexpr uint64_t kTestSpeakId = 101;
constexpr uint64_t kTestGeneration = 1;

RequestContext CreateHealthySpeakingContext(uint64_t speakId = kTestSpeakId)
{
    RequestContext ctx;
    ctx.Reset();
    ctx.token.speakId = speakId;
    ctx.token.generation = kTestGeneration;
    ctx.upstreamState = UpstreamState::Active;
    ctx.downstreamState = DownstreamState::Speaking;
    return ctx;
}
} // namespace

// 1. SpeakingChunkFramesFullChunkAndRequestsAccounting
TEST(PcmDeliveryPolicyTests, SpeakingChunkFramesFullChunkAndRequestsAccounting)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.upstreamFinished = false;
    ctx.rawAudioBytesRead = 100;

    const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

    EXPECT_EQ(dec.action, ChunkAction::AssembleSpeakingAudio);
    EXPECT_EQ(dec.bytesToFrame, 500u);
    EXPECT_TRUE(dec.refreshProgress);
    EXPECT_TRUE(dec.countRawBytes);
}

// 2. KnownSpeakingTerminalClipsFramingAndCountsRawChunk
TEST(PcmDeliveryPolicyTests, KnownSpeakingTerminalClipsFramingAndCountsRawChunk)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.upstreamFinished = true;
    ctx.upstreamTerminalBytes = 1000;
    ctx.rawAudioBytesRead = 800; // 200 remaining declared

    const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

    EXPECT_EQ(dec.action, ChunkAction::AssembleSpeakingAudio);
    EXPECT_EQ(dec.bytesToFrame, 200u);
    EXPECT_TRUE(dec.refreshProgress);
    EXPECT_TRUE(dec.countRawBytes);
}

// 3. SpeakingAtOrPastTerminalFramesZeroBytesWithoutUnderflow
TEST(PcmDeliveryPolicyTests, SpeakingAtOrPastTerminalFramesZeroBytesWithoutUnderflow)
{
    // Exact at terminal boundary
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamFinished = true;
        ctx.upstreamTerminalBytes = 1000;
        ctx.rawAudioBytesRead = 1000;

        const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

        EXPECT_EQ(dec.action, ChunkAction::AssembleSpeakingAudio);
        EXPECT_EQ(dec.bytesToFrame, 0u);
        EXPECT_TRUE(dec.refreshProgress);
        EXPECT_TRUE(dec.countRawBytes);
    }

    // Past terminal boundary (overrun condition)
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamFinished = true;
        ctx.upstreamTerminalBytes = 1000;
        ctx.rawAudioBytesRead = 1200;

        const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

        EXPECT_EQ(dec.action, ChunkAction::AssembleSpeakingAudio);
        EXPECT_EQ(dec.bytesToFrame, 0u);
        EXPECT_TRUE(dec.refreshProgress);
        EXPECT_TRUE(dec.countRawBytes);
    }
}

// 4. CancellingChunkDrainsWithProgressAndRawAccounting
TEST(PcmDeliveryPolicyTests, CancellingChunkDrainsWithProgressAndRawAccounting)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.downstreamState = DownstreamState::Cancelling;
    ctx.rawAudioBytesRead = 400;

    const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

    EXPECT_EQ(dec.action, ChunkAction::DrainCancellationAudio);
    EXPECT_EQ(dec.bytesToFrame, 0u);
    EXPECT_TRUE(dec.refreshProgress);
    EXPECT_TRUE(dec.countRawBytes);
}

// 5. IdleAndFaultedChunkActionsRemainDistinctAndEffectFree
TEST(PcmDeliveryPolicyTests, IdleAndFaultedChunkActionsRemainDistinctAndEffectFree)
{
    // Idle chunk ingest
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.downstreamState = DownstreamState::Idle;

        const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

        EXPECT_EQ(dec.action, ChunkAction::FaultUnexpectedIdleAudio);
        EXPECT_EQ(dec.bytesToFrame, 0u);
        EXPECT_FALSE(dec.refreshProgress);
        EXPECT_FALSE(dec.countRawBytes);
    }

    // Faulted chunk ingest
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.downstreamState = DownstreamState::Faulted;

        const ChunkDecision dec = EvaluateChunkIngest(ctx, 500);

        EXPECT_EQ(dec.action, ChunkAction::DrainFaultedSessionAudio);
        EXPECT_EQ(dec.bytesToFrame, 0u);
        EXPECT_FALSE(dec.refreshProgress);
        EXPECT_FALSE(dec.countRawBytes);
    }
}

// 6. MatchingSpeakingTokenAdmitsSpan
TEST(PcmDeliveryPolicyTests, MatchingSpeakingTokenAdmitsSpan)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    const RequestToken batchToken = ctx.token;

    const SpanAdmission adm = EvaluateSpanAdmission(ctx, batchToken);

    EXPECT_EQ(adm, SpanAdmission::Allow);
}

// 7. StaleTokenWrongGenerationInactiveStateAndPendingFaultRejectAdmission
TEST(PcmDeliveryPolicyTests, StaleTokenWrongGenerationInactiveStateAndPendingFaultRejectAdmission)
{
    const RequestContext baseCtx = CreateHealthySpeakingContext();
    const RequestToken validToken = baseCtx.token;

    struct TestCase
    {
        const char* name;
        RequestContext ctx;
        RequestToken token;
    };

    auto makeCtx = [&](DownstreamState st, bool fault = false)
    {
        RequestContext c = baseCtx;
        c.downstreamState = st;
        c.faultPending = fault;
        return c;
    };

    const TestCase cases[] =
    {
        { "Stale speakId", baseCtx, RequestToken{ kTestSpeakId + 1, kTestGeneration } },
        { "Wrong generation", baseCtx, RequestToken{ kTestSpeakId, kTestGeneration + 1 } },
        { "Inactive Cancelling", makeCtx(DownstreamState::Cancelling), validToken },
        { "Inactive Idle", makeCtx(DownstreamState::Idle), validToken },
        { "Inactive Faulted", makeCtx(DownstreamState::Faulted), validToken },
        { "Pending fault", makeCtx(DownstreamState::Speaking, true), validToken }
    };

    for (const auto& tc : cases)
    {
        const SpanAdmission adm = EvaluateSpanAdmission(tc.ctx, tc.token);
        EXPECT_EQ(adm, SpanAdmission::Reject) << "Failed rejection for: " << tc.name;
    }
}

// 8. AcceptedSpeakingBatchCreditsFullPrefixAndChecksCompletion
TEST(PcmDeliveryPolicyTests, AcceptedSpeakingBatchCreditsFullPrefixAndChecksCompletion)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    const RequestToken batchToken = ctx.token;

    const BatchDecision dec = EvaluateBatchOutcome(ctx, batchToken, 1024, false);

    EXPECT_EQ(dec.action, BatchAction::CreditAndCheckBoundary);
    EXPECT_EQ(dec.acceptedBytesToCredit, 1024u);
}

// 9. RejectedSpeakingBatchCreditsAcceptedPrefixAndBeginsCancellation
TEST(PcmDeliveryPolicyTests, RejectedSpeakingBatchCreditsAcceptedPrefixAndBeginsCancellation)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    const RequestToken batchToken = ctx.token;

    const BatchDecision dec = EvaluateBatchOutcome(ctx, batchToken, 512, true);

    EXPECT_EQ(dec.action, BatchAction::CreditAndBeginCancellation);
    EXPECT_EQ(dec.acceptedBytesToCredit, 512u);
}

// 10. PostCallbackOutcomesPreserveDistinctStateBehaviors
TEST(PcmDeliveryPolicyTests, PostCallbackOutcomesPreserveDistinctStateBehaviors)
{
    const RequestContext baseCtx = CreateHealthySpeakingContext();
    const RequestToken validToken = baseCtx.token;
    const RequestToken staleToken{ kTestSpeakId + 99, kTestGeneration };

    // Stale token ignores batch and credits zero
    {
        const BatchDecision dec = EvaluateBatchOutcome(baseCtx, staleToken, 1024, false);
        EXPECT_EQ(dec.action, BatchAction::IgnoreStaleOrInactive);
        EXPECT_EQ(dec.acceptedBytesToCredit, 0u);
    }

    // Cancelling state checks cancellation boundary without crediting delivered bytes or re-cancelling
    {
        RequestContext cancellingCtx = baseCtx;
        cancellingCtx.downstreamState = DownstreamState::Cancelling;

        const BatchDecision decNormal = EvaluateBatchOutcome(cancellingCtx, validToken, 1024, false);
        EXPECT_EQ(decNormal.action, BatchAction::CheckCancellationBoundary);
        EXPECT_EQ(decNormal.acceptedBytesToCredit, 0u);

        const BatchDecision decRejected = EvaluateBatchOutcome(cancellingCtx, validToken, 1024, true);
        EXPECT_EQ(decRejected.action, BatchAction::CheckCancellationBoundary);
        EXPECT_EQ(decRejected.acceptedBytesToCredit, 0u);
    }

    // Idle and Faulted states ignore batch and credit zero
    {
        RequestContext idleCtx = baseCtx;
        idleCtx.downstreamState = DownstreamState::Idle;
        const BatchDecision decIdle = EvaluateBatchOutcome(idleCtx, validToken, 1024, false);
        EXPECT_EQ(decIdle.action, BatchAction::IgnoreStaleOrInactive);
        EXPECT_EQ(decIdle.acceptedBytesToCredit, 0u);

        RequestContext faultedCtx = baseCtx;
        faultedCtx.downstreamState = DownstreamState::Faulted;
        const BatchDecision decFaulted = EvaluateBatchOutcome(faultedCtx, validToken, 1024, false);
        EXPECT_EQ(decFaulted.action, BatchAction::IgnoreStaleOrInactive);
        EXPECT_EQ(decFaulted.acceptedBytesToCredit, 0u);
    }

    // faultPending=true in Speaking state preserves accepted-prefix credit and boundary/rejection path
    {
        RequestContext pendingFaultCtx = baseCtx;
        pendingFaultCtx.faultPending = true;

        const BatchDecision decAccepted = EvaluateBatchOutcome(pendingFaultCtx, validToken, 768, false);
        EXPECT_EQ(decAccepted.action, BatchAction::CreditAndCheckBoundary);
        EXPECT_EQ(decAccepted.acceptedBytesToCredit, 768u);

        const BatchDecision decRejected = EvaluateBatchOutcome(pendingFaultCtx, validToken, 768, true);
        EXPECT_EQ(decRejected.action, BatchAction::CreditAndBeginCancellation);
        EXPECT_EQ(decRejected.acceptedBytesToCredit, 768u);
    }
}

// 11. TerminalBoundaryFactsCoverOverrunCompletionAndCancellationSemantics
TEST(PcmDeliveryPolicyTests, TerminalBoundaryFactsCoverOverrunCompletionAndCancellationSemantics)
{
    // Case A: Exact Speaking terminal without carry
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 4000;
        ctx.deliveredAudioBytes = 4000;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, false);
        EXPECT_FALSE(facts.speakingAudioOverrun);
        EXPECT_TRUE(facts.speakingTerminalReached);
        EXPECT_TRUE(facts.cancellationTerminalReached);
    }

    // Case B: Exact Speaking terminal with carry (not complete yet)
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 4000;
        ctx.deliveredAudioBytes = 4000;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, true);
        EXPECT_FALSE(facts.speakingAudioOverrun);
        EXPECT_FALSE(facts.speakingTerminalReached);
        EXPECT_TRUE(facts.cancellationTerminalReached);
    }

    // Case C: Overrun on rawAudioBytesRead
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 4004;
        ctx.deliveredAudioBytes = 4000;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, false);
        EXPECT_TRUE(facts.speakingAudioOverrun);
        EXPECT_FALSE(facts.speakingTerminalReached);
        EXPECT_TRUE(facts.cancellationTerminalReached);
    }

    // Case D: Overrun on deliveredAudioBytes
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 4000;
        ctx.deliveredAudioBytes = 4004;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, false);
        EXPECT_TRUE(facts.speakingAudioOverrun);
        EXPECT_FALSE(facts.speakingTerminalReached);
        EXPECT_TRUE(facts.cancellationTerminalReached);
    }

    // Case E: Incomplete Speaking audio (raw < terminal, delivered < terminal)
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 3000;
        ctx.deliveredAudioBytes = 2000;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, false);
        EXPECT_FALSE(facts.speakingAudioOverrun);
        EXPECT_FALSE(facts.speakingTerminalReached);
        EXPECT_FALSE(facts.cancellationTerminalReached);
    }

    // Case F: Cancellation terminal reached when raw == terminal, even if delivered < terminal
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ctx.upstreamTerminalBytes = 4000;
        ctx.rawAudioBytesRead = 4000;
        ctx.deliveredAudioBytes = 1000;

        const TerminalBoundaryFacts facts = EvaluateTerminalBoundaryFacts(ctx, false);
        EXPECT_FALSE(facts.speakingAudioOverrun);
        EXPECT_FALSE(facts.speakingTerminalReached);
        EXPECT_TRUE(facts.cancellationTerminalReached);
    }
}

// 12. IsTerminalByteCountFrameAlignedCoversZeroExactAndRemainders
TEST(PcmDeliveryPolicyTests, IsTerminalByteCountFrameAlignedCoversZeroExactAndRemainders)
{
    // Zero block align is always invalid
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(0, 0));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(4, 0));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(1600, 0));

    // Valid block align 4 (e.g. 16-bit stereo)
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(0, 4));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(4, 4));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(8, 4));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(1600, 4));

    EXPECT_FALSE(IsTerminalByteCountFrameAligned(1, 4));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(2, 4));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(3, 4));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(1601, 4));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(1602, 4));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(1603, 4));

    // Valid block align 2 (16-bit mono)
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(0, 2));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(2, 2));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(3, 2));

    // Valid block align 6 (24-bit stereo)
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(0, 6));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(6, 6));
    EXPECT_TRUE(IsTerminalByteCountFrameAligned(12, 6));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(7, 6));
    EXPECT_FALSE(IsTerminalByteCountFrameAligned(10, 6));
}
