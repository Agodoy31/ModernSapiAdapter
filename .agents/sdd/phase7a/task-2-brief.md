# Task 2 Brief: Add the Pure Policy with TDD

**Worktree:** `D:\Projects\ModernSapiAdapter\.worktrees\phase7a-speech-state-policy`
**Plan Document:** `.agents/plans/2026-09-16-speechworker-phase-a-request-policy.md` (Task 2)
**Design Document:** `.agents/specs/2026-09-16-speechworker-phase-a-request-policy-design.md` (Sections 4 & 5)

## Task Objectives
1. Create `CoreEngine/SpeechStatePolicy.h`:
   - Self-contained Level-0 header in `namespace SpeechStatePolicy`.
   - Exact enums and structs:
     - `StartAction` (`Reject = 0`, `Accept = 1`), `StartDecision` (`action`, `speakId`, `generation`)
     - `UpstreamTerminalKind` (`Completed = 0`, `Cancelled = 1`)
     - `UpstreamTerminalAction` (`Apply = 0`, `DuplicateFault = 1`, `InvalidBytesFault = 2`, `MisalignedBytesFault = 3`), `UpstreamTerminalDecision` (`action`, `targetState`, `terminalAudioBytes`)
     - `RequestFailureAction` (`ApplyUtteranceFailure = 0`), `RequestFailureDecision` (`action`, `terminalAudioBytes`)
     - `TerminalBoundaryFacts` (`speakingAudioOverrun`, `speakingTerminalReached`, `cancellationTerminalReached`)
     - `TerminalBoundaryAction` (`Continue = 0`, `NormalCompleteReset = 1`, `CancelDrainReset = 2`, `UtteranceFailedReset = 3`, `OverrunFault = 4`), `TerminalBoundaryDecision` (`action`)
     - `BeginCancellationAction` (`AlreadyIdle = 0`, `AlreadyCancelling = 1`, `Faulted = 2`, `TransitionToCancelling = 3`), `BeginCancellationDecision` (`action`, `speakId`, `deadlineTick`)
     - `StopAction` (`NoAction = 0`, `ResetAndCancel = 1`), `StopDecision` (`action`, `speakId`)
     - `TimeoutCondition` (`None = 0`, `CancellationTimeout = 1`, `InactivityTimeout = 2`), `TimeoutDecision` (`condition`, `speakId`)
   - Exact function signatures (all `[[nodiscard]]`, `noexcept`):
     - `StartDecision EvaluateStart(const RequestContext& context, std::uint64_t speakId, std::uint64_t nextGeneration) noexcept;`
     - `UpstreamTerminalDecision EvaluateUpstreamTerminal(const RequestContext& context, UpstreamTerminalKind kind, std::uint64_t terminalAudioBytes, bool hasValidTerminalBytes, bool isFrameAligned) noexcept;`
     - `RequestFailureDecision EvaluateUtteranceFailure(const RequestContext& context) noexcept;`
     - `TerminalBoundaryDecision EvaluateTerminalBoundary(const RequestContext& context, const TerminalBoundaryFacts& facts) noexcept;`
     - `BeginCancellationDecision EvaluateBeginCancellation(const RequestContext& context, std::uint64_t cancellationDeadlineTick) noexcept;`
     - `StopDecision EvaluateStop(const RequestContext& context) noexcept;`
     - `TimeoutDecision EvaluateTimeouts(const RequestContext& context, std::uint64_t nowTick, std::uint64_t lastProgressTick, std::uint64_t inactivityTimeoutMs) noexcept;`
     - `bool IsWaitTerminal(const RequestContext& context, bool exitSignaled) noexcept;`

