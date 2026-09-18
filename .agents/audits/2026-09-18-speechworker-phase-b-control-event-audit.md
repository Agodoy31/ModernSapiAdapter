# SpeechWorker Phase B Control-Event Audit

## Scope

This audit records the current control-event path that Phase B must preserve. It covers parsed provider events from `ControlThreadProc` through locked state handling, provisional admission, the Debug pause hook, final callback admission, `CSapiEngine::OnSpeechEvent`, and fault publication.

Phase B is classification-only. It does not move JSON parsing, named-pipe reads, clocks, logging, mutexes, state mutation, terminal-byte arithmetic, callbacks, notifications, or fault publication out of `SpeechWorker`.

## Ownership and Ordering

| Concern | Current owner | Phase B rule |
|---|---|---|
| Control-pipe read and JSON lifetime | `SpeechWorker::ControlThreadProc` | Remains in worker. Borrowed event string views never escape synchronous handling. |
| Missing/zero `speak_id` validation | `ControlThreadProc` | Remains before policy evaluation. Nonempty named events with ID zero quarantine the session. |
| Request identity and lifecycle state | `SpeechWorker::m_context` under `m_requestMutex` | Policy reads immutable state; worker applies effects during the same uninterrupted lock hold. |
| Provider progress clock | `m_lastProviderProgressTick` | Policy returns eligibility only; worker performs `GetTickCount64` and the store. |
| Terminal-byte validation and lifecycle | `HandleTerminalEventLocked` plus `SpeechStatePolicy` | Remains outside Phase B. |
| Log text emission and lifecycle effects | `HandleLogEventLocked` | Policy classifies severity; worker logs and applies request/session effects. |
| Provisional admission | `HandleParsedControlEventLocked` | Becomes a pure decision but does not authorize a callback. |
| Final callback admission | `ForwardEventToSapi` | Re-evaluated from live state under `m_eventForwardMutex` then `m_requestMutex`. |
| SAPI callback | `CSapiEngine::OnSpeechEvent` / `ISpTTSEngineSite::AddEvents` | Remains outside both state locks. |
| Fault publication | `EnterFaultedState` | Remains after optional event forwarding so fatal logs retain current ordering. |

The only permitted nested production lock order remains:

```text
m_eventForwardMutex -> m_requestMutex
```

## Baseline Decision Matrix

`P0` is the current provisional admission predicate:

```text
upstream != Faulted && downstream != Faulted && !faultPending
```

| Input | Locked classification and progress | Provisional result | Final admission |
|---|---|---|---|
| Empty unnamed event | Silently ignored before ID validation | No forwarding | Not reached |
| Nonempty event with `speak_id == 0` | Session fault, outside policy | No forwarding | Not reached |
| Stale nonzero ID | No validation, progress, lifecycle mutation, or fault | Retains `P0`, including Debug pause | Rejected by current-ID check |
| Valid word/sentence/bookmark, Speaking | Refresh progress | `P0` | Allowed only if still matching, Speaking, and not fault-pending |
| Valid boundary, Cancelling | Refresh progress | Suppressed | Rejected |
| Valid boundary, Idle/Faulted | No progress | Suppressed | Rejected |
| Malformed matching boundary in any lifecycle | No progress; session fault | Suppressed | Not forwarded |
| Valid terminal, Speaking/Cancelling | Refresh progress before terminal lifecycle handling | Retains initial `P0` | Live admission may reject after normal reset to Idle or after fault state; the engine callback layer ignores any admitted terminal |
| Invalid terminal payload | No progress; existing terminal handler faults | Retains initial `P0` | Rejected by fresh state/fault-pending admission |
| Valid but frame-misaligned terminal | Progress is refreshed before the terminal handler faults | Retains initial `P0` | Rejected by fresh state/fault-pending admission |
| Legacy `completed` | Diagnostic only | `P0` | Engine ignores it |
| Unknown named event, including `punctuation_boundary` | Diagnostic only | `P0` | Engine ignores it |
| Log severity `error` | Emit log; fail only matching request and reset its lifecycle | `P0` | If provisionally admitted, matching logs remain allowed after the action changes lifecycle state |
| Log severity `fatal` | Emit log; request session fault | `P0` | If provisionally admitted and no concurrent fault wins first, forwarded before its own fault publication |
| Any other log severity, including empty or uppercase | Informational only | `P0` | If provisionally admitted, matching logs are allowed regardless of later downstream/fault-pending changes |

Important retained asymmetries:

- A stale event can pass provisional admission and reach the Debug pause hook, but final admission drops it after live-state revalidation.
- Matching valid boundaries are suppressed while Idle or Cancelling; matching malformed boundaries fault in those states.
- Logs bypass downstream-state and `faultPending` checks at final admission after identity matches, but only reach that gate if the earlier `P0` was true. This preserves diagnostics when the same event changes lifecycle or sets `faultPending`; it does not admit a log that arrived after fault-pending/faulted state already existed. A concurrent fault may publish `m_faultVisible` first and suppress even a provisionally admitted fatal log.
- Empty severity is treated as `info` by worker lifecycle classification, while `CSapiEngine` currently displays an empty severity as an error. Phase B preserves this discrepancy.
- `ResetToIdleLocked` retains request identity and residual terminal facts. Idle does not imply an absent request token.
- A matching `error` log is still applied when the retained request state is already Faulted, and a matching terminal can still update upstream lifecycle while downstream is Faulted. These unusual paths are baseline worker-owned mutation behavior; Phase B must neither normalize nor newly reject them.

