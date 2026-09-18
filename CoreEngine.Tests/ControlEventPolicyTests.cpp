#include "pch.h"
#include <gtest/gtest.h>
#include <cstring>
#include <vector>
#include "../CoreEngine/ControlEventPolicy.h"

using namespace ControlEventPolicy;

class ControlEventPolicyTests : public ::testing::Test
{
protected:
    static constexpr uint64_t kTestSpeakId = 42ULL;
    static constexpr uint64_t kOtherSpeakId = 99ULL;

    RequestContext CreateHealthySpeakingContext(uint64_t speakId = kTestSpeakId)
    {
        RequestContext ctx;
        ctx.Reset();
        ctx.token.speakId = speakId;
        ctx.token.generation = 1;
        ctx.upstreamState = UpstreamState::Active;
        ctx.downstreamState = DownstreamState::Speaking;
        ctx.faultPending = false;
        return ctx;
    }

    ProviderControlEvent CreateBoundaryEvent(
        ProviderEventType type = ProviderEventType::WordBoundary,
        uint64_t speakId = kTestSpeakId,
        bool validOffsets = true)
    {
        ProviderControlEvent ev;
        ev.type = type;
        ev.speakId = speakId;
        ev.hasValidSpeechOffsets = validOffsets;
        ev.speechOffsets.audioOffsetMs = 100;
        ev.speechOffsets.textOffset = 5;
        ev.speechOffsets.textLength = 4;
        return ev;
    }

    ProviderControlEvent CreateTerminalEvent(
        ProviderEventType type = ProviderEventType::SynthesisComplete,
        uint64_t speakId = kTestSpeakId,
        bool validBytes = true,
        uint64_t terminalBytes = 1024)
    {
        ProviderControlEvent ev;
        ev.type = type;
        ev.speakId = speakId;
        ev.hasValidTerminalBytes = validBytes;
        ev.terminalAudioBytes = terminalBytes;
        return ev;
    }

    ProviderControlEvent CreateLogEvent(
        std::string_view severity,
        uint64_t speakId = kTestSpeakId)
    {
        ProviderControlEvent ev;
        ev.type = ProviderEventType::Log;
        ev.speakId = speakId;
        ev.logSeverity = severity;
        ev.logMessage = "Test log message";
        return ev;
    }
};

// 1. MatchedWordBoundarySpeakingRefreshesAndAdmits
TEST_F(ControlEventPolicyTests, MatchedWordBoundarySpeakingRefreshesAndAdmits)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ProviderControlEvent ev = CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true);

    const EventDecision decision = EvaluateParsedEvent(ev, ctx);
    EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
    EXPECT_TRUE(decision.shouldRefreshProgress);
    EXPECT_TRUE(decision.shouldForwardToSapi);

    const FinalAdmission admission = EvaluateFinalAdmission(ev, ctx);
    EXPECT_EQ(admission, FinalAdmission::Allow);
}

// 2. SentenceAndBookmarkUseSpeechBoundaryPolicy
TEST_F(ControlEventPolicyTests, SentenceAndBookmarkUseSpeechBoundaryPolicy)
{
    const ProviderEventType boundaryTypes[] =
    {
        ProviderEventType::SentenceBoundary,
        ProviderEventType::Bookmark
    };

    for (const ProviderEventType type : boundaryTypes)
    {
        // Speaking state: refreshes and forwards provisionally + final allow
        RequestContext speakingCtx = CreateHealthySpeakingContext();
        ProviderControlEvent ev = CreateBoundaryEvent(type, kTestSpeakId, true);

        EventDecision decision = EvaluateParsedEvent(ev, speakingCtx);
        EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
        EXPECT_TRUE(decision.shouldRefreshProgress);
        EXPECT_TRUE(decision.shouldForwardToSapi);

        FinalAdmission admission = EvaluateFinalAdmission(ev, speakingCtx);
        EXPECT_EQ(admission, FinalAdmission::Allow);

        // Cancelling state: refreshes but does not forward
        RequestContext cancellingCtx = speakingCtx;
        cancellingCtx.downstreamState = DownstreamState::Cancelling;

        decision = EvaluateParsedEvent(ev, cancellingCtx);
        EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
        EXPECT_TRUE(decision.shouldRefreshProgress);
        EXPECT_FALSE(decision.shouldForwardToSapi);

        admission = EvaluateFinalAdmission(ev, cancellingCtx);
        EXPECT_EQ(admission, FinalAdmission::Reject);
    }
}

