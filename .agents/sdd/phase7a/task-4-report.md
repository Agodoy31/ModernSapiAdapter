# Task 4 Report: Rewire Terminal, Failure, Boundary, and Timeout Lifecycle

## Status
DONE

## Summary of Changes
- In `SpeechWorker::HandleTerminalEventLocked`:
  - Computed `isFrameAligned` and `kind` (`Completed` vs `Cancelled`).
  - Passed all facts to `SpeechStatePolicy::EvaluateUpstreamTerminal`.
  - Handled `DuplicateFault`, `InvalidBytesFault`, and `MisalignedBytesFault` by logging the matching diagnostic message and transitioning the request to faulted state via `TransitionRequestToFaultedLocked()`.
  - Handled `Apply` by setting `upstreamTerminalBytes`, `upstreamFinished`, and `upstreamState`, followed by the debug trace and `CheckTerminalBoundaryLocked()`.
  - Duplicate-terminal precedence is preserved over invalid or misaligned terminal bytes.
- In `SpeechWorker::HandleLogEventLocked`:
  - Maintained severity classification and provider logging.
  - On `severity == "error"`, called `SpeechStatePolicy::EvaluateUtteranceFailure(m_context)` and applied `decision.terminalAudioBytes`, setting `upstreamState = UpstreamState::Failed`, `upstreamFinished = true`, and `completionHr = E_FAIL`, before delegating to `CheckTerminalBoundaryLocked()`.
- In `SpeechWorker::CheckTerminalBoundaryLocked`:
  - Computed `TerminalBoundaryFacts` (`speakingAudioOverrun`, `speakingTerminalReached`, `cancellationTerminalReached`) via existing locked helper methods.
  - Passed facts to `SpeechStatePolicy::EvaluateTerminalBoundary`.
  - Handled each action:
    - `Continue`: returned `false`.
    - `UtteranceFailedReset`: called `ResetToIdleLocked()` and returned `false`.
    - `OverrunFault`: emitted overrun log and returned `true`.
    - `NormalCompleteReset`: emitted debug trace, called `ResetToIdleLocked()`, and returned `false`.
    - `CancelDrainReset`: emitted debug trace, called `ResetToIdleLocked()`, and returned `false`.
- In `SpeechWorker::WaitUntilFinished`:
  - Loaded `now` and `lastProgress` once per loop iteration.
  - Evaluated timeouts via `SpeechStatePolicy::EvaluateTimeouts`.
  - Handled `CancellationTimeout` and `InactivityTimeout` by unlocking `m_requestMutex`, logging the specific timeout diagnostic, invoking `EnterFaultedState()`, and returning `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`.
- In `SpeechWorker::IsWaitTerminalLocked`:
  - Delegated directly to `SpeechStatePolicy::IsWaitTerminal(m_context, m_exit.load())`.

## Verification Evidence
- Debug x64 build: 0 warnings, 0 errors (`/WX` compliant).
- Watchdog execution: All 53 focused tests passed in ~4.68s under the 30-second watchdog.
- Quiescence: `Assert-WorktreeMockProviderQuiesced` passed cleanly before and after the test run.
- Code hygiene: `git diff --check` emitted zero errors.

## Concerns
None. All 53 tests passed cleanly on the first run with zero regressions.
