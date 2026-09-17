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
| `m_context.token` | `RequestToken` (`speakId`, `generation`) | Valid iff `speakId != 0 && generation != 0`. Zeroed when idle. | Protected by `m_requestMutex`. | SAPI caller, Audio worker, Control worker | Identifies active request and guards against ABA recycling across restarts. |
| `m_context.upstreamState` | `UpstreamState` enum | `Idle`, `Active`, `Completed`, `Cancelled`, `Failed`, `Faulted`. | Protected by `m_requestMutex`. | SAPI caller, Control worker, Audio worker | Tracks provider-side synthesis lifecycle. |
| `m_context.downstreamState` | `DownstreamState` enum | `Idle`, `Speaking`, `Cancelling`, `Faulted`. | Protected by `m_requestMutex`. | SAPI caller, Audio worker, Control worker | Tracks SAPI-side audio delivery lifecycle. |
| `m_context.rawAudioBytesRead` | `uint64_t` | Monotonically increases during request. Reset to 0 on new request. | Protected by `m_requestMutex`. | Audio worker, SAPI caller (audit/cancel) | Total raw PCM bytes read from audio pipe for this request. |
| `m_context.deliveredAudioBytes`| `uint64_t` | Monotonically increases during `Speaking`. Never exceeds `upstreamTerminalBytes` without fault. | Protected by `m_requestMutex`. | Audio worker | Total PCM bytes delivered to `pOutputSite->Write()`. |
| `m_context.upstreamTerminalBytes` | `uint64_t` | Frame-aligned (`% blockAlign == 0`). Set upon terminal event or error log. | Protected by `m_requestMutex`. | Control worker, Audio worker | Exact declared terminal byte boundary from provider. |
| `m_context.upstreamFinished` | `bool` | True iff provider emitted `synthesis_complete`, `synthesis_cancelled`, or `severity="error"` log. | Protected by `m_requestMutex`. | Control worker, Audio worker | Signals provider has completed/terminated its upstream transmission. |
| `m_context.faultPending` | `bool` | True when protocol/boundary error detected but session quarantine publication incomplete. | Protected by `m_requestMutex`. | Audio worker, Control worker, SAPI caller | Inhibits new request admission and SAPI event forwarding. |
| `m_context.cancellationDeadlineTick` | `ULONGLONG` | Monotonic tick (`GetTickCount64() + CancellationTimeoutMs`). 0 when not cancelling. | Protected by `m_requestMutex`. | SAPI caller, Audio worker | Absolute deadline for provider cancellation acknowledgement and PCM drain. |
| `m_context.completionHr` | `HRESULT` | `S_OK`, `E_FAIL`, `E_ABORT`, or Win32 error code. | Protected by `m_requestMutex`. | SAPI caller, Control worker, Audio worker | Terminal HRESULT returned by `WaitUntilFinished` / `CancelAndDrain`. |
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

---

## 4. Allowed Composite-State Matrix

The composite request state is defined by the 4-tuple:
`C = (UpstreamState, DownstreamState, upstreamFinished, faultPending)`

