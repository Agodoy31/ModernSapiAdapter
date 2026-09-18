#pragma once

#include <cstdint>

#include "SpeechWorkerTypes.h"

namespace SpeechStatePolicy
{
enum class StartAction : std::uint8_t
{
    Reject = 0,
    Accept = 1
};

struct StartDecision
{
    StartAction action = StartAction::Reject;
    std::uint64_t speakId = 0;
    std::uint64_t generation = 0;
};

enum class UpstreamTerminalKind : std::uint8_t
{
    Completed = 0,
    Cancelled = 1
};

enum class UpstreamTerminalAction : std::uint8_t
{
    Apply = 0,
    DuplicateFault = 1,
    InvalidBytesFault = 2,
    MisalignedBytesFault = 3
};

struct UpstreamTerminalDecision
{
    UpstreamTerminalAction action = UpstreamTerminalAction::DuplicateFault;
    UpstreamState targetState = UpstreamState::Faulted;
    std::uint64_t terminalAudioBytes = 0;
};

enum class RequestFailureAction : std::uint8_t
{
    ApplyUtteranceFailure = 0
};

struct RequestFailureDecision
{
    RequestFailureAction action = RequestFailureAction::ApplyUtteranceFailure;
    std::uint64_t terminalAudioBytes = 0;
};

struct TerminalBoundaryFacts
{
    bool speakingAudioOverrun = false;
    bool speakingTerminalReached = false;
    bool cancellationTerminalReached = false;
};

enum class TerminalBoundaryAction : std::uint8_t
{
    Continue = 0,
    NormalCompleteReset = 1,
    CancelDrainReset = 2,
    UtteranceFailedReset = 3,
    OverrunFault = 4
};

struct TerminalBoundaryDecision
{
    TerminalBoundaryAction action = TerminalBoundaryAction::Continue;
};

enum class BeginCancellationAction : std::uint8_t
{
    AlreadyIdle = 0,
    AlreadyCancelling = 1,
    Faulted = 2,
    TransitionToCancelling = 3
};

struct BeginCancellationDecision
{
    BeginCancellationAction action = BeginCancellationAction::Faulted;
    std::uint64_t speakId = 0;
    std::uint64_t deadlineTick = 0;
};

enum class StopAction : std::uint8_t
{
    NoAction = 0,
    ResetAndCancel = 1
};

struct StopDecision
{
    StopAction action = StopAction::NoAction;
    std::uint64_t speakId = 0;
};

enum class TimeoutCondition : std::uint8_t
{
    None = 0,
    CancellationTimeout = 1,
    InactivityTimeout = 2
};

struct TimeoutDecision
{
    TimeoutCondition condition = TimeoutCondition::None;
    std::uint64_t speakId = 0;
};

[[nodiscard]] StartDecision EvaluateStart(
    const RequestContext& context,
    std::uint64_t speakId,
    std::uint64_t nextGeneration) noexcept;

[[nodiscard]] UpstreamTerminalDecision EvaluateUpstreamTerminal(
    const RequestContext& context,
    UpstreamTerminalKind kind,
    std::uint64_t terminalAudioBytes,
    bool hasValidTerminalBytes,
    bool isFrameAligned) noexcept;

[[nodiscard]] RequestFailureDecision EvaluateUtteranceFailure(
    const RequestContext& context) noexcept;

[[nodiscard]] TerminalBoundaryDecision EvaluateTerminalBoundary(
    const RequestContext& context,
    const TerminalBoundaryFacts& facts) noexcept;

[[nodiscard]] BeginCancellationDecision EvaluateBeginCancellation(
    const RequestContext& context,
    std::uint64_t cancellationDeadlineTick) noexcept;

[[nodiscard]] StopDecision EvaluateStop(
    const RequestContext& context) noexcept;

[[nodiscard]] TimeoutDecision EvaluateTimeouts(
    const RequestContext& context,
    std::uint64_t nowTick,
    std::uint64_t lastProgressTick,
    std::uint64_t inactivityTimeoutMs) noexcept;

[[nodiscard]] bool IsWaitTerminal(
    const RequestContext& context,
    bool exitSignaled) noexcept;
}
