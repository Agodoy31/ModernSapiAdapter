# Task 4 Review Package: Rewire Final Callback Admission

Base commit: 9163f1e913a968987b767d020d29486c91e018aa

Modified files:
- `CoreEngine/SpeechWorker.cpp`
- `CoreEngine/SpeechWorker.h`

Diff:
```diff
diff --git a/CoreEngine/SpeechWorker.cpp b/CoreEngine/SpeechWorker.cpp
index 9e36310..30c6555 100644
--- a/CoreEngine/SpeechWorker.cpp
+++ b/CoreEngine/SpeechWorker.cpp
@@ -437,10 +437,8 @@ void SpeechWorker::ForwardEventToSapi(const ProviderControlEvent& event)
             return;
         }

-        const bool isLog = (event.type == ProviderEventType::Log);
-
         std::lock_guard<std::mutex> requestLock(m_requestMutex);
-        if (!ShouldForwardEventLocked(event.speakId, isLog))
+        if (ControlEventPolicy::EvaluateFinalAdmission(event, m_context) != ControlEventPolicy::FinalAdmission::Allow)
         {
             return;
         }
@@ -630,26 +628,6 @@ bool SpeechWorker::IsWaitTerminalLocked() const noexcept
     return SpeechStatePolicy::IsWaitTerminal(m_context, m_exit.load());
 }

-bool SpeechWorker::ShouldForwardEventLocked(uint64_t speakId, bool isLog) const noexcept
-{
-    if (speakId != m_context.token.speakId)
-    {
-        return false;
-    }
-
-    if (isLog)
-    {
-        return true;
-    }
-
-    if (m_context.faultPending)
-    {
-        return false;
-    }
-
-    return m_context.downstreamState == DownstreamState::Speaking;
-}
-
 void SpeechWorker::ForwardTerminalAudioToSiteLocked()
 {
     if (!m_pEngine || !m_context.speakingOverrun)
diff --git a/CoreEngine/SpeechWorker.h b/CoreEngine/SpeechWorker.h
index e51e23f..8e31fc5 100644
--- a/CoreEngine/SpeechWorker.h
+++ b/CoreEngine/SpeechWorker.h
@@ -82,7 +82,6 @@ private:
     void HandleSpeechBoundaryLocked(const ProviderControlEvent& event);
     bool HandleTerminalEventLocked(ProviderEventType type, uint64_t speakId, uint64_t terminalAudioBytes, bool hasValidTerminalBytes, std::string_view rawEventName);
     bool HandleLogEventLocked(uint64_t speakId, std::string_view severity, std::string_view message);
-    bool ShouldForwardEventLocked(uint64_t speakId, bool isLog) const noexcept;
     void ForwardTerminalAudioToSiteLocked();
     bool CheckTerminalBoundaryLocked();
```

Verification Evidence:
- Debug x64 build: 0 errors, 0 warnings.
- Focused filter (81 tests) under 30s watchdog: 81/81 passed (in ~4.1s).
- MockProvider quiescence: verified clean.