// 3. MalformedMatchedBoundaryFaultActionSuppressesProgressAndForwarding
// (table-driven across Speaking, Cancelling, and Idle)
TEST_F(ControlEventPolicyTests, MalformedMatchedBoundaryFaultActionSuppressesProgressAndForwarding)
{
    const ProviderEventType boundaryTypes[] =
    {
        ProviderEventType::WordBoundary,
        ProviderEventType::SentenceBoundary,
        ProviderEventType::Bookmark
    };

    const DownstreamState downstreamStates[] =
    {
        DownstreamState::Speaking,
        DownstreamState::Cancelling,
        DownstreamState::Idle
    };

    for (const ProviderEventType type : boundaryTypes)
    {
        for (const DownstreamState state : downstreamStates)
        {
            RequestContext ctx = CreateHealthySpeakingContext();
            ctx.downstreamState = state;

            ProviderControlEvent ev = CreateBoundaryEvent(type, kTestSpeakId, false);

            const EventDecision decision = EvaluateParsedEvent(ev, ctx);
            EXPECT_EQ(decision.action, EventAction::MalformedSpeechBoundary);
            EXPECT_FALSE(decision.shouldRefreshProgress);
            EXPECT_FALSE(decision.shouldForwardToSapi);
        }
    }
}

// 4. ValidBoundaryCancellingRefreshesButDoesNotForward
TEST_F(ControlEventPolicyTests, ValidBoundaryCancellingRefreshesButDoesNotForward)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.downstreamState = DownstreamState::Cancelling;

    ProviderControlEvent ev = CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true);

    const EventDecision decision = EvaluateParsedEvent(ev, ctx);
    EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
    EXPECT_TRUE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);

    const FinalAdmission admission = EvaluateFinalAdmission(ev, ctx);
    EXPECT_EQ(admission, FinalAdmission::Reject);
}

// 5. ValidBoundaryIdleNeitherRefreshesNorForwards
TEST_F(ControlEventPolicyTests, ValidBoundaryIdleNeitherRefreshesNorForwards)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.downstreamState = DownstreamState::Idle;

    ProviderControlEvent ev = CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true);

    const EventDecision decision = EvaluateParsedEvent(ev, ctx);
    EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);

    const FinalAdmission admission = EvaluateFinalAdmission(ev, ctx);
    EXPECT_EQ(admission, FinalAdmission::Reject);
}

// 6. ValidBoundaryWithFaultPendingStillRefreshesButDoesNotForward
TEST_F(ControlEventPolicyTests, ValidBoundaryWithFaultPendingStillRefreshesButDoesNotForward)
{
    RequestContext ctx = CreateHealthySpeakingContext();
    ctx.downstreamState = DownstreamState::Speaking;
    ctx.faultPending = true;

    ProviderControlEvent ev = CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true);

    const EventDecision decision = EvaluateParsedEvent(ev, ctx);
    EXPECT_EQ(decision.action, EventAction::SpeechBoundary);
    EXPECT_TRUE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);

    const FinalAdmission admission = EvaluateFinalAdmission(ev, ctx);
    EXPECT_EQ(admission, FinalAdmission::Reject);
}

// 7. ValidSynthesisCompleteClassifiesAndRefreshes
// (tables Speaking, Cancelling, Idle, Faulted, fault-pending, and duplicate-terminal facts)
TEST_F(ControlEventPolicyTests, ValidSynthesisCompleteClassifiesAndRefreshes)
{
    struct TestCase
    {
        const char* description;
        UpstreamState upstream;
        DownstreamState downstream;
        bool faultPending;
        bool upstreamFinished;
        bool expectedRefresh;
        bool expectedForward;
    };

    const TestCase cases[] =
    {
        { "Speaking healthy", UpstreamState::Active, DownstreamState::Speaking, false, false, true, true },
        { "Cancelling healthy", UpstreamState::Active, DownstreamState::Cancelling, false, false, true, true },
        { "Idle healthy", UpstreamState::Idle, DownstreamState::Idle, false, false, false, true },
        { "Downstream Faulted", UpstreamState::Active, DownstreamState::Faulted, false, false, false, false },
        { "Upstream Faulted while Speaking", UpstreamState::Faulted, DownstreamState::Speaking, false, false, true, false },
        { "Fault pending while Speaking", UpstreamState::Active, DownstreamState::Speaking, true, false, true, false },
        { "Duplicate-terminal fact (upstreamFinished true)", UpstreamState::Completed, DownstreamState::Speaking, false, true, true, true }
    };

    ProviderControlEvent ev = CreateTerminalEvent(ProviderEventType::SynthesisComplete, kTestSpeakId, true);

    for (const auto& tc : cases)
    {
        RequestContext ctx;
        ctx.Reset();
        ctx.token.speakId = kTestSpeakId;
        ctx.token.generation = 1;
        ctx.upstreamState = tc.upstream;
        ctx.downstreamState = tc.downstream;
        ctx.faultPending = tc.faultPending;
        ctx.upstreamFinished = tc.upstreamFinished;

        const EventDecision decision = EvaluateParsedEvent(ev, ctx);
        EXPECT_EQ(decision.action, EventAction::SynthesisComplete) << tc.description;
        EXPECT_EQ(decision.shouldRefreshProgress, tc.expectedRefresh) << tc.description;
        EXPECT_EQ(decision.shouldForwardToSapi, tc.expectedForward) << tc.description;
    }
}

