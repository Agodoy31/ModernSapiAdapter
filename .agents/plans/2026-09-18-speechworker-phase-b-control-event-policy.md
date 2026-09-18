# SpeechWorker Phase B Control-Event Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `subagent-driven-development` task by task. Use Gemini 3.1 Pro subagents for specification, concurrency, and code-quality reviews. AntiGravity may perform per-task reviews, but Codex owns the final whole-branch review.

**Goal:** Extract control-event classification and callback-admission decisions into `ControlEventPolicy` while preserving all current observable behavior and timing.

**Architecture:** `SpeechWorker` continues to own the control thread, request state, locks, clocks, diagnostics, callbacks, terminal handling, and faults. The new Level-0 leaf accepts immutable event and request state and returns owned typed decisions under the worker's existing lock boundaries.

**Risk tier:** Tier 3 because the change touches event admission and fault/callback ordering.

## Global Constraints

- Work only in a dedicated Phase 7B worktree and branch based on the approved Phase A integrated head.
- Build only targeted `CoreEngine.Tests` x64 Debug and Release. x86 is prohibited. Do not attempt ARM64 in this environment.
- Add no thread, queue, mutex, condition variable, callback, IPC operation, COM call, allocation, sleep, retry, timeout, polling interval, or logging call.
- Preserve `m_eventForwardMutex -> m_requestMutex` as the only nested production lock order.
- Hold `m_requestMutex` continuously across policy evaluation and decision application.
- Release state locks before `OnSpeechEvent` / `AddEvents`.
- Preserve the Debug event-forward pause between provisional and final admission.
- Preserve fatal-log forwarding before fault publication.
- Keep terminal byte/alignment and lifecycle behavior in the current terminal handler and Phase A policy.
- Keep exact case-sensitive log severity behavior, including the existing empty-severity display discrepancy.
- Use TDD for the new policy and request independent Pro-tier review after each task.
- Do not merge, squash, push, delete the worktree, or mark Phase B complete. Return the finished branch to Codex for whole-branch review.

## Files

Create:

- `CoreEngine/ControlEventPolicy.h`
- `CoreEngine/ControlEventPolicy.cpp`
- `CoreEngine.Tests/ControlEventPolicyTests.cpp`

Modify only as needed:

- `CoreEngine/SpeechWorker.cpp`
- `CoreEngine/SpeechWorker.h`
- `CoreEngine/CoreEngine.vcxproj`
- `CoreEngine/CoreEngine.vcxproj.filters`
- `CoreEngine.Tests/CoreEngine.Tests.vcxproj`
- `CoreEngine.Tests/WorkerFaultTests.cpp` for the real-control-pipe punctuation characterization
- `CoreEngine.Tests/ProtocolParsingTests.cpp` for the two additional Unknown mapping assertions

Record compact execution evidence beneath `.agents/sdd/phase7b/`. Evidence must report commands, counts, elapsed time, and failures; do not generate large narrative transcripts when the executable result is already clear.

## Verification Setup

Use the existing Phase 7A fail-closed 30-second process watchdog and worktree-owned MockProvider quiescence helper. Copy it rather than inventing a weaker wrapper. The wrapper must capture both output streams, kill a timed-out test process, report its exit status, and check worktree-owned MockProvider quiescence in `finally`.

The current exact focused filter is recorded in the Phase B audit and selects 60 Debug tests and 44 Release tests. After Task 1 and the 20 direct policy tests, prepend `ControlEventPolicyTests.*` and add `SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault`; require exactly 81 Debug and 65 Release tests. Do not replace the exact existing list with broad unrelated suites.

Construct the final filter exactly as:

