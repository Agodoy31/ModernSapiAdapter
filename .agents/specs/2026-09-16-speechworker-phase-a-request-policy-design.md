# SpeechWorker Phase A: Request-State Policy Design Specification

**Date:** 2026-09-16
**Target Unit:** `CoreEngine/SpeechStatePolicy.h`, `CoreEngine/SpeechStatePolicy.cpp`
**Adopting Class:** `CoreEngine/SpeechWorker` (`SpeechWorker.h`, `SpeechWorker.cpp`)
**Scope:** Specification of the stateless request-state decision policy, input types, owned typed decision contracts, caller mutation rules, assembler actions, condition-variable notifications, lock-hold invariants, and verification gates.

---

## 1. Architectural Principles and Boundary Guarantees

### 1.1 Non-Goals
1. No thread transfer or removal. Both `m_audioThread` and `m_controlThread` remain in `SpeechWorker`.
2. No mutex or condition-variable transfer. `m_requestMutex`, `m_eventForwardMutex`, and `m_requestChanged` remain private members of `SpeechWorker`.
3. No IPC or I/O migration. Named pipe reading, writing, cancellation commands, and handle management remain in `SpeechWorker` / `PipeClient`.
4. No COM or SAPI callback migration. `m_pEngine->OnSpeechEvent`, `m_pEngine->OnAudioData`, and `pOutputSite->GetActions()` remain in `SpeechWorker` / `CSapiEngine`.
5. No changes to timing budgets, cancellation deadlines (500ms), inactivity timeouts (1500ms), or poll intervals (250ms).

### 1.2 Pure Policy Invariants
The `SpeechStatePolicy` component is a pure functional Level-0 leaf unit conforming to the following invariants:
1. **Stateless:** The policy encapsulates no mutable or persistent internal state. All operations are static/free functions.
2. **Immutable Inputs:** Functions accept inputs by value or `const` reference to plain data structures (`RequestContext`, `RequestToken`, scalars, `std::string_view`). Inputs are never mutated.
3. **No Out-Parameters:** No function takes pointers or non-const references for writing outputs. All outputs are returned by value.
4. **Owned Typed Decisions:** Functions return owned, strongly-typed decision structs representing discrete semantic outcomes.
5. **Zero Allocation:** No heap allocation (`new`, `malloc`, `std::vector`, `std::string`) occurs in any policy path.
6. **No Retained Pointers/References:** Returned decision types hold zero pointers, raw references, or object lifetime handles to caller data.
7. **`noexcept` Execution:** Every policy function is explicitly marked `noexcept`.
8. **Bounded and Nonblocking:** Execution is strictly $O(1)$, non-iterative (or bounded small scalar arithmetic), and performs zero synchronization, sleeps, or waiting.
9. **Zero Side Effects:** The policy performs zero logging, COM invocations, named pipe I/O, clock reads, or condition-variable notifications.

### 1.3 Caller Lock-Hold Contract
`SpeechWorker` must strictly adhere to the continuous-lock-hold discipline when invoking `SpeechStatePolicy`:
1. `SpeechWorker` acquires `m_requestMutex`.
2. `SpeechWorker` captures policy inputs from its members (`m_context`, `m_frameAssembler`).
3. `SpeechWorker` executes the policy function synchronously under `m_requestMutex`.
4. `SpeechWorker` validates token matches / request identity where applicable.
5. `SpeechWorker` applies the returned field mutations to `m_context`, performs any required `m_frameAssembler` action, and triggers any condition-variable notification (`m_requestChanged.notify_all()`).
6. `SpeechWorker` releases `m_requestMutex`.
7. `SpeechWorker` executes any required post-unlock actions (such as `SendCancellation`, `EnterFaultedState`, or COM callbacks).

**Critical Rule:** No policy decision may be applied after releasing and reacquiring `m_requestMutex`. Evaluation, validation, and mutation must form one atomic critical section.

---

## 2. Policy Interface & Type Definitions