// 8. ValidSynthesisCancelledClassifiesAndRefreshes
// (tables Speaking, Cancelling, Idle, Faulted, and fault-pending)
TEST_F(ControlEventPolicyTests, ValidSynthesisCancelledClassifiesAndRefreshes)
{
    struct TestCase
    {
        const char* description;
        UpstreamState upstream;
        DownstreamState downstream;
        bool faultPending;
        bool expectedRefresh;
        bool expectedForward;
    };

    const TestCase cases[] =
    {
        { "Speaking healthy", UpstreamState::Active, DownstreamState::Speaking, false, true, true },
        { "Cancelling healthy", UpstreamState::Active, DownstreamState::Cancelling, false, true, true },
        { "Idle healthy", UpstreamState::Idle, DownstreamState::Idle, false, false, true },
        { "Downstream Faulted", UpstreamState::Active, DownstreamState::Faulted, false, false, false },
        { "Upstream Faulted while Speaking", UpstreamState::Faulted, DownstreamState::Speaking, false, true, false },
        { "Fault pending while Speaking", UpstreamState::Active, DownstreamState::Speaking, true, true, false }
    };

    ProviderControlEvent ev = CreateTerminalEvent(ProviderEventType::SynthesisCancelled, kTestSpeakId, true);

    for (const auto& tc : cases)
    {
        RequestContext ctx;
        ctx.Reset();
        ctx.token.speakId = kTestSpeakId;
        ctx.token.generation = 1;
        ctx.upstreamState = tc.upstream;
        ctx.downstreamState = tc.downstream;
        ctx.faultPending = tc.faultPending;

        const EventDecision decision = EvaluateParsedEvent(ev, ctx);
        EXPECT_EQ(decision.action, EventAction::SynthesisCancelled) << tc.description;
        EXPECT_EQ(decision.shouldRefreshProgress, tc.expectedRefresh) << tc.description;
        EXPECT_EQ(decision.shouldForwardToSapi, tc.expectedForward) << tc.description;
    }
}

// 9. InvalidTerminalPayloadDoesNotRefreshProgress
// (covers both terminal types in each lifecycle category)
TEST_F(ControlEventPolicyTests, InvalidTerminalPayloadDoesNotRefreshProgress)
{
    const ProviderEventType terminalTypes[] =
    {
        ProviderEventType::SynthesisComplete,
        ProviderEventType::SynthesisCancelled
    };

    struct LifecycleCase
    {
        const char* description;
        DownstreamState downstream;
        UpstreamState upstream;
        bool faultPending;
        bool expectedForward;
    };

    const LifecycleCase cases[] =
    {
        { "Speaking healthy", DownstreamState::Speaking, UpstreamState::Active, false, true },
        { "Cancelling healthy", DownstreamState::Cancelling, UpstreamState::Active, false, true },
        { "Idle healthy", DownstreamState::Idle, UpstreamState::Idle, false, true },
        { "Downstream Faulted", DownstreamState::Faulted, UpstreamState::Active, false, false }
    };

    for (const ProviderEventType type : terminalTypes)
    {
        ProviderControlEvent ev = CreateTerminalEvent(type, kTestSpeakId, false);
        const EventAction expectedAction = (type == ProviderEventType::SynthesisComplete)
            ? EventAction::SynthesisComplete
            : EventAction::SynthesisCancelled;

        for (const auto& lc : cases)
        {
            RequestContext ctx;
            ctx.Reset();
            ctx.token.speakId = kTestSpeakId;
            ctx.token.generation = 1;
            ctx.downstreamState = lc.downstream;
            ctx.upstreamState = lc.upstream;
            ctx.faultPending = lc.faultPending;

            const EventDecision decision = EvaluateParsedEvent(ev, ctx);
            EXPECT_EQ(decision.action, expectedAction) << lc.description;
            EXPECT_FALSE(decision.shouldRefreshProgress) << lc.description;
            EXPECT_EQ(decision.shouldForwardToSapi, lc.expectedForward) << lc.description;
        }
    }
}

