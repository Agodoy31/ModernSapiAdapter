# Task 3 Report: Rewire Request Admission, Stop, and Cancellation

## Status
DONE

## Summary of Changes
- Included `SpeechStatePolicy.h` in `CoreEngine/SpeechWorker.cpp`.
- Rewired `SpeechWorker::Start` to call `SpeechStatePolicy::EvaluateStart(m_context, speakId, m_generationCounter + 1)`. Preserved full reset, atomic generation increment, assembler reset, progress tick update, and state transition under the single lock hold.
- Rewired `SpeechWorker::Stop` to call `SpeechStatePolicy::EvaluateStop(m_context)`. On non-idle non-faulted requests, captured `speakId` and delegated to `ResetToIdleLocked()`, preserving the partial lifecycle reset and condition-variable notification, followed by post-lock cancellation send.
- Rewired `SpeechWorker::BeginCancellationLocked` to call `SpeechStatePolicy::EvaluateBeginCancellation(m_context, cancellationDeadline)`. Preserved HRESULT mapping (`S_FALSE` for AlreadyIdle, `E_UNEXPECTED` for AlreadyCancelling, `E_FAIL` for Faulted, `S_OK` for TransitionToCancelling), retained upstream enum, assembler reset, debug trace placement, and caller-supplied deadline.
- Rewired `SpeechWorker::UpdateAfterAudioDeliveryLocked` to call `SpeechStatePolicy::EvaluateBeginCancellation` only after existing code classifies write rejection (`!writeAccepted`). On `TransitionToCancelling`, populated `outCancellationToSend`, called `TransitionToCancelling`, and reset the assembler. Caller sends cancellation strictly outside the lock with fresh full `CancellationTimeoutMs`.
- Verified no declarations in `SpeechWorker.h` became obsolete.

## Verification Evidence
- Debug x64 build: 0 warnings, 0 errors (`/WX` compliant).
- Watchdog execution: All 53 focused tests passed in ~5.15s under the 30-second watchdog.
- Quiescence: `Assert-WorktreeMockProviderQuiesced` passed cleanly before and after the test run.
- Code hygiene: `git diff --check` emitted zero errors.

## Concerns
None. All 53 tests passed cleanly on the first run with zero regressions.