| State ID | `UpstreamState` | `DownstreamState` | `upstreamFinished` | `faultPending` | Classification | Invariant & Meaning |
|---|---|---|---|---|---|---|
| **S0** | `Idle` | `Idle` | `false` | `false` | **Legal (Quiescent)** | Normal idle state between synthesis requests. `token` is zeroed or unassigned. Assembler carry is empty. |
| **S1** | `Active` | `Speaking` | `false` | `false` | **Legal (Synthesizing)** | Normal synthesis underway. Audio is framed and delivered to SAPI. Progress clock advances. |
| **S2** | `Completed` | `Speaking` | `true` | `false` | **Legal (Awaiting Terminal Audio)** | Provider finished synthesis (`synthesis_complete`). Worker is framing and delivering remaining declared PCM frames. |
| **S3** | `Active` | `Cancelling` | `false` | `false` | **Legal (Draining Cancel)** | Cancellation requested by SAPI or write rejection; cancel message sent to provider; awaiting `synthesis_cancelled`. Audio is discarded. |
| **S4** | `Cancelled` | `Cancelling` | `true` | `false` | **Legal (Draining Cancel with Boundary)** | Provider acknowledged cancel with `synthesis_cancelled` declaring byte boundary; worker discarding remaining audio until `rawAudioBytesRead >= upstreamTerminalBytes`. |
| **S5** | `Completed` | `Cancelling` | `true` | `false` | **Legal (Cancel Racing Complete)** | Provider sent `synthesis_complete` before receiving or processing cancel; worker discards remaining audio until declared terminal reached. |
| **S6** | `Failed` | `Speaking` / `Cancelling` | `true` | `false` | **Transient (Utterance Error)** | Utterance error log received. Immediately transitions to `S0` via `ResetToIdleLocked()`. |
| **S7** | Any | Any | Any | `true` | **Transient (Fault Pending)** | Protocol error, audio overrun, or invalid boundary detected. Awaiting execution of `EnterFaultedState()`. Start and event forward are inhibited. |
| **S8** | `Faulted` | `Faulted` | Any | `false` | **Legal (Session Faulted / Quarantined)** | Fatal session error. All subsequent requests rejected. Worker threads drain/exit. |
| **U1** | `Idle` | `Speaking` | Any | Any | **Illegal (Unreachable)** | Cannot speak without active upstream session. |
| **U2** | `Idle` | `Cancelling` | Any | Any | **Illegal (Unreachable)** | Cannot drain cancellation when idle. |
| **U3** | `Active` | `Idle` | Any | Any | **Illegal (Unreachable)** | Active upstream must correspond to downstream speaking or cancelling. |
| **U4** | `Completed` | `Idle` | `false` | Any | **Illegal (Unreachable)** | Completed upstream requires `upstreamFinished == true`. |
| **U5** | `Cancelled` | `Speaking` | Any | Any | **Illegal (Unreachable)** | Cannot deliver audio to SAPI when upstream is cancelled. |
| **U6** | `Completed` / `Cancelled` | `Speaking` / `Cancelling` | `false` | `false` | **Illegal (Unreachable)** | Terminal upstream states require `upstreamFinished == true`. |

---

## 5. Transition and Effect Table

This table maps every state transition, its precondition, atomic effects, notifications, and post-unlock actions.

