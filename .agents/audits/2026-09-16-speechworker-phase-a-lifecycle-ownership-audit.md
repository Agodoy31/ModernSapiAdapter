# SpeechWorker Phase A: Request Lifecycle and Ownership Audit

**Date:** 2026-09-16
**Target Component:** `CoreEngine/SpeechWorker` (`SpeechWorker.h`, `SpeechWorker.cpp`, `SpeechWorkerTypes.h`)
**Scope:** Complete inventory of request lifecycle state, ownership boundaries, access sites, synchronization discipline, composite state matrix, state transitions, lock ordering, callback boundaries, and teardown/blocking paths for Phase A (`SpeechStatePolicy` extraction).

---

## 1. Executive Summary

This audit establishes the baseline for Phase A of the `SpeechWorker` incremental decomposition program. In Phase A, `SpeechWorker` retains sole ownership of:
1. Both background worker threads (`m_audioThread`, `m_controlThread`).
2. All synchronization primitives (`m_requestMutex`, `m_eventForwardMutex`, `m_requestChanged`).
3. Mutable request context (`m_context`), frame assembler (`m_frameAssembler`), and generation counter (`m_generationCounter`).
4. Named pipe IPC handles and operations (`m_pClient`).
5. SAPI engine callbacks (`m_pEngine->OnSpeechEvent`, `m_pEngine->OnAudioData`, `pOutputSite->GetActions()`).
6. Session fault quarantine (`EnterFaultedState()`).

The goal of Phase A is to extract deterministic, pure decision policies into a standalone leaf unit (`SpeechStatePolicy`). The policy will be stateless, `noexcept`, allocation-free, nonblocking, and bounded, accepting immutable inputs and returning owned typed decisions. `SpeechWorker` must hold `m_requestMutex` continuously across input capture, policy evaluation, token validation, and decision application.

---

## 2. State & Variable Inventory

The following table catalogues every piece of state within `SpeechWorker` involved in request execution, lifecycle progression, framing, timeouts, and fault publication.

| Variable | Type | Invariant / Range | Access Discipline | Primary Thread(s) | Lifecycle Role |
|---|---|---|---|---|---|
| `m_context.token` | `RequestToken` (`speakId`, `generation`) | Valid iff `speakId != 0 && generation != 0`. `Reset()` clears only `speakId`; `generation` and the complete token remain retained after `ResetToIdleLocked()`. | Protected by `m_requestMutex`. | SAPI caller, Audio worker, Control worker | Identifies active request and guards against ABA recycling across restarts. |
| `m_context.upstreamState` | `UpstreamState` enum | `Idle`, `Active`, `Completed`, `Cancelled`, `Failed`, `Faulted`. | Protected by `m_requestMutex`. | SAPI caller, Control worker, Audio worker | Tracks provider-side synthesis lifecycle. |
| `m_context.downstreamState` | `DownstreamState` enum | `Idle`, `Speaking`, `Cancelling`, `Faulted`. | Protected by `m_requestMutex`. | SAPI caller, Audio worker, Control worker | Tracks SAPI-side audio delivery lifecycle. |
| `m_context.rawAudioBytesRead` | `uint64_t` | Monotonically increases during request. Reset to 0 on new request. | Protected by `m_requestMutex`. | Audio worker, SAPI caller (audit/cancel) | Total raw PCM bytes read from audio pipe for this request. |
| `m_context.deliveredAudioBytes`| `uint64_t` | Monotonically increases during `Speaking`. Before a terminal declaration it normally exceeds the still-zero `upstreamTerminalBytes`. After a terminal byte count is accepted, any excess is detected as an overrun during boundary evaluation. | Protected by `m_requestMutex`. | Audio worker | Total PCM bytes delivered to `pOutputSite->Write()`. |
| `m_context.upstreamTerminalBytes` | `uint64_t` | Provider terminal values are frame-aligned before acceptance. Error-log handling copies `rawAudioBytesRead` and immediately resets lifecycle enums without clearing this field. | Protected by `m_requestMutex`. | Control worker, Audio worker | Exact declared terminal byte boundary from provider, or retained diagnostic residue after reset. |
| `m_context.upstreamFinished` | `bool` | Set by terminal events and request-error logs. `ResetToIdleLocked()` does not clear it; `Start()` and cancellation explicitly reset it. | Protected by `m_requestMutex`. | Control worker, Audio worker | Signals upstream termination while a request is active and remains retained diagnostic state while idle. |
| `m_context.faultPending` | `bool` | True when protocol/boundary error detected but session quarantine publication incomplete. | Protected by `m_requestMutex`. | Audio worker, Control worker, SAPI caller | Inhibits new request admission and SAPI event forwarding. |
| `m_context.cancellationDeadlineTick` | `ULONGLONG` | Monotonic tick (`GetTickCount64() + CancellationTimeoutMs`) while cancelling. Retained after `ResetToIdleLocked()` and cleared by the next full `RequestContext::Reset()`. | Protected by `m_requestMutex`. | SAPI caller, Audio worker | Absolute deadline for provider cancellation acknowledgement and PCM drain. |
| `m_context.completionHr` | `HRESULT` | `S_OK`, `E_FAIL`, `E_ABORT`, or Win32 error code. Retained after `ResetToIdleLocked()` so the waiter can observe the completed request result. | Protected by `m_requestMutex`. | SAPI caller, Control worker, Audio worker | Terminal HRESULT returned by `WaitUntilFinished` / `CancelAndDrain`. |
| `m_frameAssembler` | `PcmFrameAssembler` | Holds partial PCM frames (`< blockAlign`). Reset on request transitions. | Protected by `m_requestMutex`. | Audio worker, SAPI caller (`Start`, `Stop`, `Cancel`) | Assembles complete PCM frames across arbitrary pipe read boundaries. |
| `m_generationCounter` | `uint64_t` | Monotonically increments on every successful `Start()`. Never wraps practically. | Protected by `m_requestMutex`. | SAPI caller (`Start()`) | Allocates unique generation tokens to prevent ABA request confusion. |
| `m_lastProviderProgressTick` | `std::atomic<ULONGLONG>` | Monotonic tick of last valid audio or control progress. | Atomic (release store, acquire load). | Audio worker, Control worker, SAPI caller | Inactivity watchdog clock; prevents SAPI caller hangs on silent provider stall. |
| `m_exit` | `std::atomic_bool` | False initially. True on destructor or thread creation rollback. | Atomic (store/load). | SAPI caller (destruction), Audio worker, Control worker | Signals worker threads to exit loops and cease pipe operations. |
| `m_faultPublicationStarted` | `std::atomic_bool` | False initially. CAS to true in `EnterFaultedState()`. | Atomic CAS. | Audio worker, Control worker, SAPI caller | Guarantees single-publication of session quarantine across concurrent errors. |
| `m_faultVisible` | `std::atomic_bool` | False initially. Set true under `m_eventForwardMutex`. | Atomic (release store, acquire load). | SAPI caller, Control worker, Test hooks | Fast-path gate preventing event callbacks once session fault is visible. |
| `m_requestMutex` | `std::mutex` | Serializes request context, framing, and generation. | Native mutex. | SAPI caller, Audio worker, Control worker | Core state lock. NEVER held during pipe I/O or COM callbacks. |
| `m_eventForwardMutex` | `std::mutex` | Serializes event callback admission against session fault publication. | Native mutex. | Control worker, Fault publisher | Outer lock. Only permitted nested order is `m_eventForwardMutex` then `m_requestMutex`. |
| `m_requestChanged` | `std::condition_variable` | Signaled on terminal transitions (`ResetToIdleLocked`, `TransitionRequestToFaultedLocked`). | Associated with `m_requestMutex`. | SAPI caller (waiter), Audio/Control workers (notifiers) | Wakes synchronous `WaitUntilFinished` and `FinishCancellation` callers. |

