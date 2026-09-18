# Task 2 Brief: Add Pure ControlEventPolicy with TDD

## Objectives
1. **API Declaration:**
   Create `CoreEngine/ControlEventPolicy.h` with the exact approved API from design:
   ```cpp
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
   ```

2. **20 Direct Policy Tests:**
   Create `CoreEngine.Tests/ControlEventPolicyTests.cpp` implementing the 20 tests specified in the design doc:
   1. `MatchedWordBoundarySpeakingRefreshesAndAdmits`
   2. `SentenceAndBookmarkUseSpeechBoundaryPolicy`
   3. `MalformedMatchedBoundaryFaultActionSuppressesProgressAndForwarding` (table-driven across Speaking, Cancelling, and Idle)
   4. `ValidBoundaryCancellingRefreshesButDoesNotForward`
   5. `ValidBoundaryIdleNeitherRefreshesNorForwards`
   6. `ValidBoundaryWithFaultPendingStillRefreshesButDoesNotForward`
   7. `ValidSynthesisCompleteClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, fault-pending, and duplicate-terminal facts)
   8. `ValidSynthesisCancelledClassifiesAndRefreshes` (tables Speaking, Cancelling, Idle, Faulted, and fault-pending)
   9. `InvalidTerminalPayloadDoesNotRefreshProgress` (covers both terminal types in each lifecycle category)
   10. `StaleAdversarialEventsHaveNoActionOrProgressButPreserveHealthyProvisionalAdmission` (tables valid and malformed boundaries, invalid terminal, error/fatal logs, legacy, and unknown; also repeats with P0 == false)
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

3. **TDD Red Phase:**
   - Add `ControlEventPolicyTests.cpp` to `CoreEngine.Tests/CoreEngine.Tests.vcxproj`.
   - Build Debug x64 and capture the expected linker error (unresolved external symbol `EvaluateParsedEvent` and `EvaluateFinalAdmission`) to `.agents/sdd/phase7b/task2-red.txt`.
   - Confirm there are zero compile syntax errors; only missing symbols.

4. **Implementation:**
   - Create `CoreEngine/ControlEventPolicy.cpp` implementing `EvaluateParsedEvent` and `EvaluateFinalAdmission`.
   - Start with `#include "pch.h"` followed by `#include "ControlEventPolicy.h"`.
   - Logic contracts:
     - `P0 = (context.upstreamState != UpstreamState::Faulted && context.downstreamState != DownstreamState::Faulted && !context.faultPending)`
     - `EvaluateParsedEvent`:
       - If `event.speakId != context.token.speakId`: return `{ EventAction::NoLockedAction, false, P0 }`
       - Matching ID:
         - `WordBoundary`, `SentenceBoundary`, `Bookmark`:
           - If `!event.hasValidSpeechOffsets`: return `{ EventAction::MalformedSpeechBoundary, false, false }`
           - Else:
             - `refresh = (context.downstreamState == DownstreamState::Speaking || context.downstreamState == DownstreamState::Cancelling)`
             - `forward = (context.downstreamState == DownstreamState::Speaking && P0)`
             - return `{ EventAction::SpeechBoundary, refresh, forward }`
         - `SynthesisComplete`:
           - `refresh = (event.hasValidTotalBytes && (context.downstreamState == DownstreamState::Speaking || context.downstreamState == DownstreamState::Cancelling))`
           - return `{ EventAction::SynthesisComplete, refresh, P0 }`
         - `SynthesisCancelled`:
           - `refresh = (event.hasValidTotalBytes && (context.downstreamState == DownstreamState::Speaking || context.downstreamState == DownstreamState::Cancelling))`
           - return `{ EventAction::SynthesisCancelled, refresh, P0 }`
         - `LegacyCompleted`: return `{ EventAction::LegacyCompleted, false, P0 }`
         - `Unknown`: return `{ EventAction::Unknown, false, P0 }`
         - `Log`:
           - `action`: if `event.logSeverity == "error"` -> `RequestErrorLog`, if `event.logSeverity == "fatal"` -> `FatalLog`, else `InformationalLog`
           - return `{ action, false, P0 }`
     - `EvaluateFinalAdmission`:
       - If `event.speakId != context.token.speakId`: return `FinalAdmission::Reject`
       - If `event.type == ProviderEventType::Log`: return `FinalAdmission::Allow`
       - Else: return `(context.downstreamState == DownstreamState::Speaking && !context.faultPending) ? FinalAdmission::Allow : FinalAdmission::Reject`

5. **Project Metadata:**
   - In `CoreEngine/CoreEngine.vcxproj`: add `<ClInclude Include="ControlEventPolicy.h" />` and `<ClCompile Include="ControlEventPolicy.cpp" />`.
   - In `CoreEngine/CoreEngine.vcxproj.filters`: add `ControlEventPolicy.h` to `<Filter>Header Files</Filter>` and `ControlEventPolicy.cpp` to `<Filter>Source Files</Filter>`.
   - In `CoreEngine.Tests/CoreEngine.Tests.vcxproj`: add `..\CoreEngine\ControlEventPolicy.cpp` with `<PrecompiledHeader>NotUsing</PrecompiledHeader>` and `<ProgramDataBaseFileName>$(IntDir)ControlEventPolicy.pdb</ProgramDataBaseFileName>`.

6. **TDD Green Phase:**
   - Build Debug x64 (zero errors, zero warnings).
   - Run `ControlEventPolicyTests.*` under 30s watchdog.
   - Record green output in `.agents/sdd/phase7b/task2-green.txt`.
   - Verify exactly 20 tests ran and passed.
   - Verify MockProvider quiescence.