// 10. StaleAdversarialEventsHaveNoActionOrProgressButPreserveHealthyProvisionalAdmission
// (tables valid and malformed boundaries, invalid terminal, error/fatal logs, legacy, and unknown; also repeats with P0 == false)
TEST_F(ControlEventPolicyTests, StaleAdversarialEventsHaveNoActionOrProgressButPreserveHealthyProvisionalAdmission)
{
    std::vector<ProviderControlEvent> staleEvents;

    // Valid boundary
    staleEvents.push_back(CreateBoundaryEvent(ProviderEventType::WordBoundary, kOtherSpeakId, true));
    // Malformed boundary
    staleEvents.push_back(CreateBoundaryEvent(ProviderEventType::WordBoundary, kOtherSpeakId, false));
    // Invalid terminal
    staleEvents.push_back(CreateTerminalEvent(ProviderEventType::SynthesisComplete, kOtherSpeakId, false));
    // Error log
    staleEvents.push_back(CreateLogEvent("error", kOtherSpeakId));
    // Fatal log
    staleEvents.push_back(CreateLogEvent("fatal", kOtherSpeakId));
    // Legacy completed
    {
        ProviderControlEvent ev;
        ev.type = ProviderEventType::LegacyCompleted;
        ev.speakId = kOtherSpeakId;
        staleEvents.push_back(ev);
    }
    // Unknown
    {
        ProviderControlEvent ev;
        ev.type = ProviderEventType::Unknown;
        ev.speakId = kOtherSpeakId;
        staleEvents.push_back(ev);
    }

    // Subcase A: Healthy context (P0 == true)
    {
        RequestContext healthyCtx = CreateHealthySpeakingContext(kTestSpeakId);

        for (size_t i = 0; i < staleEvents.size(); ++i)
        {
            const EventDecision decision = EvaluateParsedEvent(staleEvents[i], healthyCtx);
            EXPECT_EQ(decision.action, EventAction::NoLockedAction) << "Event index: " << i;
            EXPECT_FALSE(decision.shouldRefreshProgress) << "Event index: " << i;
            EXPECT_TRUE(decision.shouldForwardToSapi) << "Event index: " << i;
        }
    }

    // Subcase B: Unhealthy context (P0 == false, via faultPending)
    {
        RequestContext unhealthyCtx = CreateHealthySpeakingContext(kTestSpeakId);
        unhealthyCtx.faultPending = true;

        for (size_t i = 0; i < staleEvents.size(); ++i)
        {
            const EventDecision decision = EvaluateParsedEvent(staleEvents[i], unhealthyCtx);
            EXPECT_EQ(decision.action, EventAction::NoLockedAction) << "Event index: " << i;
            EXPECT_FALSE(decision.shouldRefreshProgress) << "Event index: " << i;
            EXPECT_FALSE(decision.shouldForwardToSapi) << "Event index: " << i;
        }
    }
}