| Transition Name | Trigger / Input | Precondition | Mutated State Fields | Assembler Action | Notification | Post-Unlock Action | Destination State |
|---|---|---|---|---|---|---|---|
| `StartRequest` | `SpeechWorker::Start(speakId)` | `upstreamState == Idle && downstreamState == Idle && !faultPending` | `token.speakId = speakId`, `token.generation = ++m_generationCounter`, `upstreamState = Active`, `downstreamState = Speaking`, audio counters = 0, `upstreamFinished = false`, `faultPending = false`, `completionHr = S_OK` | `Reset()` | None | None | **S1** (`Active`, `Speaking`) |
| `RejectStart` | `SpeechWorker::Start(speakId)` | `upstreamState != Idle || downstreamState != Idle || faultPending` | None | None | None | None | No change |
| `TerminalComplete` | `synthesis_complete` control event | `token.Matches() && !upstreamFinished && hasValidTerminalBytes && (terminalBytes % blockAlign == 0)` | `upstreamTerminalBytes = terminalBytes`, `upstreamFinished = true`, `upstreamState = Completed` | None | Evaluates boundary; if reached, notifies `m_requestChanged` | If boundary reached: None; if overrun: `EnterFaultedState()` | **S2** (if draining), **S0** (if reached), or **S7** (if overrun) |
| `TerminalCancelled`| `synthesis_cancelled` control event | `token.Matches() && !upstreamFinished && hasValidTerminalBytes && (terminalBytes % blockAlign == 0)` | `upstreamTerminalBytes = terminalBytes`, `upstreamFinished = true`, `upstreamState = Cancelled` | None | Evaluates boundary; if reached, notifies `m_requestChanged` | If boundary reached: None | **S4** (if draining), or **S0** (if reached) |
| `DuplicateTerminal`| Terminal event with `upstreamFinished == true` | `token.Matches() && upstreamFinished == true` | `upstreamState = Faulted`, `downstreamState = Faulted`, `completionHr = E_FAIL`, `faultPending = true` | None | `m_requestChanged.notify_all()` | `EnterFaultedState()` | **S7** / **S8** |
| `MisalignedTerminal`| Terminal event with `terminalBytes % blockAlign != 0` | `token.Matches()` | `upstreamState = Faulted`, `downstreamState = Faulted`, `completionHr = E_FAIL`, `faultPending = true` | None | `m_requestChanged.notify_all()` | `EnterFaultedState()` | **S7** / **S8** |
| `SapiAbortRequested`| SAPI `pOutputSite->GetActions()` includes `SPVES_ABORT` | `downstreamState == Speaking` | `downstreamState = Cancelling`, `upstreamFinished = false`, `upstreamTerminalBytes = 0`, `cancellationDeadlineTick = deadline` | `Reset()` | None | `SendCancellation(speakId, timeout)` outside lock | **S3** (`Active`, `Cancelling`) |
| `WriteRejected` | SAPI `OnAudioData` returns `false` | `token.Matches() && downstreamState == Speaking` | `downstreamState = Cancelling`, `upstreamFinished = false`, `upstreamTerminalBytes = 0`, `cancellationDeadlineTick = deadline` | `Reset()` | None | `SendCancellation(speakId, timeout)` outside lock | **S3** (`Active`, `Cancelling`) |
| `TerminalAudioDrain`| Ingest / delivery reaches `deliveredAudioBytes == declared && rawAudioBytes == declared && !HasCarry()` | `upstreamFinished && downstreamState == Speaking` | `upstreamState = Idle`, `downstreamState = Idle` | `Reset()` | `m_requestChanged.notify_all()` | None | **S0** (`Idle`, `Idle`) |
| `CancelDrainComplete`| Ingest reaches `rawAudioBytesRead >= upstreamTerminalBytes` | `upstreamFinished && downstreamState == Cancelling` | `upstreamState = Idle`, `downstreamState = Idle` | `Reset()` | `m_requestChanged.notify_all()` | None | **S0** (`Idle`, `Idle`) |
| `AudioOverrun` | Ingest / delivery results in `raw > declared || delivered > declared` | `upstreamFinished && downstreamState == Speaking` | `faultPending = true` | None | None | `EnterFaultedState()` | **S7** / **S8** |
| `UtteranceErrorLog` | `severity == "error"` log event | `token.Matches()` | `upstreamState = Failed`, `upstreamFinished = true`, `completionHr = E_FAIL`, `upstreamTerminalBytes = rawAudioBytesRead` -> immediately `ResetToIdleLocked()` | `Reset()` | `m_requestChanged.notify_all()` | Forward log event outside lock | **S0** (`Idle`, `Idle`) |
| `FatalLog` | `severity == "fatal"` log event | Any | `faultPending = true` | None | None | `EnterFaultedState()` | **S7** / **S8** |
| `SessionQuarantine` | Transport failure, pipe crash, or timeout | Any | `upstreamState = Faulted`, `downstreamState = Faulted`, `completionHr = E_FAIL` | `Reset()` | `m_requestChanged.notify_all()` | `m_pClient->Cancel()` | **S8** (`Faulted`, `Faulted`) |
| `StopRequest` | `SpeechWorker::Stop()` | `downstreamState != Idle && downstreamState != Faulted` | `upstreamState = Idle`, `downstreamState = Idle` | `Reset()` | `m_requestChanged.notify_all()` | `SendCancellation(speakId, timeout)` | **S0** (`Idle`, `Idle`) |

