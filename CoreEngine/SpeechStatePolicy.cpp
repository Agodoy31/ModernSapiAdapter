#include "pch.h"
#include "SpeechStatePolicy.h"

namespace SpeechStatePolicy
{
[[nodiscard]] StartDecision EvaluateStart(
    const RequestContext& context,
    std::uint64_t speakId,
    std::uint64_t nextGeneration) noexcept
{
    if (context.upstreamState == UpstreamState::Idle &&
        context.downstreamState == DownstreamState::Idle &&
        !context.faultPending)
    {
        return {StartAction::Accept, speakId, nextGeneration};
    }
    return {StartAction::Reject, 0, 0};
}

[[nodiscard]] UpstreamTerminalDecision EvaluateUpstreamTerminal(
    const RequestContext& context,
    UpstreamTerminalKind kind,
    std::uint64_t terminalAudioBytes,
    bool hasValidTerminalBytes,
    bool isFrameAligned) noexcept
{
    if (context.upstreamFinished)
    {
        return {UpstreamTerminalAction::DuplicateFault, UpstreamState::Faulted, 0};
    }
    if (!hasValidTerminalBytes)
    {
        return {UpstreamTerminalAction::InvalidBytesFault, UpstreamState::Faulted, 0};
    }
    if (!isFrameAligned)
    {
        return {UpstreamTerminalAction::MisalignedBytesFault, UpstreamState::Faulted, 0};
    }

    UpstreamState target = (kind == UpstreamTerminalKind::Completed) ? UpstreamState::Completed : UpstreamState::Cancelled;
    return {UpstreamTerminalAction::Apply, target, terminalAudioBytes};
}

[[nodiscard]] RequestFailureDecision EvaluateUtteranceFailure(
    const RequestContext& context) noexcept
{
    return {RequestFailureAction::ApplyUtteranceFailure, context.rawAudioBytesRead};
}

[[nodiscard]] TerminalBoundaryDecision EvaluateTerminalBoundary(
    const RequestContext& context,
    const TerminalBoundaryFacts& facts) noexcept
{
    if (context.upstreamState == UpstreamState::Failed)
    {
        return {TerminalBoundaryAction::UtteranceFailedReset};
    }
    if (!context.upstreamFinished)
    {
        return {TerminalBoundaryAction::Continue};
    }
    if (context.downstreamState == DownstreamState::Speaking && facts.speakingAudioOverrun)
    {
        return {TerminalBoundaryAction::OverrunFault};
    }
    if (context.downstreamState == DownstreamState::Speaking && facts.speakingTerminalReached)
    {
        return {TerminalBoundaryAction::NormalCompleteReset};
    }
    if (context.downstreamState == DownstreamState::Cancelling && facts.cancellationTerminalReached)
    {
        return {TerminalBoundaryAction::CancelDrainReset};
    }
    return {TerminalBoundaryAction::Continue};
}

[[nodiscard]] BeginCancellationDecision EvaluateBeginCancellation(
    const RequestContext& context,
    std::uint64_t cancellationDeadlineTick) noexcept
{
    if (context.downstreamState == DownstreamState::Idle)
    {
        return {BeginCancellationAction::AlreadyIdle, 0, 0};
    }
    if (context.downstreamState == DownstreamState::Faulted || context.upstreamState == UpstreamState::Faulted)
    {
        return {BeginCancellationAction::Faulted, 0, 0};
    }
    if (context.downstreamState == DownstreamState::Cancelling)
    {
        return {BeginCancellationAction::AlreadyCancelling, 0, 0};
    }
    return {BeginCancellationAction::TransitionToCancelling, context.token.speakId, cancellationDeadlineTick};
}

[[nodiscard]] StopDecision EvaluateStop(
    const RequestContext& context) noexcept
{
    if (context.downstreamState == DownstreamState::Idle ||
        context.downstreamState == DownstreamState::Faulted ||
        context.upstreamState == UpstreamState::Faulted)
    {
        return {StopAction::NoAction, 0};
    }
    return {StopAction::ResetAndCancel, context.token.speakId};
}

[[nodiscard]] TimeoutDecision EvaluateTimeouts(
    const RequestContext& context,
    std::uint64_t nowTick,
    std::uint64_t lastProgressTick,
    std::uint64_t inactivityTimeoutMs) noexcept
{
    if (context.IsDrainingCancellation() && context.cancellationDeadlineTick != 0 && nowTick >= context.cancellationDeadlineTick)
    {
        return {TimeoutCondition::CancellationTimeout, context.token.speakId};
    }

    if (context.IsActivelySynthesizing() || context.IsAwaitingTerminalAudio())
    {
        if (nowTick >= lastProgressTick && (nowTick - lastProgressTick) >= inactivityTimeoutMs)
        {
            return {TimeoutCondition::InactivityTimeout, context.token.speakId};
        }
    }

    return {TimeoutCondition::None, 0};
}

[[nodiscard]] bool IsWaitTerminal(
    const RequestContext& context,
    bool exitSignaled) noexcept
{
    return context.downstreamState == DownstreamState::Idle ||
           context.downstreamState == DownstreamState::Faulted ||
           exitSignaled;
}
}
