# SpeechWorker Phase A: Request-State Policy Design Specification

**Risk tier:** Tier 3 — concurrent request lifecycle and SAPI/IPC coordination are affected, although observable behavior and ownership remain unchanged.

## 1. Objective and Scope

Phase A extracts only deterministic request-lifecycle decisions from `SpeechWorker` into a pure leaf unit named `SpeechStatePolicy`. It does not extract event classification or PCM delivery policy.

`SpeechWorker` remains the sole owner of:

- `m_audioThread` and `m_controlThread`;
- `m_requestMutex`, `m_eventForwardMutex`, and `m_requestChanged`;
- `m_context`, `m_frameAssembler`, `m_generationCounter`, progress and fault atomics;
- all named-pipe reads, writes, cancellation, and session quarantine;
- `CSapiEngine::OnAudioData`, `CSapiEngine::OnSpeechEvent`, `GetActions`, and all COM calls;
- every debug test hook and its current placement.

### 1.1 Phase boundary

Phase A owns:

- request admission;
- lifecycle start, stop, cancellation, reset, and fault decisions;
- application of already-classified upstream completion, cancellation, and request failure;
- selection between cancellation and inactivity timeout outcomes;
- lifecycle response to terminal-boundary facts computed by the caller;
- wait-terminal classification.

Phase B retains responsibility for control-event identity/category/severity classification, progress eligibility, provisional callback admission, and final callback revalidation.

Phase C retains responsibility for frame alignment, terminal-byte validation and arithmetic, PCM span eligibility, stale-span classification, and SAPI-write result classification.

Therefore Phase A does **not** define `EvaluateLogEvent`, `EvaluateAudioIngestHeader`, `EvaluateAudioDeliveryResult`, `ShouldForwardEvent`, or `IsAudioDeliveryEligible`. It does not accept `ProviderEventType`, raw JSON, PCM pointers, frame spans, or SAPI write results.

### 1.2 Behavior-preservation rule

The extraction preserves current state mutation, return values, notifications, callback timing, cancellation timing, and fault publication. In particular:

- `ResetToIdleLocked()` continues to reset only the assembler and the two lifecycle enums. It does not clear the token, counters, terminal fields, deadline, `upstreamFinished`, or `completionHr`.
- `TransitionToCancelling()` continues to clear `upstreamFinished` and terminal bytes without normalizing the retained upstream enum.
- invalid or duplicate terminal events retain the existing two-stage fault path: the handler performs `TransitionRequestToFaultedLocked()`, then post-lock `EnterFaultedState()` repeats the idempotent transition before publishing fault visibility.
- no new fixed wait, timer, queue, allocation, logging call, callback, or I/O operation is introduced.

## 2. Pure Policy Contract

Every policy function:

- accepts immutable values or `const` references;
- has no output parameter and retains no pointer or reference;
- returns an owned typed decision;
- is `noexcept`, stateless, allocation-free, bounded O(1), and nonblocking;
- performs no clock read, lock, wait, notification, logging, I/O, COM operation, or callback.

The implementation may include `SpeechWorkerTypes.h` for the existing lifecycle enums, request token, and request context. It must not include `SpeechProtocolUtils.h`, `nlohmann/json.hpp`, `sapi.h`, or other event/parsing/COM machinery directly. Timeout arithmetic is implemented from the supplied integer ticks.

## 3. Caller Synchronization Contract

For every policy call, `SpeechWorker` holds `m_requestMutex` continuously across:

1. capture of the current request state and caller-computed facts;
2. policy evaluation;
3. current `RequestToken` validation when a token participates;
4. application of the returned decision and all associated field mutations.

The lock is not released and reacquired between evaluation and application. Notifications may occur while locked as in the baseline. Logging, IPC, COM callbacks, condition-variable waits, and thread joins occur only after the required state lock has been released, except for pre-existing locked logging explicitly catalogued as baseline debt.

The only permitted nested production order remains `m_eventForwardMutex` followed by `m_requestMutex`. Phase A introduces no lock.

## 4. Proposed Interface

`CoreEngine/SpeechStatePolicy.h` is a self-contained internal header under namespace `SpeechStatePolicy`.

```cpp
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
```

`EvaluateUtteranceFailure` intentionally has no severity input. The existing handler, and later Phase B, decide whether a provider log represents an utterance error. Phase A only captures the lifecycle effect and current raw-byte boundary after that classification.

## 5. Decision Contracts

### 5.1 `EvaluateStart`

- **Precondition:** caller holds `m_requestMutex`.
- **Decision:** reject unless both lifecycle enums are Idle and `faultPending` is false; otherwise accept the supplied `speakId` and `nextGeneration`.
- **Apply on accept:** caller performs full `m_context.Reset()`, assigns both token fields, increments `m_generationCounter` exactly once to the accepted generation, resets the assembler, writes the progress clock, and sets `Active/Speaking`.
- **Post-lock effect:** none.
- **Return behavior:** `SpeechWorker::Start` returns true only for Accept.