```powershell
$baselineFocusedFilter = 'SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings:SpeechProtocolUtilsTests.ParseControlEvent_*:SapiEngineTests.OnSpeechEvent*:SapiEngineTests.IgnoredEventTypesDoNotCallAddEvents:SapiEngineTests.EventAfterReplacementSitePublishedIsDropped:SapiEngineTests.EventWithCapturedOldSiteCompletesOnlyAgainstOldSite:SapiEngineTests.RealControlPipeBoundaryAndBookmarkEndToEnd:SapiEngineTests.RealControlPipeLogWithFriendlyTextEndToEnd:SapiEngineTests.MatchingProviderEventsKeepLongRequestAlive:SapiEngineTests.MultipleWordBoundariesWithinRequestAreProcessed:SapiEngineTests.WarningLogDoesNotFaultSession:SapiEngineTests.RequestErrorLogIsForwardedAndFailsOnlyTheUtterance:SapiEngineTests.FatalLogIsForwardedBeforeFaultPublication:SapiEngineTests.OutputSiteAbortCancelsTheActiveRequest:SapiEngineTests.BoundaryDuringCancellationIsSuppressedWithoutFault:SapiEngineTests.CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary:SapiEngineTests.InvalidSpeechEventSpeakIdQuarantinesTheWorker:SapiEngineTests.MalformedRequiredSpeechEventNumbersQuarantineTheWorker:SapiEngineTests.UnknownNamedEventWithoutSpeakIdQuarantinesTheWorker:SapiEngineTests.StaleSpeechEventWithValidSpeakIdDoesNotQuarantineTheWorker:SapiEngineTests.MissingEventNameIsSilentlyIgnored:SapiEngineTests.MalformedBoundaryWhileIdleFaultsTheWorker:SapiEngineTests.LegacyCompletedFollowedByRealTerminalCompletesSuccessfully:SapiEngineTests.UnknownNamedEventWithActiveSpeakIdDoesNotFaultWorker:SapiEngineTests.UnknownNamedEventWithStaleSpeakIdIsRejectedByAdmissionWithoutFault:SapiEngineTests.ValidBoundaryArrivingWhileIdleIsSuppressedWithoutFault:SapiEngineTests.StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker:SapiEngineTests.InvalidCancellationBoundaryFaultsTheWorker:SapiEngineTests.MisalignedSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.MissingSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.NonIntegerSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.IntegralFloatSynthesisCompleteFieldsCompleteTheRequest:SapiEngineTests.DuplicateSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.TerminalEventDeclaringFewerBytesThanAlreadyReadFaultsTheWorker:SapiEngineTests.MisalignedCancellationTotalFaultsTheWorker:SapiEngineTests.RequestErrorFailsUtteranceWithoutKillingProvider:SapiEngineTests.FatalErrorFaultsSessionAndTriggersRestart:SapiEngineTests.LogEventsDoNotExtendSynthesisInactivityTimeout:SapiEngineTests.ValidSynthesisCancelledWhileSpeakingCompletesWithoutFault:SapiEngineTests.SynthesisCompleteWhileCancellingCompletesPromptly:SapiEngineTests.AddEventsBlockingDoesNotDelayWorkerFaultPublication:SapiEngineTests.FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback:SapiEngineTests.FaultPendingRejectsStartBeforeFaultPublicationCompletes:SapiEngineTests.SpeakWaitsForSynthesisCompleteByteBoundary:SapiEngineTests.TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames'
$focusedFilter = "ControlEventPolicyTests.*:$baselineFocusedFilter:SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault"
```

Expected complete-suite counts after all new tests are 249 Debug and 203 Release, based on the verified 228/182 baseline. Any count difference must be explained before approval.

## Task 1: Characterize Ignored Punctuation Through the Real Control Pipe

**Files:** modify only `CoreEngine.Tests/WorkerFaultTests.cpp` and `CoreEngine.Tests/ProtocolParsingTests.cpp`.

- [ ] Add `SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault` using the existing real named-pipe fixture.
- [ ] Begin an active request and send a named `punctuation_boundary` with the active `speak_id`. Offset-looking fields may be present but are not parsed or validated because the event type remains Unknown.
- [ ] In Debug, use the existing event-forward pause hook and captured diagnostic log to prove the unknown event reaches the provisional path. Release the hook before sending the terminal. Keep the test case itself compiled in both Debug and Release.
- [ ] Extend `SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings`, without adding a new test case, to prove representative punctuation and viseme names remain Unknown.
- [ ] Send a valid terminal event after releasing any Debug hook.
- [ ] Assert no SAPI event is added, no session fault occurs, and the request completes normally.
- [ ] Do not add `PunctuationBoundary` to `ProviderEventType`; preserving unknown-event behavior is the characterization.
- [ ] Build Debug x64 and run only the new test under the watchdog. It must pass before production extraction.
- [ ] Request a Gemini 3.1 Pro specification review, resolve findings, and commit Task 1 independently.

## Task 2: Add the Pure Policy with TDD

**Files:** create the policy and test files; update project metadata.

- [ ] Declare the exact approved API from the design.
- [ ] Write all 20 required tests before implementation.
- [ ] Add the test file and declarations to the test project, build Debug x64, and capture the expected unresolved policy symbols. Syntax errors or malformed tests are not an acceptable red state.
- [ ] Implement `ControlEventPolicy.cpp`; keep `pch.h` first when compiled through the production project.
- [ ] Add policy files to the production project and filters.
- [ ] Compile the policy source directly into the test project using the established production-leaf convention, with PCH disabled and the existing PDB convention.
- [ ] Build Debug x64 and run `ControlEventPolicyTests.*` under the watchdog.
- [ ] Prove the direct suite selects exactly 20 tests.
- [ ] Request Gemini 3.1 Pro specification and code-quality reviews, fix every P0/P1 issue, and commit Task 2.