---

## 3. Comprehensive Access-Site Audit

Every read and write site of request lifecycle state is mapped below:

### 3.1 `SpeechWorker::Start(uint64_t speakId)`
- **Thread:** SAPI Caller Thread.
- **Lock:** Holds `m_requestMutex` (exclusive `lock_guard`).
- **Reads:** `m_context.upstreamState`, `m_context.downstreamState`, `m_context.faultPending`.
- **Precondition for admission:** `upstreamState == Idle && downstreamState == Idle && !faultPending`.
- **Writes on admission:**
  - `m_context.Reset()`
  - `m_context.token.speakId = speakId`
  - `m_context.token.generation = ++m_generationCounter`
  - `m_frameAssembler.Reset()`
  - `m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release)`
  - `m_context.upstreamState = UpstreamState::Active`
  - `m_context.downstreamState = DownstreamState::Speaking`
- **Return Value:** `true` if admitted, `false` if rejected.
- **Notifications:** None.

### 3.2 `SpeechWorker::Stop()`
- **Thread:** SAPI Caller Thread.
- **Lock:** Holds `m_requestMutex` during decision and state mutation; releases before `SendCancellation`.
- **Reads:** `m_context.downstreamState`, `m_context.upstreamState`, `m_context.token.speakId`.
- **Writes on active request:**
  - `m_frameAssembler.Reset()`
  - `m_context.upstreamState = UpstreamState::Idle`
  - `m_context.downstreamState = DownstreamState::Idle`
- **Notifications under lock:** `m_requestChanged.notify_all()`.
- **Action outside lock:** If active, calls `SendCancellation(speakId, CancellationTimeoutMs)`.