## Debug and Callback Boundaries

The existing `pauseNextEventForward` hook stays after locked mutation and provisional admission, but before final admission. The final policy predicate must execute after that pause against fresh state. Moving stale-event rejection ahead of the hook would alter existing race tests.

`m_faultVisible` remains an outer atomic check in `ForwardEventToSapi`; it is not policy input. Both locks are released before `OnSpeechEvent` is invoked. Fatal-log order remains:

```text
classify -> mark fault pending -> optional Debug pause -> final log admission
-> OnSpeechEvent/AddEvents -> EnterFaultedState/fault visibility
```

## Extraction Boundary

The permitted leaf is `ControlEventPolicy` with two pure decisions:

1. Classify a parsed, nonzero-ID event against immutable locked request state, returning action category, progress eligibility, and provisional callback eligibility.
2. Decide final callback admission from the same event and freshly revalidated request state.

The extraction stops if it needs to own a mutex, clock, logger, callback, pipe, engine pointer, request mutation, terminal-byte arithmetic, debug hook, notification, or fault transition.

## Existing Verification Surface

The exact pre-change focused filter selects 60 Debug tests and 44 Release tests. It includes protocol parsing, direct SAPI event mapping, real named-pipe control delivery, stale/missing identity, boundary suppression, terminal handling, log severity, progress timeout, blocked callbacks, and fault-publication races.

```text
SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings:SpeechProtocolUtilsTests.ParseControlEvent_*:SapiEngineTests.OnSpeechEvent*:SapiEngineTests.IgnoredEventTypesDoNotCallAddEvents:SapiEngineTests.EventAfterReplacementSitePublishedIsDropped:SapiEngineTests.EventWithCapturedOldSiteCompletesOnlyAgainstOldSite:SapiEngineTests.RealControlPipeBoundaryAndBookmarkEndToEnd:SapiEngineTests.RealControlPipeLogWithFriendlyTextEndToEnd:SapiEngineTests.MatchingProviderEventsKeepLongRequestAlive:SapiEngineTests.MultipleWordBoundariesWithinRequestAreProcessed:SapiEngineTests.WarningLogDoesNotFaultSession:SapiEngineTests.RequestErrorLogIsForwardedAndFailsOnlyTheUtterance:SapiEngineTests.FatalLogIsForwardedBeforeFaultPublication:SapiEngineTests.OutputSiteAbortCancelsTheActiveRequest:SapiEngineTests.BoundaryDuringCancellationIsSuppressedWithoutFault:SapiEngineTests.CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary:SapiEngineTests.InvalidSpeechEventSpeakIdQuarantinesTheWorker:SapiEngineTests.MalformedRequiredSpeechEventNumbersQuarantineTheWorker:SapiEngineTests.UnknownNamedEventWithoutSpeakIdQuarantinesTheWorker:SapiEngineTests.StaleSpeechEventWithValidSpeakIdDoesNotQuarantineTheWorker:SapiEngineTests.MissingEventNameIsSilentlyIgnored:SapiEngineTests.MalformedBoundaryWhileIdleFaultsTheWorker:SapiEngineTests.LegacyCompletedFollowedByRealTerminalCompletesSuccessfully:SapiEngineTests.UnknownNamedEventWithActiveSpeakIdDoesNotFaultWorker:SapiEngineTests.UnknownNamedEventWithStaleSpeakIdIsRejectedByAdmissionWithoutFault:SapiEngineTests.ValidBoundaryArrivingWhileIdleIsSuppressedWithoutFault:SapiEngineTests.StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker:SapiEngineTests.InvalidCancellationBoundaryFaultsTheWorker:SapiEngineTests.MisalignedSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.MissingSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.NonIntegerSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.IntegralFloatSynthesisCompleteFieldsCompleteTheRequest:SapiEngineTests.DuplicateSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.TerminalEventDeclaringFewerBytesThanAlreadyReadFaultsTheWorker:SapiEngineTests.MisalignedCancellationTotalFaultsTheWorker:SapiEngineTests.RequestErrorFailsUtteranceWithoutKillingProvider:SapiEngineTests.FatalErrorFaultsSessionAndTriggersRestart:SapiEngineTests.LogEventsDoNotExtendSynthesisInactivityTimeout:SapiEngineTests.ValidSynthesisCancelledWhileSpeakingCompletesWithoutFault:SapiEngineTests.SynthesisCompleteWhileCancellingCompletesPromptly:SapiEngineTests.AddEventsBlockingDoesNotDelayWorkerFaultPublication:SapiEngineTests.FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback:SapiEngineTests.FaultPendingRejectsStartBeforeFaultPublicationCompletes:SapiEngineTests.SpeakWaitsForSynthesisCompleteByteBoundary:SapiEngineTests.TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames
```

One residual characterization test is required: a real control-pipe `punctuation_boundary` with the active ID must remain ignored without fault and must not prevent a later valid terminal from completing the request.