The proposed Level-0 header is `CoreEngine/SpeechStatePolicy.h` under namespace `SpeechStatePolicy`.

```cpp
/**
 * @file SpeechStatePolicy.h
 * @brief Pure Level-0 functional leaf policy evaluating SpeechWorker request state transitions.
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <unknwn.h>

#include "SpeechWorkerTypes.h"
#include "SpeechProtocolUtils.h"

namespace SpeechStatePolicy
{

// ============================================================================
// 1. Start Request Policy Types
// ============================================================================

enum class StartAction : uint8_t
{
    Reject = 0,
    Accept = 1
};

struct StartDecision
{
    StartAction action = StartAction::Reject;
    uint64_t speakId = 0;
    uint64_t generation = 0;
};

// ============================================================================
// 2. Terminal Boundary Evaluation Types
// ============================================================================

enum class TerminalBoundaryAction : uint8_t
{
    Continue = 0,             ///< Boundary not reached; continue streaming/draining.
    NormalCompleteReset = 1,  ///< Speaking terminal reached; reset request to Idle and wake waiters.
    CancelDrainReset = 2,     ///< Cancelling terminal reached; reset request to Idle and wake waiters.
    UtteranceFailedReset = 3, ///< Upstream failed; reset request to Idle and wake waiters.
    OverrunFault = 4          ///< Audio bytes exceeded provider declaration; flag protocol fault.
};

struct TerminalBoundaryDecision
{
    TerminalBoundaryAction action = TerminalBoundaryAction::Continue;
};

// ============================================================================
// 3. Begin Cancellation Policy Types
// ============================================================================

enum class BeginCancellationAction : uint8_t
{
    AlreadyIdle = 0,           ///< S_FALSE: Request is idle, nothing to cancel.
    AlreadyCancelling = 1,     ///< E_UNEXPECTED: Request is already cancelling.
    Faulted = 2,               ///< E_FAIL: Request or session is faulted.
    TransitionToCancelling = 3  ///< S_OK: Transition downstream to Cancelling.
};

struct BeginCancellationDecision
{
    BeginCancellationAction action = BeginCancellationAction::Faulted;
    uint64_t speakId = 0;
    ULONGLONG deadlineTick = 0;
    HRESULT hresult = E_FAIL;
};

// ============================================================================
// 4. Stop Request Policy Types
// ============================================================================

enum class StopAction : uint8_t
{
    NoAction = 0,
    ResetAndCancel = 1
};

struct StopDecision
{
    StopAction action = StopAction::NoAction;
    uint64_t speakId = 0;
};

// ============================================================================
// 5. Terminal Event Validation Types
// ============================================================================

enum class TerminalEventValidationAction : uint8_t
{
    DuplicateTerminalFault = 0,
    InvalidTerminalBytesFault = 1,
    MisalignedTerminalBytesFault = 2,
    ApplyTerminal = 3
};

struct TerminalEventDecision
{
    TerminalEventValidationAction action = TerminalEventValidationAction::InvalidTerminalBytesFault;
    UpstreamState targetUpstreamState = UpstreamState::Faulted;
    uint64_t terminalAudioBytes = 0;
};

// ============================================================================
// 6. Log Event Evaluation Types
// ============================================================================

enum class LogEventAction : uint8_t
{
    Ignore = 0,
    FatalSessionFault = 1,
    UtteranceError = 2
};

struct LogEventDecision
{
    LogEventAction action = LogEventAction::Ignore;
};

// ============================================================================
// 7. Audio Ingest Header Types
// ============================================================================

enum class AudioIngestAction : uint8_t
{
    DeliverAudio = 0,
    DiscardCancelledAudio = 1,
    UnexpectedAudioFault = 2,
    FaultedDrain = 3
};

struct AudioIngestHeaderDecision
{
    AudioIngestAction action = AudioIngestAction::UnexpectedAudioFault;
    size_t bytesToFrame = 0;
    bool shouldUpdateProgress = false;
};

// ============================================================================
// 8. Audio Delivery Result Types
// ============================================================================

enum class AudioDeliveryResultAction : uint8_t
{
    IgnoreStaleBatch = 0,
    DeliveryAccepted = 1,
    WriteRejectedTransitionToCancelling = 2,
    DrainCancelledAudio = 3,
    FaultedOrIdleNoop = 4
};

struct AudioDeliveryResultDecision
{
    AudioDeliveryResultAction action = AudioDeliveryResultAction::IgnoreStaleBatch;
    uint64_t cancellationToSend = 0;
    ULONGLONG cancellationDeadlineTick = 0;
};

// ============================================================================
// 9. Timeout Evaluation Types
// ============================================================================

enum class TimeoutCondition : uint8_t
{
    None = 0,
    CancellationTimeout = 1,
    InactivityTimeout = 2
};

struct TimeoutDecision
{
    TimeoutCondition condition = TimeoutCondition::None;
    uint64_t speakId = 0;
};

// ============================================================================
// Policy API Functions
// ============================================================================

[[nodiscard]] StartDecision EvaluateStart(
    const RequestContext& context,
    uint64_t speakId,
    uint64_t nextGeneration) noexcept;

[[nodiscard]] TerminalBoundaryDecision EvaluateTerminalBoundary(
    const RequestContext& context,
    bool hasCarry) noexcept;

[[nodiscard]] BeginCancellationDecision EvaluateBeginCancellation(
    const RequestContext& context,
    ULONGLONG cancellationDeadline) noexcept;

[[nodiscard]] StopDecision EvaluateStop(
    const RequestContext& context) noexcept;

[[nodiscard]] TerminalEventDecision EvaluateTerminalEvent(
    const RequestContext& context,
    ProviderEventType eventType,
    uint64_t terminalAudioBytes,
    bool hasValidTerminalBytes,
    size_t blockAlign) noexcept;

[[nodiscard]] LogEventDecision EvaluateLogEvent(
    std::string_view logSeverity) noexcept;

[[nodiscard]] AudioIngestHeaderDecision EvaluateAudioIngestHeader(
    const RequestContext& context,
    DWORD bytesRead) noexcept;

[[nodiscard]] AudioDeliveryResultDecision EvaluateAudioDeliveryResult(
    const RequestContext& context,
    const RequestToken& batchToken,
    bool writeAccepted,
    ULONGLONG cancellationDeadline) noexcept;

[[nodiscard]] TimeoutDecision EvaluateTimeouts(
    const RequestContext& context,
    ULONGLONG nowTick,
    ULONGLONG lastProgressTick,
    ULONGLONG inactivityTimeoutMs) noexcept;

[[nodiscard]] bool ShouldForwardEvent(
    const RequestContext& context,
    uint64_t speakId,
    bool isLog) noexcept;

[[nodiscard]] bool IsAudioDeliveryEligible(
    const RequestContext& context,
    const RequestToken& token) noexcept;

[[nodiscard]] bool IsWaitTerminal(
    const RequestContext& context,
    bool exitSignaled) noexcept;

} // namespace SpeechStatePolicy
```