// 11. FaultedOrFaultPendingContextSuppressesProvisionalAdmission
// (covers matching and stale events with upstream Faulted, downstream Faulted, and fault-pending independently)
TEST_F(ControlEventPolicyTests, FaultedOrFaultPendingContextSuppressesProvisionalAdmission)
{
    const uint64_t speakIdsToTest[] = { kTestSpeakId, kOtherSpeakId };

    const ProviderControlEvent events[] =
    {
        CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true),
        CreateTerminalEvent(ProviderEventType::SynthesisComplete, kTestSpeakId, true),
        CreateLogEvent("info", kTestSpeakId),
        CreateLogEvent("error", kTestSpeakId),
        CreateLogEvent("fatal", kTestSpeakId)
    };

    struct SuppressionCase
    {
        const char* description;
        UpstreamState upstream;
        DownstreamState downstream;
        bool faultPending;
    };

    const SuppressionCase cases[] =
    {
        { "Upstream Faulted", UpstreamState::Faulted, DownstreamState::Speaking, false },
        { "Downstream Faulted", UpstreamState::Active, DownstreamState::Faulted, false },
        { "Fault pending", UpstreamState::Active, DownstreamState::Speaking, true }
    };

    for (const uint64_t id : speakIdsToTest)
    {
        for (const auto& baseEvent : events)
        {
            ProviderControlEvent ev = baseEvent;
            ev.speakId = id;

            for (const auto& sc : cases)
            {
                RequestContext ctx;
                ctx.Reset();
                ctx.token.speakId = kTestSpeakId;
                ctx.token.generation = 1;
                ctx.upstreamState = sc.upstream;
                ctx.downstreamState = sc.downstream;
                ctx.faultPending = sc.faultPending;

                const EventDecision decision = EvaluateParsedEvent(ev, ctx);
                EXPECT_FALSE(decision.shouldForwardToSapi)
                    << "Failed suppression for " << sc.description
                    << ", speakId " << id
                    << ", eventType " << static_cast<int>(ev.type);
            }
        }
    }
}

// 12. LegacyCompletedHasDiagnosticActionWithoutProgress
TEST_F(ControlEventPolicyTests, LegacyCompletedHasDiagnosticActionWithoutProgress)
{
    ProviderControlEvent ev;
    ev.type = ProviderEventType::LegacyCompleted;
    ev.speakId = kTestSpeakId;

    // Healthy context: action is LegacyCompleted, no progress, P0 is true
    RequestContext healthyCtx = CreateHealthySpeakingContext();
    EventDecision decision = EvaluateParsedEvent(ev, healthyCtx);
    EXPECT_EQ(decision.action, EventAction::LegacyCompleted);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_TRUE(decision.shouldForwardToSapi);

    FinalAdmission admission = EvaluateFinalAdmission(ev, healthyCtx);
    EXPECT_EQ(admission, FinalAdmission::Allow);

    // Unhealthy context: action is LegacyCompleted, no progress, P0 is false
    RequestContext unhealthyCtx = healthyCtx;
    unhealthyCtx.faultPending = true;
    decision = EvaluateParsedEvent(ev, unhealthyCtx);
    EXPECT_EQ(decision.action, EventAction::LegacyCompleted);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);

    admission = EvaluateFinalAdmission(ev, unhealthyCtx);
    EXPECT_EQ(admission, FinalAdmission::Reject);
}

// 13. UnknownEventHasDiagnosticActionWithoutProgress
TEST_F(ControlEventPolicyTests, UnknownEventHasDiagnosticActionWithoutProgress)
{
    ProviderControlEvent ev;
    ev.type = ProviderEventType::Unknown;
    ev.speakId = kTestSpeakId;

    // Healthy context: action is Unknown, no progress, P0 is true
    RequestContext healthyCtx = CreateHealthySpeakingContext();
    EventDecision decision = EvaluateParsedEvent(ev, healthyCtx);
    EXPECT_EQ(decision.action, EventAction::Unknown);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_TRUE(decision.shouldForwardToSapi);

    FinalAdmission admission = EvaluateFinalAdmission(ev, healthyCtx);
    EXPECT_EQ(admission, FinalAdmission::Allow);

    // Unhealthy context: action is Unknown, no progress, P0 is false
    RequestContext unhealthyCtx = healthyCtx;
    unhealthyCtx.faultPending = true;
    decision = EvaluateParsedEvent(ev, unhealthyCtx);
    EXPECT_EQ(decision.action, EventAction::Unknown);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);

    admission = EvaluateFinalAdmission(ev, unhealthyCtx);
    EXPECT_EQ(admission, FinalAdmission::Reject);
}