### 5.2 `EvaluateUpstreamTerminal`

- **Precondition:** caller has already matched request identity and classified the event as complete or cancelled. While continuously holding `m_requestMutex`, existing Phase C-owned code computes `hasValidTerminalBytes` and `isFrameAligned` but does not act on them before policy evaluation.
- **Decision precedence:** if `upstreamFinished` is already true, return DuplicateFault even when the new event also has missing or misaligned bytes. Otherwise missing/invalid bytes return InvalidBytesFault, failed alignment returns MisalignedBytesFault, and valid facts return Apply with `Completed` or `Cancelled` and the supplied byte count. This preserves the baseline duplicate-first diagnostic path without moving byte arithmetic into Phase A.
- **Apply on success:** assign `upstreamTerminalBytes`, set `upstreamFinished=true`, assign the target upstream state, then compute the current terminal-boundary facts using the existing `SpeechWorker` arithmetic helpers and apply `EvaluateTerminalBoundary`.
- **Apply on any fault action:** emit the action-specific existing diagnostic, call `TransitionRequestToFaultedLocked()` immediately, and mark the post-lock fault action. Preserve the subsequent idempotent transition and second notification performed by `EnterFaultedState()`.
- **Post-lock effect:** `EnterFaultedState()` on any terminal fault action or a downstream OverrunFault.
- **Exactly once:** only the first accepted terminal event mutates terminal fields.

### 5.3 `EvaluateUtteranceFailure`

- **Precondition:** caller has already matched request identity and classified an event as request-scoped severity `error`.
- **Decision:** return ApplyUtteranceFailure with `terminalAudioBytes=context.rawAudioBytesRead`.
- **Apply:** set `upstreamState=Failed`, `upstreamFinished=true`, `completionHr=E_FAIL`, and `upstreamTerminalBytes=decision.terminalAudioBytes`; then pass the caller-computed terminal facts to `EvaluateTerminalBoundary`, which returns UtteranceFailedReset because the upstream state takes precedence.
- **Post-lock effect:** the existing log callback remains outside the state lock.
- **Exactly once:** one classified request error produces one request failure/reset sequence.

### 5.4 `EvaluateTerminalBoundary`

- **Precondition:** caller holds `m_requestMutex`. All three facts were computed from the same locked state using existing `HasSpeakingAudioOverrunLocked`, `IsSpeakingTerminalReachedLocked`, and `IsCancellingTerminalReachedLocked` logic. Phase A does not compute bytes, alignment, or carry state.
- **Decision order:**
  1. `upstreamState == Failed` -> UtteranceFailedReset.
  2. `!upstreamFinished` -> Continue, even when the retained upstream enum is Completed or Cancelled during cancellation.
  3. downstream Speaking and `facts.speakingAudioOverrun` -> OverrunFault.
  4. downstream Speaking and `facts.speakingTerminalReached` -> NormalCompleteReset.
  5. downstream Cancelling and `facts.cancellationTerminalReached` -> CancelDrainReset.
  6. Otherwise -> Continue.
- **Apply reset actions:** call the existing `ResetToIdleLocked()` exactly once. Do not replace it with a full `RequestContext::Reset()`.
- **Apply fault action:** set `faultPending` and invoke `EnterFaultedState()` after unlocking.

### 5.5 `EvaluateBeginCancellation`

- **Precondition:** caller holds `m_requestMutex` and supplies one already-read deadline tick.
- **Decision order:** Idle -> AlreadyIdle; Faulted lifecycle -> Faulted; draining cancellation -> AlreadyCancelling; otherwise -> TransitionToCancelling with current `speakId` and deadline.
- **HRESULT mapping remains in `SpeechWorker`:** AlreadyIdle -> `S_FALSE`; AlreadyCancelling -> `E_UNEXPECTED`; Faulted -> `E_FAIL`; TransitionToCancelling -> `S_OK`.
- **Apply transition:** call `m_context.TransitionToCancelling(deadlineTick)` and reset the assembler. Do not normalize the retained upstream enum.
- **Post-lock effect for caller-driven `CancelAndDrain`/SAPI abort:** after the existing debug hook and unlock, call `FinishCancellation`. It sends with only the remaining deadline budget and then synchronously waits for the declared cancellation boundary.
- **Post-lock effect for write rejection:** after the audio loop unlocks, send exactly one cancellation with the existing fresh `CancellationTimeoutMs` argument. Do not call `FinishCancellation` and do not synchronously wait in the audio worker; ordinary request draining and `WaitUntilFinished` retain responsibility for completion.
- **Exactly once:** both callers apply the transition once. A successful TransitionToCancelling decision causes exactly one send in that caller's existing post-lock path; AlreadyIdle, AlreadyCancelling, and Faulted cause no send.