### 3.3 `SpeechWorker::CancelAndDrain()` & `BeginCancellationLocked()`
- **Thread:** SAPI Caller Thread.
- **Lock:** Holds `m_requestMutex` during `BeginCancellationLocked`.
- **Reads:** `m_context.downstreamState`, `m_context.upstreamState`, `m_context.IsDrainingCancellation()`, `m_context.token.speakId`.
- **Writes in `BeginCancellationLocked`:**
  - `m_context.TransitionToCancelling(cancellationDeadline)`:
    - `m_context.upstreamFinished = false`
    - `m_context.upstreamTerminalBytes = 0`
    - `m_context.cancellationDeadlineTick = cancellationDeadline`
    - `m_context.downstreamState = DownstreamState::Cancelling`
  - `m_frameAssembler.Reset()`
- **Return from locked phase:** `S_FALSE` if already Idle; `E_FAIL` if Faulted; `E_UNEXPECTED` if already Cancelling; `S_OK` if transitioned.
- **Action outside lock:** `FinishCancellation`:
  - Sends cancellation IPC message via `SendCancellation(speakId, sendTimeout)`.
  - Reacquires `m_requestMutex` via `std::unique_lock`.
  - Waits on `m_requestChanged` until `!m_context.IsDrainingCancellation() || m_exit.load()`.
  - Evaluates cancellation timeout. If timed out, unlocks and calls `EnterFaultedState()`.

### 3.4 `SpeechWorker::WaitUntilFinished(ISpTTSEngineSite* pOutputSite)`
- **Thread:** SAPI Caller Thread.
- **Lock:** Holds `m_requestMutex` (via `std::unique_lock`), unlocks around 10ms wait loop iterations, SAPI `GetActions()`, timeout fault entry, and cancellation finish.
- **Reads under lock:**
  - `IsWaitTerminalLocked()`: `downstreamState == Idle || downstreamState == Faulted || m_exit.load()`.
  - `m_context.IsAwaitingTerminalAudio()`: `upstreamFinished && downstreamState == Speaking`.
  - `m_context.IsDrainingCancellation()`: `downstreamState == Cancelling`.
  - `m_context.cancellationDeadlineTick`.
  - `m_lastProviderProgressTick`.
  - `m_context.downstreamState`.
  - `m_context.token.speakId`.
  - `m_context.completionHr`.
- **SAPI Abort Polling:**
  - Unlocks `m_requestMutex`.
  - Calls `pOutputSite->GetActions()`.
  - Re-locks `m_requestMutex`.
  - Checks `IsAbortRequested(actions) && downstreamState == Speaking`.
  - If abort requested, calls `BeginCancellationLocked`, unlocks, and calls `FinishCancellation`.
- **Inactivity Timeout:**
  - If `(isActivelySynthesizing || isAwaitingTerminalAudio) && HasSynthesisInactivityTimedOut(...)`:
  - Unlocks `m_requestMutex`, calls `EnterFaultedState()`, returns `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`.

### 3.5 `SpeechWorker::IsFaulted()`
- **Thread:** SAPI Caller Thread.
- **Lock:** Holds `m_requestMutex`.
- **Reads:** `m_context.downstreamState == Faulted || m_context.upstreamState == Faulted || m_context.faultPending`.

### 3.6 `SpeechWorker::EnterFaultedState()`
- **Thread:** Any thread (Audio worker, Control worker, SAPI caller).
- **Lock Phase 1:** Holds `m_requestMutex`.
  - `m_frameAssembler.Reset()`
  - `TransitionRequestToFaultedLocked()`:
    - `m_context.upstreamState = UpstreamState::Faulted`
    - `m_context.downstreamState = DownstreamState::Faulted`
    - `m_context.completionHr = E_FAIL`
    - `m_requestChanged.notify_all()`
  - If CAS `m_faultPublicationStarted` from `false` to `true`:
    - `m_context.faultPending = true`
- **Lock Phase 2 (Fault Publication):**
  - Acquires `m_eventForwardMutex`.
  - Sets `m_faultVisible.store(true, std::memory_order_release)`.
  - Acquires nested `m_requestMutex`.
  - Sets `m_context.faultPending = false`.
  - Releases both locks.
- **Action outside lock:** Calls `m_pClient->Cancel()`.

### 3.7 `SpeechWorker::ForwardEventToSapi(const ProviderControlEvent& event)`
- **Thread:** Control Worker Thread.
- **Lock Phase:**
  - Acquires `m_eventForwardMutex`.
  - Checks `m_faultVisible.load(std::memory_order_acquire)`. If true, drops event immediately.
  - If `event.speakId == 0`, drops event.
  - Acquires nested `m_requestMutex`.
  - Checks `ShouldForwardEventLocked(event.speakId, isLog)`:
    - `speakId == m_context.token.speakId`
    - If `isLog`: returns `true`.
    - If `m_context.faultPending`: returns `false`.
    - Returns `m_context.downstreamState == DownstreamState::Speaking`.
  - Releases `m_requestMutex` and `m_eventForwardMutex`.
- **Action outside lock:** Calls `m_pEngine->OnSpeechEvent(event)`.

