#include "pch.h"
#include "PcmDeliveryPolicy.h"
#include <algorithm>

namespace PcmDeliveryPolicy
{
ChunkDecision EvaluateChunkIngest(
    const RequestContext& context,
    uint32_t bytesRead) noexcept
{
    switch (context.downstreamState)
    {
    case DownstreamState::Speaking:
    {
        uint32_t bytesToFrame = bytesRead;
        if (context.upstreamFinished)
        {
            if (context.rawAudioBytesRead >= context.upstreamTerminalBytes)
            {
                bytesToFrame = 0;
            }
            else
            {
                const uint64_t remaining = context.upstreamTerminalBytes - context.rawAudioBytesRead;
                bytesToFrame = static_cast<uint32_t>((std::min)(static_cast<uint64_t>(bytesRead), remaining));
            }
        }
        return { ChunkAction::AssembleSpeakingAudio, bytesToFrame, true, true };
    }

    case DownstreamState::Cancelling:
    {
        return { ChunkAction::DrainCancellationAudio, 0, true, true };
    }

    case DownstreamState::Idle:
    {
        return { ChunkAction::FaultUnexpectedIdleAudio, 0, false, false };
    }

    case DownstreamState::Faulted:
    {
        return { ChunkAction::DrainFaultedSessionAudio, 0, false, false };
    }
    }

    return { ChunkAction::DrainFaultedSessionAudio, 0, false, false };
}

SpanAdmission EvaluateSpanAdmission(
    const RequestContext& context,
    const RequestToken& batchToken) noexcept
{
    if (context.token.Matches(batchToken) &&
        context.downstreamState == DownstreamState::Speaking &&
        !context.faultPending)
    {
        return SpanAdmission::Allow;
    }

    return SpanAdmission::Reject;
}

BatchDecision EvaluateBatchOutcome(
    const RequestContext& context,
    const RequestToken& batchToken,
    uint64_t fullyAcceptedBytes,
    bool sapiWriteRejected) noexcept
{
    if (!context.token.Matches(batchToken))
    {
        return { BatchAction::IgnoreStaleOrInactive, 0 };
    }

    switch (context.downstreamState)
    {
    case DownstreamState::Speaking:
    {
        if (sapiWriteRejected)
        {
            return { BatchAction::CreditAndBeginCancellation, fullyAcceptedBytes };
        }
        return { BatchAction::CreditAndCheckBoundary, fullyAcceptedBytes };
    }

    case DownstreamState::Cancelling:
    {
        return { BatchAction::CheckCancellationBoundary, 0 };
    }

    case DownstreamState::Idle:
    case DownstreamState::Faulted:
    {
        return { BatchAction::IgnoreStaleOrInactive, 0 };
    }
    }

    return { BatchAction::IgnoreStaleOrInactive, 0 };
}

TerminalBoundaryFacts EvaluateTerminalBoundaryFacts(
    const RequestContext& context,
    bool assemblerHasCarry) noexcept
{
    const bool speakingAudioOverrun =
        context.rawAudioBytesRead > context.upstreamTerminalBytes ||
        context.deliveredAudioBytes > context.upstreamTerminalBytes;

    const bool speakingTerminalReached =
        context.rawAudioBytesRead == context.upstreamTerminalBytes &&
        context.deliveredAudioBytes == context.upstreamTerminalBytes &&
        !assemblerHasCarry;

    const bool cancellationTerminalReached =
        context.rawAudioBytesRead >= context.upstreamTerminalBytes;

    return { speakingAudioOverrun, speakingTerminalReached, cancellationTerminalReached };
}

bool IsTerminalByteCountFrameAligned(
    uint64_t bytes,
    uint16_t blockAlign) noexcept
{
    if (blockAlign == 0)
    {
        return false;
    }

    return (bytes % blockAlign) == 0;
}
} // namespace PcmDeliveryPolicy