// 14. NonErrorLogSeveritiesRemainInformational
// (empty, info, warning, uppercase and arbitrary; no progress and exact pre-mutation P0)
TEST_F(ControlEventPolicyTests, NonErrorLogSeveritiesRemainInformational)
{
    const std::string_view severities[] =
    {
        "",
        "info",
        "warning",
        "ERROR",
        "FATAL",
        "Error",
        "Fatal",
        "debug",
        "arbitrary_severity"
    };

    for (const std::string_view sev : severities)
    {
        ProviderControlEvent ev = CreateLogEvent(sev, kTestSpeakId);

        // Pre-mutation P0 == true
        RequestContext healthyCtx = CreateHealthySpeakingContext();
        EventDecision decision = EvaluateParsedEvent(ev, healthyCtx);
        EXPECT_EQ(decision.action, EventAction::InformationalLog) << "Severity: " << sev;
        EXPECT_FALSE(decision.shouldRefreshProgress) << "Severity: " << sev;
        EXPECT_TRUE(decision.shouldForwardToSapi) << "Severity: " << sev;

        // Pre-mutation P0 == false
        RequestContext unhealthyCtx = healthyCtx;
        unhealthyCtx.faultPending = true;
        decision = EvaluateParsedEvent(ev, unhealthyCtx);
        EXPECT_EQ(decision.action, EventAction::InformationalLog) << "Severity: " << sev;
        EXPECT_FALSE(decision.shouldRefreshProgress) << "Severity: " << sev;
        EXPECT_FALSE(decision.shouldForwardToSapi) << "Severity: " << sev;
    }
}

// 15. ExactLowercaseErrorClassifiesAsRequestError
// (no progress and exact pre-mutation P0)
TEST_F(ControlEventPolicyTests, ExactLowercaseErrorClassifiesAsRequestError)
{
    ProviderControlEvent ev = CreateLogEvent("error", kTestSpeakId);

    // Pre-mutation P0 == true
    RequestContext healthyCtx = CreateHealthySpeakingContext();
    EventDecision decision = EvaluateParsedEvent(ev, healthyCtx);
    EXPECT_EQ(decision.action, EventAction::RequestErrorLog);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_TRUE(decision.shouldForwardToSapi);

    // Pre-mutation P0 == false
    RequestContext unhealthyCtx = healthyCtx;
    unhealthyCtx.faultPending = true;
    decision = EvaluateParsedEvent(ev, unhealthyCtx);
    EXPECT_EQ(decision.action, EventAction::RequestErrorLog);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);
}

// 16. ExactLowercaseFatalClassifiesAsFatalAndPreservesHealthyProvisionalAdmission
// (no progress and exact pre-mutation P0)
TEST_F(ControlEventPolicyTests, ExactLowercaseFatalClassifiesAsFatalAndPreservesHealthyProvisionalAdmission)
{
    ProviderControlEvent ev = CreateLogEvent("fatal", kTestSpeakId);

    // Pre-mutation P0 == true
    RequestContext healthyCtx = CreateHealthySpeakingContext();
    EventDecision decision = EvaluateParsedEvent(ev, healthyCtx);
    EXPECT_EQ(decision.action, EventAction::FatalLog);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_TRUE(decision.shouldForwardToSapi);

    // Pre-mutation P0 == false (e.g. upstreamState == Faulted)
    RequestContext unhealthyCtx = healthyCtx;
    unhealthyCtx.upstreamState = UpstreamState::Faulted;
    decision = EvaluateParsedEvent(ev, unhealthyCtx);
    EXPECT_EQ(decision.action, EventAction::FatalLog);
    EXPECT_FALSE(decision.shouldRefreshProgress);
    EXPECT_FALSE(decision.shouldForwardToSapi);
}

