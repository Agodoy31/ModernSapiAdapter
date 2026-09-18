# SpeechWorker Phase B Control-Event Policy Design

## Objective

Extract the deterministic control-event classification and callback-admission rules from `SpeechWorker` into a pure `ControlEventPolicy` leaf. Preserve all existing IPC, state, timing, diagnostic, callback, race, and fault-publication behavior.

## Non-Goals

- No provider-protocol change and no new event type.
- No punctuation or viseme support; `punctuation_boundary` remains an unknown event that CoreEngine ignores.
- No change to event timing, buffering, sleeps, polling, deadlines, retries, or forwarding order.
- No movement of JSON parsing, named-pipe reads, clocks, logging, state mutation, terminal-byte logic, COM calls, callbacks, debug hooks, notifications, or fault publication.
- No new synchronization object and no transfer of thread, mutex, engine, pipe, or request-state ownership.

## Pure API

Create `CoreEngine/ControlEventPolicy.h` and `CoreEngine/ControlEventPolicy.cpp`.

```cpp
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
```

The header is self-contained and directly includes the headers that define `ProviderControlEvent` and `RequestContext`. Outputs contain no borrowed views or pointers.

## Preconditions

`EvaluateParsedEvent`:

- The parser and empty-envelope checks have already run.
- `event.speakId` is nonzero; missing/zero identity remains a worker-level protocol fault.
- The caller holds `m_requestMutex` and retains it without interruption through evaluation and application.
- The function may inspect borrowed event views only during the call.

`EvaluateFinalAdmission`:

- The caller has acquired `m_eventForwardMutex`, rejected `m_faultVisible`, rejected zero identity, and only then acquired `m_requestMutex`.
- The decision is applied before either lock is released; the locks are then released before the callback.

## Classification Contract

The policy performs exact, case-sensitive comparisons. It returns semantic decisions only. `NoLockedAction` means that no diagnostic or state action is applied under the request lock; it does not imply that provisional dispatch is false.

- A stale nonzero ID returns `NoLockedAction`, no progress, and the healthy provisional predicate. It performs no payload validation or severity action. This preserves the Debug pause and second-gate rejection sequence.
- A matching valid word, sentence, or bookmark returns `SpeechBoundary`. It refreshes progress while downstream is Speaking or Cancelling. It is provisionally forwardable only while Speaking and the healthy provisional predicate holds.
- A matching boundary with invalid offsets returns `MalformedSpeechBoundary`, no progress, and no forwarding, regardless of downstream lifecycle.
- A matching terminal returns the corresponding terminal action. Progress is eligible only when its byte field parsed successfully and downstream is Speaking or Cancelling. Frame alignment and duplicate status are intentionally not Phase B inputs, so valid-but-misaligned and valid duplicate terminals refresh progress before the existing terminal handler faults.
- Every matching terminal retains the healthy provisional predicate regardless of downstream lifecycle or payload validity. The worker must not rewrite this flag after terminal handling; resulting state normally causes the live final gate to reject it.
- Legacy-completed and unknown events return their diagnostic actions and do not refresh progress. Their provisional flag retains the healthy predicate; the downstream engine continues to ignore them.
- Exact lowercase `error` returns `RequestErrorLog`; exact lowercase `fatal` returns `FatalLog`; every other severity returns `InformationalLog`.
- Log severity classification neither emits a diagnostic nor changes request state.

The provisional predicate remains:

```text
upstream != Faulted && downstream != Faulted && !faultPending
```

## Final Admission Contract

Final admission always rejects a stale ID. For a matching ID:

- `Log` is allowed regardless of downstream lifecycle and `faultPending` after the caller's `m_faultVisible` check. This is only the second gate: a log reaches it only when its earlier provisional decision was true.
- Every non-log event requires downstream `Speaking` and `faultPending == false`.

This decision remains distinct from provisional admission. It is recomputed from live state immediately before callback invocation.

## Worker Application Order

`ControlThreadProc` keeps early parsing and missing-ID behavior. Under one continuous `m_requestMutex` hold, `HandleParsedControlEventLocked`:

1. calls `EvaluateParsedEvent`;
2. refreshes `m_lastProviderProgressTick` only when requested;
3. unconditionally switches on `EventAction`, independently of `shouldForwardToSapi`, and invokes the current worker-owned behavior:
   - no state effect for a valid boundary;
   - request a session fault for a malformed boundary;
   - call the existing terminal handler for terminal actions;
   - emit the existing diagnostics for legacy and unknown events;
   - emit the existing provider log and apply current informational, request-error, or fatal behavior for log actions;
4. marks `faultPending` exactly where current code does when a session fault is requested;
5. returns the existing worker disposition for post-unlock dispatch.