### 3.8 `SpeechWorker::IngestAudioChunkLocked(const uint8_t* pChunkData, DWORD bytesRead)`
- **Thread:** Audio Worker Thread.
- **Lock:** Holds `m_requestMutex`.
- **Reads:** `m_context.token`, `m_context.downstreamState`, `m_context.upstreamFinished`, `m_context.upstreamTerminalBytes`, `m_context.rawAudioBytesRead`.
- **Writes:**
  - If `Speaking` or `Cancelling`: updates `m_lastProviderProgressTick`.
  - If `Speaking`:
    - Clamps `bytesToFrame` if `upstreamFinished` to `upstreamTerminalBytes - rawAudioBytesRead`.
    - `m_context.rawAudioBytesRead += bytesRead`.
    - Runs `m_frameAssembler.Process(pChunkData, bytesToFrame)`.
    - If batch empty, calls `CheckTerminalBoundaryLocked()`.
  - If `Cancelling`:
    - `m_context.rawAudioBytesRead += bytesRead`.
    - Calls `CheckTerminalBoundaryLocked()`.
  - If `Idle`:
    - Unsolicited audio received -> flags `protocolBoundaryFailed = true`.
  - If `protocolBoundaryFailed`: sets `m_context.faultPending = true`.

### 3.9 `SpeechWorker::UpdateAfterAudioDeliveryLocked(...)`
- **Thread:** Audio Worker Thread.
- **Lock:** Holds `m_requestMutex`.
- **Token Validation:** If `!m_context.token.Matches(batchToken)`, ignores stale audio without state mutation.
- **Writes:**
  - If `Speaking`:
    - `m_context.deliveredAudioBytes += deliveredBytes`.
    - If `!writeAccepted` (SAPI rejected write):
      - Sets `outCancellationToSend = m_context.token.speakId`.
      - Calls `m_context.TransitionToCancelling(...)`.
      - Calls `m_frameAssembler.Reset()`.
    - If `writeAccepted`:
      - Calls `CheckTerminalBoundaryLocked()`.
  - If `Cancelling`:
    - Calls `CheckTerminalBoundaryLocked()`.
  - If `protocolBoundaryFailed`: sets `m_context.faultPending = true`.

### 3.10 `SpeechWorker::CheckTerminalBoundaryLocked()`
- **Thread:** Audio Worker or Control Worker Thread.
- **Lock:** Holds `m_requestMutex`.
- **Logic:**
  - If `upstreamState == UpstreamState::Failed`: calls `ResetToIdleLocked()`, returns `false`.
  - If `!upstreamFinished`: returns `false`.
  - If `downstreamState == DownstreamState::Speaking`:
    - Overrun check (`rawAudioBytesRead > upstreamTerminalBytes || deliveredAudioBytes > upstreamTerminalBytes`): returns `true` (caller flags protocol failure).
    - Terminal reached (`rawAudioBytesRead == upstreamTerminalBytes && deliveredAudioBytes == upstreamTerminalBytes && !m_frameAssembler.HasCarry()`): calls `ResetToIdleLocked()`, returns `false`.
    - Otherwise returns `false` (still draining).
  - If `downstreamState == DownstreamState::Cancelling`:
    - Terminal reached (`rawAudioBytesRead >= upstreamTerminalBytes`): calls `ResetToIdleLocked()`, returns `false`.
    - Otherwise returns `false`.
  - If `Idle` or `Faulted`: returns `false`.

### 3.11 `SpeechWorker::HandleTerminalEventLocked(...)`
- **Thread:** Control Worker Thread.
- **Lock:** Holds `m_requestMutex`.
- **Validation:**
  - If `m_context.upstreamFinished`: duplicate terminal -> `TransitionRequestToFaultedLocked()`, returns `true`.
  - If `!hasValidTerminalBytes`: invalid terminal bytes -> `TransitionRequestToFaultedLocked()`, returns `true`.
  - If `terminalAudioBytes % m_frameAssembler.BlockAlign() != 0`: misaligned terminal bytes -> `TransitionRequestToFaultedLocked()`, returns `true`.
- **Writes on Success:**
  - `m_context.upstreamTerminalBytes = terminalAudioBytes`
  - `m_context.upstreamFinished = true`
  - `m_context.upstreamState = (eventType == SynthesisComplete ? Completed : Cancelled)`
  - Calls `CheckTerminalBoundaryLocked()`.

### 3.12 `SpeechWorker::HandleLogEventLocked(...)`
- **Thread:** Control Worker Thread.
- **Lock:** Holds `m_requestMutex`.
- **Severity Handling:**
  - If `severity == "fatal"`: returns `true` (caller faults session).
  - If `severity == "error"`:
    - `m_context.upstreamState = UpstreamState::Failed`
    - `m_context.upstreamFinished = true`
    - `m_context.completionHr = E_FAIL`
    - `m_context.upstreamTerminalBytes = m_context.rawAudioBytesRead`
    - Calls `CheckTerminalBoundaryLocked()` (which resets to Idle).
    - Returns `false`.
  - Other severities (info, warn): returns `false`.

### 3.13 Debug-only test-hook state that Phase A must preserve