---

## 3. Detailed Contract for Proposed Decisions

Each decision returned by `SpeechStatePolicy` is governed by exact preconditions, mutations, assembler actions, notifications, post-unlock actions, and exactly-once rules.

### 3.1 `EvaluateStart`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.upstreamState != UpstreamState::Idle || context.downstreamState != DownstreamState::Idle || context.faultPending`:
    - Returns `{ StartAction::Reject, 0, 0 }`.
  - Else:
    - Returns `{ StartAction::Accept, speakId, nextGeneration }`.
- **Caller Applied Mutations:**
  - On `Reject`: None. Returns `false`.
  - On `Accept`:
    - `m_context.Reset();`
    - `m_context.token.speakId = decision.speakId;`
    - `m_context.token.generation = decision.generation;`
    - `m_context.upstreamState = UpstreamState::Active;`
    - `m_context.downstreamState = DownstreamState::Speaking;`
    - `m_frameAssembler.Reset();`
    - `m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);`
    - Returns `true`.
- **Assembler Action:** `m_frameAssembler.Reset()` on `Accept`.
- **Notification:** None.
- **Post-Unlock Action:** None.
- **Exactly-Once Rule:** Generation is incremented only on `Accept`.

### 3.2 `EvaluateTerminalBoundary`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.upstreamState == UpstreamState::Failed`: return `UtteranceFailedReset`.
  - If `!context.upstreamFinished`: return `Continue`.
  - If `context.downstreamState == DownstreamState::Speaking`:
    - Overrun check: `context.rawAudioBytesRead > context.upstreamTerminalBytes || context.deliveredAudioBytes > context.upstreamTerminalBytes` -> return `OverrunFault`.
    - Terminal reached check: `context.rawAudioBytesRead == context.upstreamTerminalBytes && context.deliveredAudioBytes == context.upstreamTerminalBytes && !hasCarry` -> return `NormalCompleteReset`.
    - Else: return `Continue`.
  - If `context.downstreamState == DownstreamState::Cancelling`:
    - Cancellation terminal reached: `context.rawAudioBytesRead >= context.upstreamTerminalBytes` -> return `CancelDrainReset`.
    - Else: return `Continue`.
  - Default: return `Continue`.
