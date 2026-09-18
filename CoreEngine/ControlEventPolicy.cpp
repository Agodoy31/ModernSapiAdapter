#include "pch.h"
#include "ControlEventPolicy.h"

namespace ControlEventPolicy
{
EventDecision EvaluateParsedEvent(
    const ProviderControlEvent& event,
    const RequestContext& context) noexcept
{
    const bool p0 = (context.upstreamState != UpstreamState::Faulted &&
                     context.downstreamState != DownstreamState::Faulted &&
                     !context.faultPending);

    if (event.speakId != context.token.speakId)
    {
        return { EventAction::NoLockedAction, false, p0 };
    }

    switch (event.type)
    {
        case ProviderEventType::WordBoundary:
        case ProviderEventType::SentenceBoundary:
        case ProviderEventType::Bookmark:
        {
            if (!event.hasValidSpeechOffsets)
            {
                return { EventAction::MalformedSpeechBoundary, false, false };
            }

            const bool refresh = (context.downstreamState == DownstreamState::Speaking ||
                                  context.downstreamState == DownstreamState::Cancelling);
            const bool forward = (context.downstreamState == DownstreamState::Speaking && p0);
            return { EventAction::SpeechBoundary, refresh, forward };
        }

        case ProviderEventType::SynthesisComplete:
        {
            const bool refresh = (event.hasValidTerminalBytes &&
                                  (context.downstreamState == DownstreamState::Speaking ||
                                   context.downstreamState == DownstreamState::Cancelling));
            return { EventAction::SynthesisComplete, refresh, p0 };
        }

        case ProviderEventType::SynthesisCancelled:
        {
            const bool refresh = (event.hasValidTerminalBytes &&
                                  (context.downstreamState == DownstreamState::Speaking ||
                                   context.downstreamState == DownstreamState::Cancelling));
            return { EventAction::SynthesisCancelled, refresh, p0 };
        }

        case ProviderEventType::LegacyCompleted:
        {
            return { EventAction::LegacyCompleted, false, p0 };
        }

        case ProviderEventType::Log:
        {
            EventAction action = EventAction::InformationalLog;
            if (event.logSeverity == "error")
            {
                action = EventAction::RequestErrorLog;
            }
            else if (event.logSeverity == "fatal")
            {
                action = EventAction::FatalLog;
            }
            return { action, false, p0 };
        }

        case ProviderEventType::Unknown:
        default:
        {
            return { EventAction::Unknown, false, p0 };
        }
    }
}

FinalAdmission EvaluateFinalAdmission(
    const ProviderControlEvent& event,
    const RequestContext& context) noexcept
{
    if (event.speakId != context.token.speakId)
    {
        return FinalAdmission::Reject;
    }

    if (event.type == ProviderEventType::Log)
    {
        return FinalAdmission::Allow;
    }

    if (context.downstreamState == DownstreamState::Speaking && !context.faultPending)
    {
        return FinalAdmission::Allow;
    }

    return FinalAdmission::Reject;
}
}