---

## 6. Current Lock-Ordering Table

The established lock-ordering discipline is strictly hierarchical. Reverse acquisition is strictly forbidden and would produce deadlock.

| Lock Name | Native Type | Protected Resources | Acquisition Context | Permitted Nested Locks |
|---|---|---|---|---|
| `m_eventForwardMutex` | `std::mutex` | Serializes event callback admission against session fault publication. | Control worker (`ForwardEventToSapi`), Fault publisher (`EnterFaultedState`). | MAY acquire `m_requestMutex` while holding `m_eventForwardMutex`. |
| `m_requestMutex` | `std::mutex` | `m_context`, `m_frameAssembler`, `m_generationCounter`. | Audio worker, Control worker, SAPI caller. | MUST NOT acquire ANY mutex while holding `m_requestMutex`. |
| Test Hook Mutexes (`_DEBUG` only) | `std::mutex` (`eventForwardMutex`, `abortTransitionMutex`, `faultPublicationMutex`) | Test pause and synchronization flags. | Test harness and worker threads. | Acquired independently or around test synchronization points. Never held during production lock nesting. |

**Inviolable Rule:** `m_eventForwardMutex` -> `m_requestMutex` is the ONLY valid production nested order. Reverse acquisition (`m_requestMutex` -> `m_eventForwardMutex`) is strictly prohibited.

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
| `SpeechWorker::~SpeechWorker()` | Sets `m_exit = true`, releases test hooks, calls `m_pClient->Cancel()`, joins `m_audioThread` and `m_controlThread`. | Bounded by pipe I/O cancellation and 250ms read poll timeout. | Guaranteed to join; threads exit immediately upon wake. |
| Audio pipe read | `m_pClient->ReadAudioChunk` with 250ms poll interval. | Maximum 250ms per loop iteration, or immediate on `m_pClient->Cancel()`. | Exits on `m_exit` or enters fault on I/O failure. |
| Control pipe read | `m_pClient->ReadControlMessage` with 250ms poll interval. | Maximum 250ms per loop iteration, or immediate on `m_pClient->Cancel()`. | Exits on `m_exit` or enters fault on I/O failure. |
| `FinishCancellation` Wait | `m_requestChanged.wait_for(lock, remaining, ...)` | Bounded by `CancellationTimeoutMs` (500ms). | If timeout expires, unlocks and calls `EnterFaultedState()`, returning `ERROR_TIMEOUT`. |
| `WaitUntilFinished` Loop | `m_requestChanged.wait_for(lock, 10ms, ...)` | 10ms poll granularity; bounded by `SynthesisInactivityTimeoutMs` (1500ms) or `CancellationTimeoutMs` (500ms). | On inactivity timeout, unlocks and calls `EnterFaultedState()`, returning `ERROR_TIMEOUT`. |
| `SendCancellation` IPC | Overlapped pipe write with `sendTimeout` (up to 500ms). | Bounded by remaining cancellation budget. | On write timeout/error, calls `EnterFaultedState()`, returning `ERROR_TIMEOUT` or `E_FAIL`. |

---

## 9. Conclusion and Extraction Boundaries for Phase A

This audit confirms that `SpeechWorker`'s state management consists of pure deterministic decisions combined with mechanical state applications and un-locked side effects.

In Phase A:
1. All decision logic (boundary checks, overrun checks, legal state combinations, timeout predicates, cancellation eligibility, start eligibility, terminal event validation) can be extracted into a pure, stateless leaf policy (`SpeechStatePolicy`).
2. `SpeechWorker` retains all mutexes, condition variables, threads, IPC calls, COM callbacks, condition variable notifications, and session quarantine operations.
3. Every policy invocation will occur under continuous hold of `m_requestMutex`, ensuring zero race conditions or state bifurcation.