- **Caller Applied Mutations:**
  - On `Continue`: No state mutation. Returns `false`.
  - On `NormalCompleteReset`, `CancelDrainReset`, or `UtteranceFailedReset`:
    - `ResetToIdleLocked()`:
      - `m_frameAssembler.Reset();`
      - `m_context.upstreamState = UpstreamState::Idle;`
      - `m_context.downstreamState = DownstreamState::Idle;`
      - `m_requestChanged.notify_all();`
    - Returns `false`.
  - On `OverrunFault`:
    - Returns `true` (caller sets `m_context.faultPending = true` and triggers `EnterFaultedState()` outside lock).
- **Assembler Action:** `m_frameAssembler.Reset()` upon all reset outcomes.
- **Notification:** `m_requestChanged.notify_all()` on reset outcomes.
- **Post-Unlock Action:** On `OverrunFault`, call `EnterFaultedState()`.
- **Exactly-Once Rule:** Boundary satisfaction executes exactly once per request when terminal byte equality is met.

### 3.3 `EvaluateBeginCancellation`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.downstreamState == DownstreamState::Idle`: return `{ AlreadyIdle, 0, 0, S_FALSE }`.
  - If `context.downstreamState == DownstreamState::Faulted || context.upstreamState == UpstreamState::Faulted`: return `{ Faulted, 0, 0, E_FAIL }`.
  - If `context.IsDrainingCancellation()`: return `{ AlreadyCancelling, 0, 0, E_UNEXPECTED }`.
  - Else (`downstreamState == DownstreamState::Speaking`): return `{ TransitionToCancelling, context.token.speakId, cancellationDeadline, S_OK }`.
- **Caller Applied Mutations:**
  - On `AlreadyIdle`, `AlreadyCancelling`, `Faulted`: No mutation.
  - On `TransitionToCancelling`:
    - `m_context.TransitionToCancelling(decision.deadlineTick);`
    - `m_frameAssembler.Reset();`
    - `speakId = decision.speakId;`
- **Assembler Action:** `m_frameAssembler.Reset()` on `TransitionToCancelling`.
- **Notification:** None.
- **Post-Unlock Action:**
  - If `TransitionToCancelling`: `FinishCancellation` proceeds with `SendCancellation` and wait.
- **Exactly-Once Rule:** Only a request in `Speaking` state can transition to `Cancelling`.