## Task 3: Rewire Locked Classification and State Application

**Files:** modify `SpeechWorker.cpp` and, only for obsolete declarations, `SpeechWorker.h`.

- [ ] Keep empty unnamed events and missing/zero `speak_id` checks in `ControlThreadProc` before policy evaluation.
- [ ] In `HandleParsedControlEventLocked`, call `EvaluateParsedEvent` under the existing uninterrupted request-lock hold.
- [ ] Refresh the progress clock only when `shouldRefreshProgress` is true, at the same relative point as current behavior.
- [ ] Switch on `EventAction` and call existing worker-owned terminal, diagnostic, log, request-failure, and fault behavior.
- [ ] Apply the action switch regardless of provisional forwarding; matching event effects must still occur when `shouldForwardToSapi` is false.
- [ ] Preserve duplicate-first terminal precedence and valid-but-misaligned terminal progress refresh.
- [ ] Preserve progress refresh for a duplicate terminal whose byte field parsed successfully before duplicate handling faults.
- [ ] Preserve `P0` as the provisional result for every matching terminal in Speaking, Cancelling, and Idle state, and do not rewrite it after terminal handling.
- [ ] Preserve request-error reset followed by log forwarding and fatal-log forwarding before fault publication.
- [ ] Preserve stale-event provisional admission and Debug pause behavior.
- [ ] Build Debug x64, assert the 81-test focused count, and run the focused filter under the watchdog.
- [ ] Request Gemini 3.1 Pro concurrency and code-quality reviews, resolve findings, and commit Task 3.

## Task 4: Rewire Final Callback Admission

**Files:** modify `SpeechWorker.cpp` and remove only the obsolete `ShouldForwardEventLocked` declaration/definition.

- [ ] Retain the outer `m_faultVisible` and zero-ID guards in `ForwardEventToSapi`.
- [ ] Retain the exact staging: acquire `m_eventForwardMutex`; check `m_faultVisible`; check zero identity; acquire `m_requestMutex`; evaluate final admission; release `m_requestMutex`; release `m_eventForwardMutex`; only then invoke `OnSpeechEvent`.
- [ ] Replace `ShouldForwardEventLocked` with `EvaluateFinalAdmission` against fresh live state after the Debug pause.
- [ ] Apply the decision before releasing the locks; release both locks before `OnSpeechEvent`.
- [ ] Verify a healthy error/fatal log remains admissible after its own action resets lifecycle or sets `faultPending`, while a log arriving with pre-existing `faultPending` never passes provisional admission and stale logs are rejected.
- [ ] Build Debug x64, assert the 81-test focused count, and run the focused filter under the watchdog.
- [ ] Request Gemini 3.1 Pro concurrency and code-quality reviews, resolve findings, and commit Task 4.

## Task 5: Review Corrections, Then Final Runtime Verification

- [ ] Build targeted `CoreEngine.Tests` Debug x64 and Release x64.
- [ ] Request a fresh Gemini 3.1 Pro whole-task review focused on behavior preservation, locks, callback ordering, test coverage, and readability. Resolve all P0/P1 findings.
- [ ] After every review correction, rebuild targeted `CoreEngine.Tests` Debug x64 and Release x64 so all final evidence comes from the reviewed source.
- [ ] Run the 81-test Debug focused filter 10 consecutive times under the 30-second watchdog, with MockProvider quiescence after every executable.
- [ ] Run the 65-test Release focused filter once under the watchdog.
- [ ] Run the complete Debug suite and require 249/249 under the watchdog.
- [ ] Run the complete Release suite and require 203/203 under the watchdog.
- [ ] Run `git diff --check` and confirm clean tracked status except intentional Phase 7B changes/evidence.
- [ ] Confirm no worktree-owned MockProvider remains after every test executable.
- [ ] Return the branch to Codex with the head commit, exact files, test/build counts and elapsed times, reviewer findings/resolutions, deviations, and any residual risk.

## Final Acceptance Checklist

- `SpeechWorker` remains the sole mutable state and synchronization owner.
- The new policy is pure, stateless, allocation-free, bounded, and `noexcept`.
- Provisional and final admission remain separate.
- Stale IDs, malformed boundaries, terminal progress, log severity, request error, fatal error, and ignored punctuation match the audit.
- No callback or IPC occurs under `m_requestMutex`.
- No timing constant or provider protocol changed.
- The runtime gates—not prose alone—demonstrate the branch is healthy.