The following `_DEBUG` state is not production policy input, but its placement relative to request-state mutation is observable by the concurrency tests and therefore part of the extraction contract:

| Hook state | Production boundary observed | Required Phase A placement |
|---|---|---|
| `pauseNextAbortTransition`, `abortTransitionPaused`, `wasCancellingAtAbortUnlock` and `abortTransitionChanged` | The request has transitioned to `Cancelling` under `m_requestMutex`, then the request lock is released before cancellation IPC. | Keep the pause after the locked transition has been applied and after `m_requestMutex` is released. Do not move it into policy code. |
| `pauseNextEventForward`, `eventForwardPaused` and `eventForwardChanged` | Event admission has been serialized against fault publication before the final SAPI callback. | Keep it in `ForwardEventToSapi`; Phase A does not own event admission or callback timing. |
| `pauseNextFaultPublication`, `faultPublicationPaused` and `faultPublicationChanged` | The first fault transition is visible and `m_faultPublicationStarted` has won its compare/exchange, but `m_faultVisible` has not yet been published. | Keep it between the two existing fault-publication phases. Phase A decisions must not absorb this pause. |
| `failNextFrameAssembly` | The audio thread is about to call `PcmFrameAssembler::Process`. | Keep it in the audio-ingest path. Phase A does not own framing or exception injection. |
| `audioApartmentActive` | COM apartment lifetime of the audio worker thread. | Keep it in `AudioThreadProc`; Phase A has no thread or apartment ownership. |
| Static control-thread creation and entry failure injectors | Worker startup rollback and control-thread exception containment. | Keep them in thread startup/entry code; Phase A does not change worker construction or rollback. |

No Phase A policy function reads or writes these hooks. Any implementation that moves a hook across its observed mutation, unlock, callback, or fault-publication boundary is a behavior change and must stop for a separate review.

---

## 4. Observed Composite-State Matrix

The lifecycle decision tuple is `C = (UpstreamState, DownstreamState, upstreamFinished, faultPending)`. It is not the complete stored state: token fields, byte counters, terminal bytes, cancellation deadline, and `completionHr` can intentionally remain populated after lifecycle enums return to Idle. The matrix records states the current implementation can actually expose under `m_requestMutex`; it does not normalize residual fields.

| State ID | `UpstreamState` | `DownstreamState` | `upstreamFinished` | `faultPending` | Classification | Invariant & Meaning |
|---|---|---|---|---|---|---|
| **S0a** | `Idle` | `Idle` | `false` | `false` | **Legal (fresh/quiescent)** | Initial state or a newly accepted request before state publication. `Start()` performs a full `RequestContext::Reset()` before entering S1. |
| **S0b** | `Idle` | `Idle` | `true` or `false` | `false` | **Legal (completed residue)** | `ResetToIdleLocked()` changes only lifecycle enums and resets the assembler. Token generation, counters, terminal fields, deadline, and `completionHr` remain available to the waiting caller until the next `Start()`. |
| **S1** | `Active` | `Speaking` | `false` | `false` | **Legal (synthesizing)** | Normal synthesis underway. Audio is framed and delivered to SAPI. |
| **S2** | `Completed` | `Speaking` | `true` | `false` | **Legal (awaiting terminal audio)** | Normal completion arrived before all declared PCM was read and delivered. |
| **S3a** | `Active` | `Cancelling` | `false` | `false` | **Legal (draining cancel)** | Cancellation began while synthesis was active. |
| **S3b** | `Completed` | `Cancelling` | `false` | `false` | **Legal (cancel after completion declaration)** | Cancellation began while S2 was waiting for terminal PCM. `TransitionToCancelling()` clears `upstreamFinished` and terminal bytes but intentionally retains `upstreamState == Completed`. |
| **S3c** | `Cancelled` | `Cancelling` | `false` | `false` | **Representable retained state** | A repeated cancellation transition after a previously observed cancelled lifecycle can retain the enum while clearing `upstreamFinished`; callers must not assume terminal enums imply `upstreamFinished` during cancellation. |
| **S4** | `Cancelled` | `Cancelling` | `true` | `false` | **Legal (cancel boundary received)** | Provider acknowledged cancellation; audio is discarded until `rawAudioBytesRead >= upstreamTerminalBytes`. |
| **S5** | `Completed` | `Cancelling` | `true` | `false` | **Legal (completion raced cancellation)** | Provider completed before processing cancel; remaining declared audio is discarded. |
| **S6** | `Cancelled` | `Speaking` | `true` | `false` | **Protocol-unexpected but representable** | Current code accepts a matching `synthesis_cancelled` even while Speaking. It either resets immediately or waits for its declared byte boundary. Phase A preserves this baseline; Phase B may later classify whether it should fault. |
| **S7** | `Failed` | `Speaking` or `Cancelling` | `true` | `false` | **Transient (utterance error)** | A classified request error is applied and immediately evaluated into S0b. |
| **S8** | Any | Any | Any | `true` | **Transient (fault pending)** | A protocol/boundary error has been detected but fault publication is not complete. Start and event forwarding are inhibited. |
| **S9** | `Faulted` | `Faulted` | Any | `true` then `false` | **Legal (session quarantine)** | `EnterFaultedState()` first publishes faulted lifecycle state with `faultPending=true`, then serializes callback admission, sets `m_faultVisible`, and clears `faultPending`. |
| **U1** | `Idle` | `Speaking` or `Cancelling` | Any | `false` | **Not produced by current transitions** | No current transition assigns an active downstream state while leaving upstream Idle. |
| **U2** | `Active` | `Idle` | Any | `false` | **Not produced by current transitions** | Active synthesis is paired with Speaking or Cancelling. |
| **U3** | Any non-faulted upstream | `Faulted` | Any | `false` | **Not produced by current transitions** | Fault publication assigns both lifecycle enums together. |