### 3.4 `EvaluateStop`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.downstreamState == DownstreamState::Idle || context.downstreamState == DownstreamState::Faulted || context.upstreamState == UpstreamState::Faulted`: return `{ StopAction::NoAction, 0 }`.
  - Else: return `{ StopAction::ResetAndCancel, context.token.speakId }`.
- **Caller Applied Mutations:**
  - On `NoAction`: None.
  - On `ResetAndCancel`:
    - `m_frameAssembler.Reset();`
    - `m_context.upstreamState = UpstreamState::Idle;`
    - `m_context.downstreamState = DownstreamState::Idle;`
    - `m_requestChanged.notify_all();`
- **Assembler Action:** `m_frameAssembler.Reset()` on `ResetAndCancel`.
- **Notification:** `m_requestChanged.notify_all()` on `ResetAndCancel`.
- **Post-Unlock Action:** If `ResetAndCancel`, call `SendCancellation(decision.speakId, CancellationTimeoutMs)`.
- **Exactly-Once Rule:** Idempotent; subsequent calls observe `downstreamState == Idle` and take `NoAction`.

### 3.5 `EvaluateTerminalEvent`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.upstreamFinished`: return `{ DuplicateTerminalFault, UpstreamState::Faulted, 0 }`.
  - If `!hasValidTerminalBytes`: return `{ InvalidTerminalBytesFault, UpstreamState::Faulted, 0 }`.
  - If `blockAlign == 0 || terminalAudioBytes % blockAlign != 0`: return `{ MisalignedTerminalBytesFault, UpstreamState::Faulted, 0 }`.
  - Else:
    - `targetState = (eventType == ProviderEventType::SynthesisComplete ? UpstreamState::Completed : UpstreamState::Cancelled)`.
    - Return `{ ApplyTerminal, targetState, terminalAudioBytes }`.
- **Caller Applied Mutations:**
  - On Fault actions:
    - `TransitionRequestToFaultedLocked();`
    - Returns `true` (caller notes fault required).
  - On `ApplyTerminal`:
    - `m_context.upstreamTerminalBytes = decision.terminalAudioBytes;`
    - `m_context.upstreamFinished = true;`
    - `m_context.upstreamState = decision.targetUpstreamState;`
    - Invokes `EvaluateTerminalBoundary(m_context, m_frameAssembler.HasCarry())` and applies boundary result.
- **Assembler Action:** None directly; boundary evaluation may trigger `Reset()`.
- **Notification:** `m_requestChanged.notify_all()` if boundary reached or faulted.
- **Post-Unlock Action:** `EnterFaultedState()` if faulted or overrun.
- **Exactly-Once Rule:** Duplicate terminal events unconditionally fault the request.

### 3.6 `EvaluateLogEvent`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `logSeverity == "fatal"`: return `{ LogEventAction::FatalSessionFault }`.
  - If `logSeverity == "error"`: return `{ LogEventAction::UtteranceError }`.
  - Else: return `{ LogEventAction::Ignore }`.
- **Caller Applied Mutations:**
  - On `Ignore`: None. Returns `false`.
  - On `FatalSessionFault`: Returns `true` (caller sets `faultPending = true` and triggers `EnterFaultedState()`).
  - On `UtteranceError`:
    - `m_context.upstreamState = UpstreamState::Failed;`
    - `m_context.upstreamFinished = true;`
    - `m_context.completionHr = E_FAIL;`
    - `m_context.upstreamTerminalBytes = m_context.rawAudioBytesRead;`
    - Invokes `EvaluateTerminalBoundary` -> yields `UtteranceFailedReset` -> caller executes `ResetToIdleLocked()`. Returns `false`.
- **Assembler Action:** `m_frameAssembler.Reset()` on `UtteranceError`.
- **Notification:** `m_requestChanged.notify_all()` on `UtteranceError`.
- **Post-Unlock Action:** `EnterFaultedState()` on `FatalSessionFault`.
- **Exactly-Once Rule:** Provider logs with error severity fail only the active utterance without corrupting the session.

