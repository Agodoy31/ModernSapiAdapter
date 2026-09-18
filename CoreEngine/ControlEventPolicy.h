#pragma once

#include <cstdint>
#include "SpeechEventTypes.h"
#include "SpeechWorkerTypes.h"

namespace ControlEventPolicy
{
    enum class EventAction : std::uint8_t
    {
        NoLockedAction,
        SpeechBoundary,
        MalformedSpeechBoundary,
        SynthesisComplete,
        SynthesisCancelled,
        LegacyCompleted,
        InformationalLog,
        RequestErrorLog,
        FatalLog,
        Unknown
    };

    struct EventDecision
    {
        EventAction action = EventAction::NoLockedAction;
        bool shouldRefreshProgress = false;
        bool shouldForwardToSapi = false;
    };

    enum class FinalAdmission : std::uint8_t
    {
        Reject,
        Allow
    };

    [[nodiscard]] EventDecision EvaluateParsedEvent(
        const ProviderControlEvent& event,
        const RequestContext& context) noexcept;

    [[nodiscard]] FinalAdmission EvaluateFinalAdmission(
        const ProviderControlEvent& event,
        const RequestContext& context) noexcept;
}
