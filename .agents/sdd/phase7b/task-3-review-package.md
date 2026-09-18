# Task 3 Review Package: Rewire Locked Classification and State Application

Base commit: 3198d3885d568c0b62e49c719e7825b2f293b680

Modified files:
- `CoreEngine/SpeechWorker.cpp`

Diff:
```diff
diff --git a/CoreEngine/SpeechWorker.cpp b/CoreEngine/SpeechWorker.cpp
index 1c8894e..9e36310 100644
--- a/CoreEngine/SpeechWorker.cpp
+++ b/CoreEngine/SpeechWorker.cpp
@@ -2,6 +2,7 @@
 #include "SpeechWorker.h"
 #include "SpeechWorkerTypes.h"
 #include "SpeechStatePolicy.h"
+#include "ControlEventPolicy.h"
 #include "SpeechProtocolUtils.h"
 #include "SapiEngine.h"
 #include "JsonValue.h"
@@ -1050,75 +1051,62 @@ SpeechWorker::ControlEventDisposition SpeechWorker::HandleParsedControlEventLock
     const ProviderControlEvent& event)
 {
     ControlEventDisposition disposition;
-    disposition.shouldForwardToSapi = (m_context.upstreamState != UpstreamState::Faulted &&
-                                       m_context.downstreamState != DownstreamState::Faulted &&
-                                       !m_context.faultPending);
+    const auto decision = ControlEventPolicy::EvaluateParsedEvent(event, m_context);
+    disposition.shouldForwardToSapi = decision.shouldForwardToSapi;

-    if (event.speakId == m_context.token.speakId)
+    if (decision.shouldRefreshProgress)
     {
-        const bool hasValidPayload = (!event.IsSpeechBoundary() || event.hasValidSpeechOffsets) &&
-                                     (!event.IsTerminal() || event.hasValidTerminalBytes);
-        if (event.IsProgress() && hasValidPayload &&
-            (m_context.downstreamState == DownstreamState::Speaking || m_context.IsDrainingCancellation()))
-        {
-            m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
-        }
+        m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
+    }

-        if (event.IsSpeechBoundary())
-        {
-            if (!event.hasValidSpeechOffsets)
-            {
-                disposition.shouldForwardToSapi = false;
-                disposition.shouldEnterFaultedState = true;
-            }
-            else if (m_context.downstreamState != DownstreamState::Speaking)
-            {
-                // SAPI has aborted this request, so delayed provider callbacks must not move focus.
-                disposition.shouldForwardToSapi = false;
-            }
-        }
+    switch (decision.action)
+    {
+    case ControlEventPolicy::EventAction::NoLockedAction:
+    case ControlEventPolicy::EventAction::SpeechBoundary:
+    {
+        break;
+    }

-        switch (event.type)
-        {
-        case ProviderEventType::WordBoundary:
-        case ProviderEventType::SentenceBoundary:
-        case ProviderEventType::Bookmark:
-        {
-            break;
-        }
+    case ControlEventPolicy::EventAction::MalformedSpeechBoundary:
+    {
+        disposition.shouldEnterFaultedState = true;
+        break;
+    }

-        case ProviderEventType::SynthesisComplete:
-        case ProviderEventType::SynthesisCancelled:
-        {
-            disposition.shouldEnterFaultedState = HandleTerminalEventLocked(
-                event.type, event.speakId, event.terminalAudioBytes, event.hasValidTerminalBytes, event.rawEventName);
-            break;
-        }
+    case ControlEventPolicy::EventAction::SynthesisComplete:
+    case ControlEventPolicy::EventAction::SynthesisCancelled:
+    {
+        disposition.shouldEnterFaultedState = HandleTerminalEventLocked(
+            event.type, event.speakId, event.terminalAudioBytes, event.hasValidTerminalBytes, event.rawEventName);
+        break;
+    }

-        case ProviderEventType::LegacyCompleted:
-        {
-            CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", event.speakId);
-            break;
-        }
+    case ControlEventPolicy::EventAction::LegacyCompleted:
+    {
+        CoreLog(L"[SpeechWorker] Ignoring legacy completed event for speak_id %llu.", event.speakId);
+        break;
+    }

-        case ProviderEventType::Log:
-        {
-            disposition.shouldEnterFaultedState = HandleLogEventLocked(event.speakId, event.logSeverity, event.logMessage);
-            break;
-        }
+    case ControlEventPolicy::EventAction::InformationalLog:
+    case ControlEventPolicy::EventAction::RequestErrorLog:
+    case ControlEventPolicy::EventAction::FatalLog:
+    {
+        disposition.shouldEnterFaultedState = HandleLogEventLocked(
+            event.speakId, event.logSeverity, event.logMessage);
+        break;
+    }

-        case ProviderEventType::Unknown:
-        {
-            CoreLog(L"[SpeechWorker] Unknown event received: %.*hs",
-                static_cast<int>(event.rawEventName.size()), event.rawEventName.data());
-            break;
-        }
-        }
+    case ControlEventPolicy::EventAction::Unknown:
+    {
+        CoreLog(L"[SpeechWorker] Unknown event received: %.*hs",
+            static_cast<int>(event.rawEventName.size()), event.rawEventName.data());
+        break;
+    }
+    }

-        if (disposition.shouldEnterFaultedState)
-        {
-            m_context.faultPending = true;
-        }
+    if (disposition.shouldEnterFaultedState)
+    {
+        m_context.faultPending = true;
     }

     return disposition;
```

Verification Evidence:
- Debug x64 build: 0 errors, 0 warnings.
- Focused filter (81 tests) under 30s watchdog: 81/81 passed (in ~4.1s).
- MockProvider quiescence: verified clean.