2. Create `CoreEngine.Tests/SpeechStatePolicyTests.cpp`:
   - `#include "pch.h"`
   - `#include "../CoreEngine/SpeechStatePolicy.h"`
   - Implement all 34 tests in test case `SpeechStatePolicyTests`:
     1. `StartAcceptsOnlyQuiescentNonFaultPendingState`
     2. `StartDecisionOwnsAcceptedIdentityWithoutMutatingInput`
     3. `UpstreamCompleteAppliesValidatedTerminalFact`
     4. `UpstreamCancelledAppliesValidatedTerminalFact`
     5. `DuplicateUpstreamTerminalRequestsTwoStageFaultPath`
     6. `InvalidUpstreamTerminalBytesRequestFault`
     7. `MisalignedUpstreamTerminalBytesRequestFault`
     8. `DuplicateMalformedTerminalKeepsDuplicateFirstPrecedence`
     9. `UtteranceFailureCapturesCurrentRawByteBoundary`
     10. `FailedUpstreamTakesBoundaryPrecedence`
     11. `UnfinishedUpstreamContinuesDespiteRetainedCompletedEnum`
     12. `UnfinishedUpstreamContinuesDespiteRetainedCancelledEnum`
     13. `SpeakingReachedResetsOnlySpeakingLifecycle`
     14. `CancellationReachedResetsOnlyCancellingLifecycle`
     15. `SpeakingOverrunRequestsFault`
     16. `IrrelevantBoundaryFactsAreIgnoredForCurrentState`
     17. `CancellationFromActiveSpeakingTransitions`
     18. `CancellationFromCompletedSpeakingRetainsCompletedEnumContract`
     19. `CancellationFromCancelledSpeakingRetainsCancelledEnumContract`
     20. `CancellationAlreadyIdleMapsToAlreadyIdle`
     21. `CancellationAlreadyDrainingMapsToAlreadyCancelling`
     22. `CancellationFaultedMapsToFaulted`
     23. `StopActiveRequestCapturesSpeakId`
     24. `StopIdleDoesNothing`
     25. `StopFaultedDoesNothing`
     26. `CancellationTimeoutPrecedesInactivityTimeout`
     27. `CancellationDeadlineRequiresNonzeroExpiredTick`
     28. `ExpiredRetainedCancellationDeadlineWhileIdleDoesNotTimeout`
     29. `ActiveSynthesisInactivityExpires`
     30. `TerminalAudioInactivityExpires`
     31. `ClockRegressionDoesNotExpireInactivity`
     32. `NoEligibleTimeoutReturnsNone`
     33. `WaitTerminalRecognizesIdleFaultAndExit`
     34. `PolicyDecisionsDoNotMutateInputContext`

3. Add `SpeechStatePolicyTests.cpp` to `CoreEngine.Tests\CoreEngine.Tests.vcxproj`.
4. Attempt build of `CoreEngine.Tests.vcxproj` in Debug|x64 and capture expected unresolved linker errors in `.agents/sdd/phase7a/task2-red.txt` (must be linker unresolved externals, not test compile errors).
5. Implement `CoreEngine/SpeechStatePolicy.cpp`:
   - `#include "pch.h"`
   - `#include "SpeechStatePolicy.h"`
   - Pure functional implementations adhering to Section 5 of the design.
   - Do NOT `#include "SpeechProtocolUtils.h"`. Implement timeout arithmetic directly from integer ticks.
6. Register in project files:
   - In `CoreEngine\CoreEngine.vcxproj`: add `<ClInclude Include="SpeechStatePolicy.h" />` and `<ClCompile Include="SpeechStatePolicy.cpp" />`.
   - In `CoreEngine\CoreEngine.vcxproj.filters`: add `SpeechStatePolicy.h` under `Header Files` and `SpeechStatePolicy.cpp` under `Source Files`.
   - In `CoreEngine.Tests\CoreEngine.Tests.vcxproj`: add `..\CoreEngine\SpeechStatePolicy.cpp` with `<PrecompiledHeader>NotUsing</PrecompiledHeader>`.
7. Build `CoreEngine.Tests.vcxproj` in Debug|x64.
8. Execute `SpeechStatePolicyTests.*` and record in `.agents/sdd/phase7a/task2-green.txt`.
9. Run the 53-test focused filter gate and verify it selects exactly 53 tests.
10. Check `git diff --check` for zero formatting errors.
11. Write report to `.agents/sdd/phase7a/task-2-report.md`.
