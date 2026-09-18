# Task 2 Implementation Report: Add Pure ControlEventPolicy with TDD

## Status: DONE

## Overview
Task 2 created the pure, allocation-free, `noexcept` `ControlEventPolicy` leaf component implementing event classification (`EvaluateParsedEvent`) and final callback admission (`EvaluateFinalAdmission`) as specified in the Phase 7B design document and task brief. Rigorous TDD workflow was adhered to, with all 20 direct policy tests written and verified red before implementing the production policy.

## Changes Made

1. **`CoreEngine/ControlEventPolicy.h`**
   - Declared the pure `ControlEventPolicy` namespace API.
   - Defined `EventAction` enum (`NoLockedAction`, `SpeechBoundary`, `MalformedSpeechBoundary`, `SynthesisComplete`, `SynthesisCancelled`, `LegacyCompleted`, `InformationalLog`, `RequestErrorLog`, `FatalLog`, `Unknown`).
   - Defined `EventDecision` struct (`action`, `shouldRefreshProgress`, `shouldForwardToSapi`).
   - Defined `FinalAdmission` enum (`Reject`, `Allow`).
   - Declared `[[nodiscard]] EventDecision EvaluateParsedEvent(...) noexcept` and `[[nodiscard]] FinalAdmission EvaluateFinalAdmission(...) noexcept`.
   - Adhered to strict Allman bracing style and pure level-0 domain dependencies (`SpeechEventTypes.h`, `SpeechWorkerTypes.h`).

2. **`CoreEngine.Tests/ControlEventPolicyTests.cpp`**
   - Implemented all 20 required direct policy tests:
     1. `MatchedWordBoundarySpeakingRefreshesAndAdmits`
     2. `SentenceAndBookmarkUseSpeechBoundaryPolicy`
     3. `MalformedMatchedBoundaryFaultActionSuppressesProgressAndForwarding` (table-driven across Speaking, Cancelling, and Idle)
     4. `ValidBoundaryCancellingRefreshesButDoesNotForward`
     5. `ValidBoundaryIdleNeitherRefreshesNorForwards`
     6. `ValidBoundaryWithFaultPendingStillRefreshesButDoesNotForward`
     7. `ValidSynthesisCompleteClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, fault-pending, and duplicate-terminal facts)
     8. `ValidSynthesisCancelledClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, and fault-pending)
     9. `InvalidTerminalPayloadDoesNotRefreshProgress` (covers both terminal types in each lifecycle category)
     10. `StaleAdversarialEventsHaveNoActionOrProgressButPreserveHealthyProvisionalAdmission` (tables valid and malformed boundaries, invalid terminal, error/fatal logs, legacy, and unknown; repeats with P0 == false)
     11. `FaultedOrFaultPendingContextSuppressesProvisionalAdmission` (covers matching and stale events with upstream Faulted, downstream Faulted, and fault-pending independently)
     12. `LegacyCompletedHasDiagnosticActionWithoutProgress`
     13. `UnknownEventHasDiagnosticActionWithoutProgress`
     14. `NonErrorLogSeveritiesRemainInformational` (empty, info, warning, uppercase and arbitrary; no progress and exact pre-mutation P0)
     15. `ExactLowercaseErrorClassifiesAsRequestError` (no progress and exact pre-mutation P0)
     16. `ExactLowercaseFatalClassifiesAsFatalAndPreservesHealthyProvisionalAdmission` (no progress and exact pre-mutation P0)
     17. `FinalAdmissionAllowsPreviouslyAdmittedMatchingLogAfterLifecycleOrFaultPendingChange` (sequence-tests healthy error/fatal provisional admission followed by their own lifecycle/fault-pending mutation and final admission; separately proves pre-existing fault-pending makes provisional admission false; tables Speaking, Cancelling, Idle, Faulted, and fault-pending at the second gate)
     18. `FinalAdmissionRequiresSpeakingAndNoFaultPendingForNonLog` (tables boundary, terminal, legacy, and unknown categories)
     19. `FinalAdmissionRejectsStaleIdentity` (covers log and non-log)
     20. `PolicyEvaluationDoesNotMutateInputs`

3. **`CoreEngine/ControlEventPolicy.cpp`**
   - Implemented `EvaluateParsedEvent` and `EvaluateFinalAdmission` with exact contract semantics:
     - `P0 = (upstreamState != Faulted && downstreamState != Faulted && !faultPending)`.
     - Stale ID returns `{ NoLockedAction, false, P0 }`.
     - Boundary events check `hasValidSpeechOffsets`; refresh during Speaking or Cancelling; forward only when Speaking and `P0`.
     - Terminal events check `hasValidTerminalBytes`; refresh during Speaking or Cancelling; forward with `P0`.
     - Legacy and Unknown return diagnostic actions, no progress, forward with `P0`.
     - Log classifies `error` as `RequestErrorLog`, `fatal` as `FatalLog`, other as `InformationalLog`; no progress, forward with `P0`.
     - `EvaluateFinalAdmission` rejects stale IDs; allows matching `Log` unconditionally; requires `Speaking` and `!faultPending` for non-logs.
   - Enforced strict Allman bracing and `#include "pch.h"` as the first include.

4. **Project Configuration**
   - Added `ControlEventPolicy.h` and `ControlEventPolicy.cpp` to `CoreEngine/CoreEngine.vcxproj` and `CoreEngine/CoreEngine.vcxproj.filters`.
   - Added `ControlEventPolicyTests.cpp` and `..\CoreEngine\ControlEventPolicy.cpp` (with `PrecompiledHeader=NotUsing` and `ControlEventPolicy.pdb`) to `CoreEngine.Tests/CoreEngine.Tests.vcxproj`.

## TDD Verification Evidence

1. **Red Phase**:
   - `CoreEngine.Tests` built under Debug x64 with `ControlEventPolicyTests.cpp` present and `ControlEventPolicy.cpp` absent.
   - Verified zero syntax errors in test suite.
   - Captured exactly two expected linker errors (`LNK2019: unresolved external symbol EvaluateParsedEvent` and `EvaluateFinalAdmission`, `LNK1120: 2 unresolved externals`) in `.agents/sdd/phase7b/task2-red.txt`.

2. **Green Phase**:
   - Compiled `CoreEngine.Tests` Debug x64 with 0 warnings and 0 errors.
   - Compiled `CoreEngine.Tests` Release x64 with 0 warnings and 0 errors.
   - Executed `ControlEventPolicyTests.*` under a 30-second watchdog:
     - Debug x64: 20/20 tests passed in 109 ms total process elapsed time (1 ms test duration).
     - Release x64: 20/20 tests passed in 118 ms total process elapsed time (0 ms test duration).
   - Captured evidence in `.agents/sdd/phase7b/task2-green.txt`.

3. **MockProvider Quiescence**:
   - Verified `Get-Process -Name MockProvider -ErrorAction SilentlyContinue` returns exit code 1 (no active provider process).

4. **Code Hygiene & Scope**:
   - Verified `git diff --check` emits zero whitespace or formatting errors.
   - Verified strict Allman bracing across all new and modified C++ files.