`upstreamFinished == false` is therefore not a universal proof that `upstreamState == Active`; cancellation deliberately clears the flag without normalizing the retained upstream enum. Phase A tests must cover S3b explicitly.

---

## 5. Transition and Effect Table

This table maps every state transition, its precondition, atomic effects, notifications, and post-unlock actions.

| Transition Name | Trigger / Input | Precondition | Mutated State Fields | Assembler Action | Notification | Post-Unlock Action | Destination State |
|---|---|---|---|---|---|---|---|
| `StartRequest` | `SpeechWorker::Start(speakId)` | `upstreamState == Idle && downstreamState == Idle && !faultPending` | Full `RequestContext::Reset()`, then `token.speakId = speakId`, `token.generation = ++m_generationCounter`, `upstreamState = Active`, `downstreamState = Speaking` | `Reset()` | None | None | **S1** (`Active`, `Speaking`) |
| `RejectStart` | `SpeechWorker::Start(speakId)` | `upstreamState != Idle || downstreamState != Idle || faultPending` | None | None | None | None | No change |
| `TerminalComplete` | `synthesis_complete` control event | `token.Matches() && !upstreamFinished && hasValidTerminalBytes && (terminalBytes % blockAlign == 0)` | `upstreamTerminalBytes = terminalBytes`, `upstreamFinished = true`, `upstreamState = Completed` | None | Evaluates boundary; if reached, notifies `m_requestChanged` | If boundary reached: None; if overrun: `EnterFaultedState()` | **S2** (if draining), **S0b** (if reached), or **S8/S9** (if overrun) |
| `TerminalCancelled`| `synthesis_cancelled` control event | `token.Matches() && !upstreamFinished && hasValidTerminalBytes && (terminalBytes % blockAlign == 0)` | `upstreamTerminalBytes = terminalBytes`, `upstreamFinished = true`, `upstreamState = Cancelled` | None | Evaluates boundary; if reached, notifies `m_requestChanged` | If boundary reached: None | **S4** when cancelling, **S6** when still speaking and bytes remain, or **S0b** when reached |
| `DuplicateTerminal`| Terminal event with `upstreamFinished == true` | `token.Matches() && upstreamFinished == true` | First calls `TransitionRequestToFaultedLocked()`; caller then marks `faultPending` and invokes `EnterFaultedState()`, which repeats the idempotent lifecycle transition before publishing fault visibility | None | One notification at initial transition and one during fault entry, preserving baseline | `EnterFaultedState()` | **S8/S9** |
| `InvalidTerminalBytes` | First terminal event with missing or nonnumeric byte field | `token.Matches() && !upstreamFinished && !hasValidTerminalBytes` | Same two-stage fault transition as duplicate terminal | None | Same two-stage notification behavior | `EnterFaultedState()` | **S8/S9**. If a terminal is both duplicate and malformed, `DuplicateTerminal` wins by baseline precedence. |
| `MisalignedTerminal`| Terminal event with `terminalBytes % blockAlign != 0` | `token.Matches()` | Same two-stage fault transition as duplicate terminal | None | Same two-stage notification behavior | `EnterFaultedState()` | **S8/S9** |
| `SapiAbortRequested`| SAPI `pOutputSite->GetActions()` includes `SPVES_ABORT` | `downstreamState == Speaking` | `downstreamState = Cancelling`, `upstreamFinished = false`, `upstreamTerminalBytes = 0`, `cancellationDeadlineTick = deadline`; retained upstream enum is not normalized | `Reset()` | None | `SendCancellation(speakId, timeout)` outside lock | **S3a**, **S3b**, or **S3c** |
| `WriteRejected` | SAPI `OnAudioData` returns `false` | `token.Matches() && downstreamState == Speaking` | Same cancellation transition as SAPI abort; retained upstream enum is not normalized | `Reset()` | None | `SendCancellation(speakId, timeout)` outside lock | **S3a**, **S3b**, or **S3c** |
| `TerminalAudioDrain`| Ingest / delivery reaches `deliveredAudioBytes == declared && rawAudioBytes == declared && !HasCarry()` | `upstreamFinished && downstreamState == Speaking` | Only `upstreamState = Idle`, `downstreamState = Idle`; all other context fields are retained | `Reset()` | `m_requestChanged.notify_all()` | None | **S0b** |
| `CancelDrainComplete`| Ingest reaches `rawAudioBytesRead >= upstreamTerminalBytes` | `upstreamFinished && downstreamState == Cancelling` | Only `upstreamState = Idle`, `downstreamState = Idle`; all other context fields are retained | `Reset()` | `m_requestChanged.notify_all()` | None | **S0b** |
| `AudioOverrun` | Ingest / delivery results in `raw > declared || delivered > declared` | `upstreamFinished && downstreamState == Speaking` | `faultPending = true` | None | None | `EnterFaultedState()` | **S8/S9** |
| `UtteranceErrorLog` | `severity == "error"` log event | `token.Matches()` | `upstreamState = Failed`, `upstreamFinished = true`, `completionHr = E_FAIL`, `upstreamTerminalBytes = rawAudioBytesRead` -> immediately `ResetToIdleLocked()` | `Reset()` | `m_requestChanged.notify_all()` | Forward log event outside lock | **S7** then **S0b** |
| `FatalLog` | `severity == "fatal"` log event | Any | `faultPending = true` | None | None | `EnterFaultedState()` | **S8/S9** |
| `SessionQuarantine` | Transport failure, pipe crash, or timeout | Any | `upstreamState = Faulted`, `downstreamState = Faulted`, `completionHr = E_FAIL`; fault publication toggles `faultPending` and publishes `m_faultVisible` | `Reset()` | `m_requestChanged.notify_all()` | `m_pClient->Cancel()` | **S9** |
| `StopRequest` | `SpeechWorker::Stop()` | `downstreamState != Idle && downstreamState != Faulted` | Only `upstreamState = Idle`, `downstreamState = Idle`; all other context fields are retained | `Reset()` | `m_requestChanged.notify_all()` | `SendCancellation(speakId, timeout)` | **S0b** |