// 17. FinalAdmissionAllowsPreviouslyAdmittedMatchingLogAfterLifecycleOrFaultPendingChange
// (sequence-tests healthy error/fatal provisional admission followed by their own lifecycle/fault-pending mutation
// and final admission; separately proves pre-existing fault-pending makes provisional admission false; tables
// Speaking, Cancelling, Idle, Faulted, and fault-pending at the second gate)
TEST_F(ControlEventPolicyTests, FinalAdmissionAllowsPreviouslyAdmittedMatchingLogAfterLifecycleOrFaultPendingChange)
{
    // Part A: Error log sequence
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ProviderControlEvent errorEv = CreateLogEvent("error", kTestSpeakId);

        const EventDecision provisional = EvaluateParsedEvent(errorEv, ctx);
        EXPECT_EQ(provisional.action, EventAction::RequestErrorLog);
        EXPECT_TRUE(provisional.shouldForwardToSapi);

        // Simulate worker applying RequestErrorLog: downstream transitions to Idle, upstream to Failed
        ctx.downstreamState = DownstreamState::Idle;
        ctx.upstreamState = UpstreamState::Failed;

        const FinalAdmission finalAdm = EvaluateFinalAdmission(errorEv, ctx);
        EXPECT_EQ(finalAdm, FinalAdmission::Allow);
    }

    // Part B: Fatal log sequence
    {
        RequestContext ctx = CreateHealthySpeakingContext();
        ProviderControlEvent fatalEv = CreateLogEvent("fatal", kTestSpeakId);

        const EventDecision provisional = EvaluateParsedEvent(fatalEv, ctx);
        EXPECT_EQ(provisional.action, EventAction::FatalLog);
        EXPECT_TRUE(provisional.shouldForwardToSapi);

        // Simulate worker setting faultPending before fatal log final admission
        ctx.faultPending = true;
        ctx.downstreamState = DownstreamState::Faulted;

        const FinalAdmission finalAdm = EvaluateFinalAdmission(fatalEv, ctx);
        EXPECT_EQ(finalAdm, FinalAdmission::Allow);
    }

    // Part C: Pre-existing faultPending suppresses provisional admission
    {
        RequestContext preFaultCtx = CreateHealthySpeakingContext();
        preFaultCtx.faultPending = true;

        ProviderControlEvent errorEv = CreateLogEvent("error", kTestSpeakId);
        EventDecision provError = EvaluateParsedEvent(errorEv, preFaultCtx);
        EXPECT_FALSE(provError.shouldForwardToSapi);

        ProviderControlEvent fatalEv = CreateLogEvent("fatal", kTestSpeakId);
        EventDecision provFatal = EvaluateParsedEvent(fatalEv, preFaultCtx);
        EXPECT_FALSE(provFatal.shouldForwardToSapi);
    }

    // Part D: Table at second gate for matching Log across downstream states and faultPending
    {
        const DownstreamState states[] =
        {
            DownstreamState::Speaking,
            DownstreamState::Cancelling,
            DownstreamState::Idle,
            DownstreamState::Faulted
        };

        const bool faultPendingValues[] = { false, true };

        ProviderControlEvent logEv = CreateLogEvent("info", kTestSpeakId);

        for (const DownstreamState st : states)
        {
            for (const bool fp : faultPendingValues)
            {
                RequestContext ctx;
                ctx.Reset();
                ctx.token.speakId = kTestSpeakId;
                ctx.token.generation = 1;
                ctx.downstreamState = st;
                ctx.faultPending = fp;

                const FinalAdmission adm = EvaluateFinalAdmission(logEv, ctx);
                EXPECT_EQ(adm, FinalAdmission::Allow)
                    << "Failed for downstream state " << static_cast<int>(st)
                    << ", faultPending " << fp;
            }
        }
    }
}

// 18. FinalAdmissionRequiresSpeakingAndNoFaultPendingForNonLog
// (tables boundary, terminal, legacy, and unknown categories)
TEST_F(ControlEventPolicyTests, FinalAdmissionRequiresSpeakingAndNoFaultPendingForNonLog)
{
    const ProviderEventType nonLogTypes[] =
    {
        ProviderEventType::WordBoundary,
        ProviderEventType::SentenceBoundary,
        ProviderEventType::Bookmark,
        ProviderEventType::SynthesisComplete,
        ProviderEventType::SynthesisCancelled,
        ProviderEventType::LegacyCompleted,
        ProviderEventType::Unknown
    };

    const DownstreamState states[] =
    {
        DownstreamState::Speaking,
        DownstreamState::Cancelling,
        DownstreamState::Idle,
        DownstreamState::Faulted
    };

    const bool faultPendingValues[] = { false, true };

    for (const ProviderEventType type : nonLogTypes)
    {
        ProviderControlEvent ev;
        ev.type = type;
        ev.speakId = kTestSpeakId;

        for (const DownstreamState st : states)
        {
            for (const bool fp : faultPendingValues)
            {
                RequestContext ctx;
                ctx.Reset();
                ctx.token.speakId = kTestSpeakId;
                ctx.token.generation = 1;
                ctx.downstreamState = st;
                ctx.faultPending = fp;

                const FinalAdmission adm = EvaluateFinalAdmission(ev, ctx);
                const FinalAdmission expected = (st == DownstreamState::Speaking && !fp)
                    ? FinalAdmission::Allow
                    : FinalAdmission::Reject;

                EXPECT_EQ(adm, expected)
                    << "Failed for type " << static_cast<int>(type)
                    << ", state " << static_cast<int>(st)
                    << ", faultPending " << fp;
            }
        }
    }
}