`DispatchOrForwardEvent` preserves the Debug pause. `ForwardEventToSapi` retains the atomic fault-visible and zero-ID guards, acquires locks in the existing order, calls `EvaluateFinalAdmission`, releases both locks, and then invokes `OnSpeechEvent` when allowed. Fault publication remains after forwarding.

The worker may simplify `HandleLogEventLocked` to accept the already-classified action, but the helper continues to own all logging and state mutation. `HandleTerminalEventLocked` is not moved or semantically changed.

The extraction also preserves unusual matching-event behavior in an already-Faulted retained context: request-error application and terminal lifecycle mutation remain worker-owned and are not newly filtered by policy. The phase does not bless these paths as ideal; it only avoids changing them during a structural refactor.

## Required Direct Decision Tests

Create exactly 20 tests in `CoreEngine.Tests/ControlEventPolicyTests.cpp`:

1. `MatchedWordBoundarySpeakingRefreshesAndAdmits`
2. `SentenceAndBookmarkUseSpeechBoundaryPolicy`
3. `MalformedMatchedBoundaryFaultActionSuppressesProgressAndForwarding` (table-driven across Speaking, Cancelling, and Idle)
4. `ValidBoundaryCancellingRefreshesButDoesNotForward`
5. `ValidBoundaryIdleNeitherRefreshesNorForwards`
6. `ValidBoundaryWithFaultPendingStillRefreshesButDoesNotForward`
7. `ValidSynthesisCompleteClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, fault-pending, and duplicate-terminal facts)
8. `ValidSynthesisCancelledClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, and fault-pending)
9. `InvalidTerminalPayloadDoesNotRefreshProgress` (covers both terminal types in each lifecycle category)
10. `StaleAdversarialEventsHaveNoActionOrProgressButPreserveHealthyProvisionalAdmission` (tables valid and malformed boundaries, invalid terminal, error/fatal logs, legacy, and unknown; also repeats with `P0 == false`)
11. `FaultedOrFaultPendingContextSuppressesProvisionalAdmission` (covers matching and stale events with upstream Faulted, downstream Faulted, and fault-pending independently)
12. `LegacyCompletedHasDiagnosticActionWithoutProgress`
13. `UnknownEventHasDiagnosticActionWithoutProgress`
14. `NonErrorLogSeveritiesRemainInformational` (empty, info, warning, uppercase and arbitrary; no progress and exact pre-mutation `P0`)
15. `ExactLowercaseErrorClassifiesAsRequestError` (no progress and exact pre-mutation `P0`)
16. `ExactLowercaseFatalClassifiesAsFatalAndPreservesHealthyProvisionalAdmission` (no progress and exact pre-mutation `P0`)
17. `FinalAdmissionAllowsPreviouslyAdmittedMatchingLogAfterLifecycleOrFaultPendingChange` (sequence-tests healthy error/fatal provisional admission followed by their own lifecycle/fault-pending mutation and final admission; separately proves pre-existing fault-pending makes provisional admission false; tables Speaking, Cancelling, Idle, Faulted, and fault-pending at the second gate)
18. `FinalAdmissionRequiresSpeakingAndNoFaultPendingForNonLog` (tables boundary, terminal, legacy, and unknown categories)
19. `FinalAdmissionRejectsStaleIdentity` (covers log and non-log)
20. `PolicyEvaluationDoesNotMutateInputs`

Tests are deterministic and use no pipes, COM, clocks, sleeps, or production test hooks.

## Integration Characterization

Add `SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault`. It uses the existing real control pipe and sends `punctuation_boundary` for the active request. Its offset-looking fields are intentionally not parsed or validated because the name maps to `Unknown`. In Debug, the existing event-forward pause hook plus captured diagnostic log prove it traverses the current unknown-event provisional path; the test releases the hook before sending a valid terminal. In both configurations it proves that no SAPI event or fault is produced and the request completes. Keep the test compiled in both configurations so count arithmetic remains stable. Extend the existing parser mapping test, without creating another test case, to assert that representative punctuation and viseme names remain `Unknown`.

Existing event and race test behavior remains unchanged; the parser mapping test gains only the specified Unknown assertions. The final focused gate selects exactly 81 Debug tests and 65 Release tests: the current 60/44, plus 20 direct policy tests, plus one punctuation characterization.

## Stop Conditions

Stop and return to Codex if implementation requires:

- policy ownership of a mutex, clock, logger, callback, pipe, engine pointer, debug hook, notification, request mutation, or fault publication;
- moving terminal-byte or frame-alignment logic into Phase B;
- moving stale-event rejection ahead of the Debug pause hook;
- changing fatal-log forward-before-fault order;
- changing the permitted nested lock order;
- adding a wait, retry, timeout, thread, queue, or provider-protocol behavior;
- making the main path less readable than the current worker-owned switch.