---

## 6. Current Lock-Ordering Table

The established lock-ordering discipline is strictly hierarchical. Reverse acquisition is strictly forbidden and would produce deadlock.

| Lock Name | Native Type | Protected Resources | Acquisition Context | Permitted Nested Locks |
|---|---|---|---|---|
| `m_eventForwardMutex` | `std::mutex` | Serializes event callback admission against session fault publication. | Control worker (`ForwardEventToSapi`), Fault publisher (`EnterFaultedState`). | MAY acquire `m_requestMutex` while holding `m_eventForwardMutex`. |
| `m_requestMutex` | `std::mutex` | `m_context`, `m_frameAssembler`, `m_generationCounter`. | Audio worker, Control worker, SAPI caller. | MUST NOT acquire another **production** mutex. In Debug builds only, the existing abort-observation hook briefly acquires `m_testHooks.abortTransitionMutex` while `m_requestMutex` remains held so the test can observe the atomic transition boundary. |
| Test Hook Mutexes (`_DEBUG` only) | `std::mutex` (`eventForwardMutex`, `abortTransitionMutex`, `faultPublicationMutex`) | Test pause and synchronization flags. | Test harness and worker threads. | Normally independent. The one intentional exception is `m_requestMutex` -> debug `abortTransitionMutex` immediately after cancellation state publication. Phase A preserves this debug-only placement and introduces no new nesting. |

**Inviolable Rule:** `m_eventForwardMutex` -> `m_requestMutex` is the only valid nesting among production locks. Reverse acquisition (`m_requestMutex` -> `m_eventForwardMutex`) is strictly prohibited. The existing Debug-only abort test hook is not a production lock-order alternative and must remain at its characterized boundary.

---

## 7. Callback Boundary Table

To maintain low latency and eliminate deadlocks with external COM components and named pipes, no external callback, COM call, or IPC I/O is permitted while holding `m_requestMutex`.

| Boundary Operation | Target Component | Lock State Required | Rationale |
|---|---|---|---|
| `m_pEngine->OnSpeechEvent(event)` | `CSapiEngine` / SAPI Site | BOTH `m_eventForwardMutex` and `m_requestMutex` MUST BE RELEASED | SAPI's `ISpTTSEngineSite::AddEvents` may re-enter or block on UI threads. Holding locks causes deadlock. |
| `m_pEngine->OnAudioData(span, size)`| `CSapiEngine` / SAPI Site | `m_requestMutex` MUST BE RELEASED | SAPI's `ISpTTSEngineSite::Write` can block on the audio rendering device. |
| `pOutputSite->GetActions()` | SAPI Site COM Interface | `m_requestMutex` MUST BE RELEASED | COM method invocation across apartment boundaries must never hold engine locks. |
| `m_pClient->ReadAudioChunk(...)` | Win32 Named Pipe | NO LOCK HELD | Blocking overlapped read; holding locks would starve control events and SAPI calls. |
| `m_pClient->ReadControlMessage(...)`| Win32 Named Pipe | NO LOCK HELD | Blocking overlapped read; holding locks would starve audio delivery. |
| `m_pClient->SendControlMessage(...)`| Win32 Named Pipe | NO LOCK HELD | Pipe write with timeout; must not block state queries. |
| `m_pClient->Cancel()` | Win32 Named Pipe | NO LOCK HELD | Cancels pending overlapped I/O on provider pipes. |