### 3.7 `EvaluateAudioIngestHeader`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.downstreamState == DownstreamState::Speaking`:
    - `bytesToFrame = bytesRead`.
    - If `context.upstreamFinished`:
      - `remaining = context.upstreamTerminalBytes > context.rawAudioBytesRead ? context.upstreamTerminalBytes - context.rawAudioBytesRead : 0`.
      - `bytesToFrame = min(bytesRead, remaining)`.
    - Return `{ DeliverAudio, bytesToFrame, true }`.
  - If `context.downstreamState == DownstreamState::Cancelling`:
    - Return `{ DiscardCancelledAudio, 0, true }`.
  - If `context.downstreamState == DownstreamState::Idle`:
    - Return `{ UnexpectedAudioFault, 0, false }`.
  - If `context.downstreamState == DownstreamState::Faulted`:
    - Return `{ FaultedDrain, 0, false }`.
- **Caller Applied Mutations:**
  - If `shouldUpdateProgress`: caller updates `m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release)`.
  - On `DeliverAudio`:
    - `m_context.rawAudioBytesRead += bytesRead;`
    - Caller passes `bytesToFrame` to `m_frameAssembler.Process(...)`.
    - If output spans empty: evaluates `EvaluateTerminalBoundary` and applies.
  - On `DiscardCancelledAudio`:
    - `m_context.rawAudioBytesRead += bytesRead;`
    - Caller evaluates `EvaluateTerminalBoundary` and applies.
  - On `UnexpectedAudioFault`:
    - Caller sets `protocolBoundaryFailed = true` and `m_context.faultPending = true`.
- **Assembler Action:** Caller feeds `bytesToFrame` to `m_frameAssembler.Process(...)`.
- **Notification:** `m_requestChanged.notify_all()` if boundary reached during empty span check.
- **Post-Unlock Action:** `EnterFaultedState()` if protocol boundary failed.
- **Exactly-Once Rule:** Audio arriving in `Idle` state is strictly rejected as a protocol boundary violation.

### 3.8 `EvaluateAudioDeliveryResult`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `!context.token.Matches(batchToken)`: return `{ IgnoreStaleBatch, 0, 0 }`.
  - If `context.downstreamState == DownstreamState::Speaking`:
    - If `!writeAccepted`:
      - Return `{ WriteRejectedTransitionToCancelling, context.token.speakId, cancellationDeadline }`.
    - Else:
      - Return `{ DeliveryAccepted, 0, 0 }`.
  - If `context.downstreamState == DownstreamState::Cancelling`:
    - Return `{ DrainCancelledAudio, 0, 0 }`.
  - Default: return `{ FaultedOrIdleNoop, 0, 0 }`.
- **Caller Applied Mutations:**
  - On `IgnoreStaleBatch` or `FaultedOrIdleNoop`: None.
  - On `WriteRejectedTransitionToCancelling`:
    - `m_context.deliveredAudioBytes += deliveredBytes;`
    - `m_context.TransitionToCancelling(decision.cancellationDeadlineTick);`
    - `m_frameAssembler.Reset();`
    - Caller captures `outCancellationToSend = decision.cancellationToSend`.
  - On `DeliveryAccepted`:
    - `m_context.deliveredAudioBytes += deliveredBytes;`
    - Caller evaluates `EvaluateTerminalBoundary` and applies result.
  - On `DrainCancelledAudio`:
    - Caller evaluates `EvaluateTerminalBoundary` and applies result.
- **Assembler Action:** `m_frameAssembler.Reset()` on write rejection.
- **Notification:** `m_requestChanged.notify_all()` if boundary reached.
- **Post-Unlock Action:**
  - If `cancellationToSend != 0`: caller calls `SendCancellation(cancellationToSend, CancellationTimeoutMs)`.
  - If boundary evaluation failed (overrun): caller calls `EnterFaultedState()`.
- **Exactly-Once Rule:** Write rejection initiates cancellation exactly once; subsequent spans in batch are discarded.

