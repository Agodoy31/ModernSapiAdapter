# Task 4 Brief: Rewire Final Callback Admission

## Objectives
1. In `CoreEngine/SpeechWorker.cpp`:
   - In `SpeechWorker::ForwardEventToSapi(const ProviderControlEvent& event)`:
     - Preserve the exact staging:
       ```cpp
       {
           std::lock_guard<std::mutex> eventForwardLock(m_eventForwardMutex);
           if (m_faultVisible.load(std::memory_order_acquire))
           {
               return;
           }

           if (event.speakId == 0)
           {
               return;
           }

           std::lock_guard<std::mutex> requestLock(m_requestMutex);
           if (ControlEventPolicy::EvaluateFinalAdmission(event, m_context) != ControlEventPolicy::FinalAdmission::Allow)
           {
               return;
           }
       }
       ```
     - Remove the local definition of `SpeechWorker::ShouldForwardEventLocked`.
2. In `CoreEngine/SpeechWorker.h`:
   - Remove the obsolete declaration of `ShouldForwardEventLocked`.
3. Hygiene & Verification:
   - Strict Allman bracing style across modified code.
   - Build Debug x64: `CoreEngine.Tests.vcxproj`.
   - Run focused filter (81 tests) under 30s watchdog.
     `$focusedFilter = "ControlEventPolicyTests.*:$baselineFocusedFilter`:SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault"`
   - Assert all 81 tests pass.
   - Verify MockProvider quiescence.