---

## 8. Teardown and Blocking-Path Inventory

Every path that waits, polls, or blocks in `SpeechWorker` is inventoried below with its bounding guarantee.

| Path / Method | Waiting / Blocking Mechanism | Bounding Guarantee | Failure / Exit Action |
|---|---|---|---|
| `SpeechWorker::~SpeechWorker()` | Sets `m_exit = true`, releases test hooks, calls `m_pClient->Cancel()`, then performs ordinary unbounded `join()` calls. | Pipe-blocked threads are normally released by cancellation or the 250ms read poll. The destructor is **not globally bounded**: an audio worker blocked inside `OnAudioData`/SAPI `Write`, or a control worker blocked inside `OnSpeechEvent`/SAPI `AddEvents`, is not released by pipe cancellation. | Existing baseline liveness debt under `CON-03`. Phase A neither moves nor worsens callback/thread ownership and must not claim to solve this path. Ownership transfer remains blocked until a separate design supplies an enforceable callback/join bound. |
| Audio pipe read | `m_pClient->ReadAudioChunk` with 250ms poll interval. | Maximum 250ms per loop iteration, or immediate on `m_pClient->Cancel()`. | Exits on `m_exit` or enters fault on I/O failure. |
| Control pipe read | `m_pClient->ReadControlMessage` with 250ms poll interval. | Maximum 250ms per loop iteration, or immediate on `m_pClient->Cancel()`. | Exits on `m_exit` or enters fault on I/O failure. |
| `FinishCancellation` Wait | `m_requestChanged.wait_for(lock, remaining, ...)` | Bounded by `CancellationTimeoutMs` (500ms). | If timeout expires, unlocks and calls `EnterFaultedState()`, returning `ERROR_TIMEOUT`. |
| `WaitUntilFinished` Loop | `m_requestChanged.wait_for(lock, 10ms, ...)` | The individual condition-variable wait has 10ms poll granularity. Cancellation drain has a 500ms deadline, and 1500ms of no eligible provider progress triggers inactivity quarantine. This is **not** a global completion bound: valid audio/control progress can refresh the inactivity clock indefinitely. | On detected inactivity or cancellation timeout, unlocks and calls `EnterFaultedState()`, returning `ERROR_TIMEOUT`. Otherwise a legitimately long progressive request may remain active without an absolute deadline. |
| SAPI `GetActions()` | External COM call once per nonterminal wait-loop iteration after releasing `m_requestMutex`. | No CoreEngine-enforced deadline; a blocked SAPI site can block the caller independently of provider progress timers. | Existing baseline liveness debt. Phase A preserves unlock-before-call behavior and does not claim to bound it. |
| `SendCancellation` IPC | Overlapped pipe write with `sendTimeout` (up to 500ms). | Caller-driven `FinishCancellation` supplies only the remaining transaction budget. `Stop` and write rejection each supply a fresh full `CancellationTimeoutMs`. Every individual send remains bounded by its supplied value. | On write timeout/error, caller-driven cancellation quarantines immediately; `Stop` ignores its send result; write rejection records a delivery fault and quarantines from the audio loop. |
| SAPI `OnAudioData` / `Write` callback | External COM callback from audio worker. | No CoreEngine-enforced deadline. SAPI may block independently of pipe cancellation. | Existing unbounded callback path; covered by tests that prove request-state locks are not held, not by a completion guarantee. |
| SAPI `OnSpeechEvent` / `AddEvents` callback | External COM callback from control worker. | No CoreEngine-enforced deadline. SAPI may block independently of pipe cancellation. | Existing unbounded callback path; fault publication must remain independent, but final worker join can still wait for callback return. |

---

## 9. Conclusion and Extraction Boundaries for Phase A

This audit confirms that `SpeechWorker`'s state management consists of pure deterministic decisions combined with mechanical state applications and un-locked side effects.

In Phase A:
1. Phase A extracts lifecycle decisions only: request admission, already-classified upstream terminal/failure transitions, cancellation eligibility, reset/fault effects, timeout selection, and mapping of a caller-computed terminal-boundary observation. Control-event classification remains in `SpeechWorker` for Phase B. PCM/frame arithmetic, span eligibility, and write-result classification remain in `SpeechWorker` for Phase C.
2. `SpeechWorker` retains all mutexes, condition variables, threads, IPC calls, COM callbacks, condition variable notifications, and session quarantine operations.
3. Every policy invocation and application occurs under one continuous hold of `m_requestMutex`, preventing a stale policy result from being applied to a replaced request. This does not make claims about unrelated external callback liveness.