### 3.9 `EvaluateTimeouts`
- **Precondition:** Caller holds `m_requestMutex`.
- **Logic:**
  - If `context.IsDrainingCancellation() && HasCancellationTimedOut(nowTick, context.cancellationDeadlineTick)`:
    - Return `{ CancellationTimeout, context.token.speakId }`.
  - If `(context.IsActivelySynthesizing() || context.IsAwaitingTerminalAudio()) && HasSynthesisInactivityTimedOut(nowTick, lastProgressTick, inactivityTimeoutMs)`:
    - Return `{ InactivityTimeout, context.token.speakId }`.
  - Else: return `{ None, 0 }`.
- **Caller Applied Mutations:** None under lock.
- **Assembler Action:** None.
- **Notification:** None under lock.
- **Post-Unlock Action:**
  - If `CancellationTimeout` or `InactivityTimeout`: caller unlocks `m_requestMutex`, calls `EnterFaultedState()`, and returns `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`.
- **Exactly-Once Rule:** First timeout evaluation that expires triggers session quarantine.

### 3.10 Helper Query Predicates
- `ShouldForwardEvent(context, speakId, isLog)`:
  - If `speakId != context.token.speakId`: return `false`.
  - If `isLog`: return `true`.
  - If `context.faultPending`: return `false`.
  - Return `context.downstreamState == DownstreamState::Speaking`.
- `IsAudioDeliveryEligible(context, token)`:
  - Return `context.token.Matches(token) && context.downstreamState == DownstreamState::Speaking && !context.faultPending`.
- `IsWaitTerminal(context, exitSignaled)`:
  - Return `context.downstreamState == DownstreamState::Idle || context.downstreamState == DownstreamState::Faulted || exitSignaled`.

---

## 4. Concurrency & Lock Order Guarantees

1. **Only Permitted Production Lock Order:**
   $$\text{acquire } m\_eventForwardMutex \implies \text{acquire } m\_requestMutex \implies \text{release } m\_requestMutex \implies \text{release } m\_eventForwardMutex$$
   Reverse acquisition ($m\_requestMutex \implies m\_eventForwardMutex$) is **strictly forbidden**.
2. **Policy Evaluation Invariant:**
   All `SpeechStatePolicy` evaluations are synchronous, nonblocking, and execute while holding `m_requestMutex`.
3. **No Locks Held Across External Boundaries:**
   Neither `m_requestMutex` nor `m_eventForwardMutex` may be held during:
   - Pipe I/O (`ReadAudioChunk`, `ReadControlMessage`, `SendControlMessage`, `Cancel`).
   - SAPI callbacks (`OnSpeechEvent`, `OnAudioData`, `GetActions`).
   - Condition variable waits (`m_requestChanged.wait_for(...)`).
   - Thread joins (`join()`).

---

## 5. Verification Gate Criteria

1. **Unit Test Coverage (`CoreEngine.Tests/SpeechStatePolicyTests.cpp`):**
   - 100% path coverage of every policy function.
   - Comprehensive test cases for every action in each decision enum.
   - Tests validating boundary equality, byte overrun, block alignment mismatch, carry buffer impact, and timeout arithmetic.
2. **Integration Equivalence:**
   - Existing real named pipe tests (`SpeakWaitsForSynthesisCompleteByteBoundary`, `RealControlPipeBoundaryAndBookmarkEndToEnd`) execute unchanged and pass.
   - SAPI abort and write-rejection tests (`OutputSiteAbortCancelsTheActiveRequest`, `RejectedAudioWriteDrainsCancellationBeforeNextSpeak`) pass cleanly.
3. **Watchdog Verification:**
   - All tests execute under 30-second external watchdogs.
4. **Platform Hygiene:**
   - Strict x64 build (ARM64 exempt per documented rule; x86 prohibited).
   - Strict Allman formatting across all files.
   - `git diff --check` emits zero diagnostics.
