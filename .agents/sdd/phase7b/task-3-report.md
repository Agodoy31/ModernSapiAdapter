# Task 3 Implementation Report: Rewire Locked Classification and State Application

## Status: DONE

## Overview
Task 3 rewired the locked control event classification and state application pipeline in `CoreEngine/SpeechWorker.cpp` to consume the pure, allocation-free `ControlEventPolicy::EvaluateParsedEvent` leaf function implemented in Task 2. All lock holdings, timing clocks, diagnostics, terminal handling, request error logic, and fault transitions remain strictly owned by `SpeechWorker`, exactly preserving existing behavior and concurrency boundaries.

## Changes Made

1. **`CoreEngine/SpeechWorker.cpp`**:
   - Included `"ControlEventPolicy.h"`.
   - In `SpeechWorker::HandleParsedControlEventLocked`:
     - Preserved the existing uninterrupted `m_requestMutex` lock hold across event evaluation and state mutation.
     - Evaluated event using `const auto decision = ControlEventPolicy::EvaluateParsedEvent(event, m_context);`.
     - Assigned `disposition.shouldForwardToSapi = decision.shouldForwardToSapi;`.
     - Refreshed provider progress monotonic tick (`m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);`) strictly when `decision.shouldRefreshProgress` is true.
     - Switched on `decision.action`:
       - `EventAction::NoLockedAction`: No action taken (e.g. stale events or unhandled events).
       - `EventAction::SpeechBoundary`: No action taken (offsets validated by policy).
       - `EventAction::MalformedSpeechBoundary`: Set `disposition.shouldEnterFaultedState = true;`.
       - `EventAction::SynthesisComplete` / `EventAction::SynthesisCancelled`: Called `HandleTerminalEventLocked(event.type, event.speakId, event.terminalAudioBytes, event.hasValidTerminalBytes, event.rawEventName)` and assigned result to `disposition.shouldEnterFaultedState`.
       - `EventAction::LegacyCompleted`: Logged diagnostic `CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", event.speakId);`.
       - `EventAction::InformationalLog` / `EventAction::RequestErrorLog` / `EventAction::FatalLog`: Called `HandleLogEventLocked(event.speakId, event.logSeverity, event.logMessage)` and assigned result to `disposition.shouldEnterFaultedState`.
       - `EventAction::Unknown`: Logged diagnostic `CoreLog(L"[SpeechWorker] Unknown event received: %.*hs", static_cast<int>(event.rawEventName.size()), event.rawEventName.data());`.
     - Set `m_context.faultPending = true;` whenever `disposition.shouldEnterFaultedState` is true.
     - Returned `disposition`.
   - Kept `ControlThreadProc` empty-event and zero-`speak_id` guards intact.
   - Kept `HandleTerminalEventLocked`, `HandleLogEventLocked`, and `DispatchOrForwardEvent` intact.
   - Enforced strict Allman bracing across all modified code.

## Verification Evidence

1. **Targeted Build**:
   - Command: `& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" CoreEngine.Tests\CoreEngine.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:m`
   - Result: Exit code `0`, clean build with 0 errors and 0 warnings.

2. **Focused Filter Test Count**:
   - Command: `CoreEngine.Tests.exe --gtest_filter=$focusedFilter --gtest_list_tests`
   - Filter: `ControlEventPolicyTests.*:$baselineFocusedFilter`:SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault`
   - Result: Exactly 81 tests selected across 3 test cases (`SapiEngineTests` [50], `SpeechProtocolUtilsTests` [11], `ControlEventPolicyTests` [20]).

3. **30-Second Watchdog Execution**:
   - Executed under PowerShell fail-closed 30-second watchdog (`Invoke-WatchdogTest` with `Assert-WorktreeMockProviderQuiesced`):
     - ExitCode: `0`
     - TimedOut: `False`
     - Quiesced: `True`
     - Total Run Time: ~4161 - 4202 ms (test execution ~4056 ms)
     - Test Result: `[==========] 81 tests from 3 test cases ran. [  PASSED  ] 81 tests.`

4. **MockProvider Quiescence**:
   - Command: `Get-Process -Name MockProvider -ErrorAction SilentlyContinue`
   - Result: Exit code `1` (no active `MockProvider` process running).

5. **Code Hygiene & Clean Working Tree**:
   - `git diff --check` emitted 0 whitespace or formatting issues.
   - `git status --short` verified only `CoreEngine/SpeechWorker.cpp` modified.
