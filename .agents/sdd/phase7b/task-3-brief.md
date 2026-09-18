# Task 3 Brief: Rewire Locked Classification and State Application

## Objectives
1. In `CoreEngine/SpeechWorker.cpp`:
   - Include `ControlEventPolicy.h`.
   - In `SpeechWorker::HandleParsedControlEventLocked`:
     - Under the existing uninterrupted `m_requestMutex` lock hold, call:
       `const auto decision = ControlEventPolicy::EvaluateParsedEvent(event, m_context);`
     - Set `disposition.shouldForwardToSapi = decision.shouldForwardToSapi;`
     - Refresh progress:
       ```cpp
       if (decision.shouldRefreshProgress)
       {
           m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
       }
       ```
     - Switch on `decision.action`:
       - `EventAction::NoLockedAction`: do nothing (e.g. stale events).
       - `EventAction::SpeechBoundary`: do nothing (offsets already validated by policy).
       - `EventAction::MalformedSpeechBoundary`: set `disposition.shouldEnterFaultedState = true;`
       - `EventAction::SynthesisComplete`:
       - `EventAction::SynthesisCancelled`:
         call `HandleTerminalEventLocked(event.type, event.speakId, event.terminalAudioBytes, event.hasValidTerminalBytes, event.rawEventName);`
         and assign the result to `disposition.shouldEnterFaultedState`.
       - `EventAction::LegacyCompleted`:
         `CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", event.speakId);`
       - `EventAction::InformationalLog`:
       - `EventAction::RequestErrorLog`:
       - `EventAction::FatalLog`:
         call `HandleLogEventLocked(event.speakId, event.logSeverity, event.logMessage);`
         and assign the result to `disposition.shouldEnterFaultedState`.
       - `EventAction::Unknown`:
         `CoreLog(L"[SpeechWorker] Unknown event received: %.*hs", static_cast<int>(event.rawEventName.size()), event.rawEventName.data());`
     - If `disposition.shouldEnterFaultedState`:
       `m_context.faultPending = true;`
     - Return `disposition`.
2. In `CoreEngine/SpeechWorker.cpp` / `SpeechWorker.h`:
   - Keep `ControlThreadProc` empty-event and zero-speak_id guards unchanged.
   - Keep `HandleTerminalEventLocked`, `HandleLogEventLocked`, and `DispatchOrForwardEvent` intact.
   - Strict Allman bracing format.
3. Verification:
   - Build Debug x64: `CoreEngine.Tests.vcxproj`.
   - Run focused filter (81 tests) under 30s watchdog.
   - Note: In pwsh, escape the colon before SapiEngineTests:
     `$focusedFilter = "ControlEventPolicyTests.*:$baselineFocusedFilter`:SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault"`
   - Verify all 81 tests pass and MockProvider quiesces.