### 5.6 `EvaluateStop`

- **Precondition:** caller holds `m_requestMutex`.
- **Decision:** return NoAction for downstream Idle or either faulted lifecycle enum; otherwise return ResetAndCancel with current `speakId`.
- **Apply:** call existing `ResetToIdleLocked()`; do not clear retained context fields.
- **Post-lock effect:** call `SendCancellation(speakId, CancellationTimeoutMs)` exactly once and preserve its ignored return value.

### 5.7 `EvaluateTimeouts`

- **Precondition:** caller holds `m_requestMutex` and has loaded `nowTick` and `lastProgressTick` once for this iteration.
- **Decision order:** cancellation expiry takes precedence over inactivity expiry. Cancellation expiry requires `context.IsDrainingCancellation()`, a nonzero deadline, and `nowTick >= deadline`; an expired deadline retained in Idle must not trigger. Inactivity expiry applies only while actively synthesizing or awaiting terminal audio and only when monotonic subtraction is valid (`nowTick >= lastProgressTick`).
- **Apply:** no mutation while locked. Capture the decision’s `speakId`, unlock, preserve the existing diagnostic message for the selected timeout, invoke `EnterFaultedState()`, and return `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`.
- **Exactly once:** the first expired decision exits `WaitUntilFinished`; fault publication remains CAS-protected.

### 5.8 `IsWaitTerminal`

Return true when downstream is Idle, downstream is Faulted, or `exitSignaled` is true. This remains a pure predicate used by the condition-variable loop.

## 6. Integration Mapping

| Existing `SpeechWorker` location | Phase A call | Logic that explicitly remains outside Phase A |
|---|---|---|
| `Start` | `EvaluateStart` | clock read and field application |
| `BeginCancellationLocked` | `EvaluateBeginCancellation` | HRESULT mapping, debug logging, assembler mutation |
| `Stop` | `EvaluateStop` | cancellation IPC |
| `HandleTerminalEventLocked` | `EvaluateUpstreamTerminal`, then `EvaluateTerminalBoundary` | event classification, byte presence, frame alignment, diagnostic logging |
| `HandleLogEventLocked` after severity classification | `EvaluateUtteranceFailure`, then `EvaluateTerminalBoundary` | severity classification and logging |
| `CheckTerminalBoundaryLocked` | caller computes three terminal facts, then `EvaluateTerminalBoundary` | byte/carry arithmetic remains for Phase C |
| `WaitUntilFinished` | `EvaluateTimeouts`, `IsWaitTerminal` | 10ms wait, clock/atomic reads, SAPI `GetActions`, logging, fault publication |
| `UpdateAfterAudioDeliveryLocked` | existing code calls `EvaluateBeginCancellation` only after it classifies write rejection | token/span eligibility, byte credit, and write-result classification remain for Phase C |

No Phase A call is added to event forwarding, audio ingest, frame assembly, pipe reads, or COM callback bodies.

## 6.1 Deterministic inactive payloads

Every decision field is deterministic and directly asserted by tests:

- Start Reject returns zero `speakId` and generation; Accept returns supplied values.
- Upstream Apply returns its target state and supplied terminal bytes. Duplicate/Invalid/Misaligned fault actions return target `Faulted` and zero terminal bytes.
- Utterance failure returns current `rawAudioBytesRead`.
- Begin-cancellation Transition returns current `speakId` and supplied deadline. All non-transition actions return zero identity and deadline.
- Stop ResetAndCancel returns current `speakId`; NoAction returns zero.
- Timeout expiry returns current `speakId`; None returns zero.
- Terminal-boundary decisions and `IsWaitTerminal` contain no inactive payload.

## 7. Verification Gate

The implementation must provide:

1. Direct unit tests for every action and precedence rule in this specification, including duplicate-plus-malformed terminal precedence, retained Completed-to-Cancelling state, and an expired cancellation deadline retained while Idle.
2. Characterization tests proving `ResetToIdleLocked()` does not become a full reset.
3. A regression covering the current valid `synthesis_cancelled`-while-Speaking behavior.
4. Existing real-pipe completion, cancellation, timeout, write-rejection, request-error, and callback-boundary tests unchanged and passing.
5. Debug and Release x64 complete suites under external 30-second watchdogs with an asserted selected-test count and worktree-owned MockProvider quiescence.
6. Fresh independent concurrency/state and contract/execution reviews after all corrections. The original two approvals are superseded because they did not detect scope drift, reachable-state errors, or non-executable verification commands.

Phase A may not begin implementation until this corrected package passes the pre-code gate.