// 19. FinalAdmissionRejectsStaleIdentity
// (covers log and non-log)
TEST_F(ControlEventPolicyTests, FinalAdmissionRejectsStaleIdentity)
{
    const ProviderEventType allTypes[] =
    {
        ProviderEventType::WordBoundary,
        ProviderEventType::SentenceBoundary,
        ProviderEventType::Bookmark,
        ProviderEventType::SynthesisComplete,
        ProviderEventType::SynthesisCancelled,
        ProviderEventType::Log,
        ProviderEventType::LegacyCompleted,
        ProviderEventType::Unknown
    };

    RequestContext healthySpeakingCtx = CreateHealthySpeakingContext(kTestSpeakId);

    for (const ProviderEventType type : allTypes)
    {
        ProviderControlEvent ev;
        ev.type = type;
        ev.speakId = kOtherSpeakId;
        if (type == ProviderEventType::Log)
        {
            ev.logSeverity = "error";
        }

        const FinalAdmission adm = EvaluateFinalAdmission(ev, healthySpeakingCtx);
        EXPECT_EQ(adm, FinalAdmission::Reject)
            << "Failed stale rejection for type " << static_cast<int>(type);
    }
}

// 20. PolicyEvaluationDoesNotMutateInputs
TEST_F(ControlEventPolicyTests, PolicyEvaluationDoesNotMutateInputs)
{
    RequestContext ctx = CreateHealthySpeakingContext(kTestSpeakId);
    const RequestContext ctxSnapshot = ctx;

    ProviderControlEvent ev = CreateBoundaryEvent(ProviderEventType::WordBoundary, kTestSpeakId, true);
    ev.rawEventName = "word_boundary";
    ev.logSeverity = "info";
    ev.logMessage = "message";
    ev.bookmarkName = "bm";
    ev.logFriendlyText = "friendly";
    const ProviderControlEvent evSnapshot = ev;

    const EventDecision dec = EvaluateParsedEvent(ev, ctx);
    EXPECT_EQ(dec.action, EventAction::SpeechBoundary);

    const FinalAdmission adm = EvaluateFinalAdmission(ev, ctx);
    EXPECT_EQ(adm, FinalAdmission::Allow);

    // Verify context fields unchanged (avoiding memcmp over struct padding)
    EXPECT_EQ(ctx.token.speakId, ctxSnapshot.token.speakId);
    EXPECT_EQ(ctx.token.generation, ctxSnapshot.token.generation);
    EXPECT_EQ(ctx.upstreamState, ctxSnapshot.upstreamState);
    EXPECT_EQ(ctx.downstreamState, ctxSnapshot.downstreamState);
    EXPECT_EQ(ctx.rawAudioBytesRead, ctxSnapshot.rawAudioBytesRead);
    EXPECT_EQ(ctx.deliveredAudioBytes, ctxSnapshot.deliveredAudioBytes);
    EXPECT_EQ(ctx.upstreamTerminalBytes, ctxSnapshot.upstreamTerminalBytes);
    EXPECT_EQ(ctx.upstreamFinished, ctxSnapshot.upstreamFinished);
    EXPECT_EQ(ctx.faultPending, ctxSnapshot.faultPending);
    EXPECT_EQ(ctx.cancellationDeadlineTick, ctxSnapshot.cancellationDeadlineTick);
    EXPECT_EQ(ctx.completionHr, ctxSnapshot.completionHr);

    // Verify event fields unchanged
    EXPECT_EQ(ev.type, evSnapshot.type);
    EXPECT_EQ(ev.speakId, evSnapshot.speakId);
    EXPECT_EQ(ev.speechOffsets.audioOffsetMs, evSnapshot.speechOffsets.audioOffsetMs);
    EXPECT_EQ(ev.speechOffsets.textOffset, evSnapshot.speechOffsets.textOffset);
    EXPECT_EQ(ev.speechOffsets.textLength, evSnapshot.speechOffsets.textLength);
    EXPECT_EQ(ev.terminalAudioBytes, evSnapshot.terminalAudioBytes);
    EXPECT_EQ(ev.hasValidSpeechOffsets, evSnapshot.hasValidSpeechOffsets);
    EXPECT_EQ(ev.hasValidTerminalBytes, evSnapshot.hasValidTerminalBytes);
    EXPECT_EQ(ev.rawEventName, evSnapshot.rawEventName);
    EXPECT_EQ(ev.logSeverity, evSnapshot.logSeverity);
    EXPECT_EQ(ev.logMessage, evSnapshot.logMessage);
    EXPECT_EQ(ev.bookmarkName, evSnapshot.bookmarkName);
    EXPECT_EQ(ev.logFriendlyText, evSnapshot.logFriendlyText);
}
