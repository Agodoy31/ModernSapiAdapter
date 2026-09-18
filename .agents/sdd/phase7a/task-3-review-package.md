# Task 3 Review Package: Rewire Request Admission, Stop, and Cancellation

## Diffstat
```text
 CoreEngine/SpeechWorker.cpp | 46 ++++++++++++++++++++-------------------------
 1 file changed, 20 insertions(+), 26 deletions(-)
```

## Full Diff
```diff
diff --git a/CoreEngine/SpeechWorker.cpp b/CoreEngine/SpeechWorker.cpp
index cb41971..68863b5 100644
--- a/CoreEngine/SpeechWorker.cpp
+++ b/CoreEngine/SpeechWorker.cpp
@@ -1,6 +1,7 @@
 #include "pch.h"
 #include "SpeechWorker.h"
 #include "SpeechWorkerTypes.h"
+#include "SpeechStatePolicy.h"
 #include "SpeechProtocolUtils.h"
 #include "SapiEngine.h"
 #include "JsonValue.h"
@@ -102,16 +103,16 @@ SpeechWorker::~SpeechWorker()
 bool SpeechWorker::Start(uint64_t speakId)
 {
     std::lock_guard<std::mutex> lock(m_requestMutex);
-    if (m_context.upstreamState != UpstreamState::Idle ||
-        m_context.downstreamState != DownstreamState::Idle ||
-        m_context.faultPending)
+    const auto decision = SpeechStatePolicy::EvaluateStart(m_context, speakId, m_generationCounter + 1);
+    if (decision.action != SpeechStatePolicy::StartAction::Accept)
     {
         return false;
     }
+
     m_context.Reset();
-    m_context.token.speakId = speakId;
-    m_context.token.generation = ++m_generationCounter;
+    m_generationCounter = decision.generation;
+    m_context.token.speakId = decision.speakId;
+    m_context.token.generation = decision.generation;
     m_frameAssembler.Reset();
     m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
     m_context.upstreamState = UpstreamState::Active;
@@ -251,22 +252,14 @@ void SpeechWorker::Stop()
     uint64_t speakId = 0;
     {
         std::lock_guard<std::mutex> lock(m_requestMutex);
-        if (m_context.downstreamState == DownstreamState::Idle)
+        const auto decision = SpeechStatePolicy::EvaluateStop(m_context);
+        if (decision.action == SpeechStatePolicy::StopAction::NoAction)
         {
             return;
         }
-
-        if (m_context.downstreamState == DownstreamState::Faulted ||
-            m_context.upstreamState == UpstreamState::Faulted)
-        {
-            return;
-        }
-
-        speakId = m_context.token.speakId;
-        m_frameAssembler.Reset();
-        m_context.upstreamState = UpstreamState::Idle;
-        m_context.downstreamState = DownstreamState::Idle;
-        m_requestChanged.notify_all();
+        speakId = decision.speakId;
+        ResetToIdleLocked();
     }
+
     SendCancellation(speakId, CancellationTimeoutMs);
@@ -301,31 +294,27 @@ HRESULT SpeechWorker::BeginCancellationLocked(ULONGLONG cancellationDeadline,
                                               ULONGLONG cancellationEntryTick,
                                               uint64_t& speakId)
 {
-    if (m_context.downstreamState == DownstreamState::Idle)
+    const auto decision = SpeechStatePolicy::EvaluateBeginCancellation(m_context, cancellationDeadline);
+    switch (decision.action)
     {
+    case SpeechStatePolicy::BeginCancellationAction::AlreadyIdle:
         return S_FALSE;
-    }
-
-    if (m_context.downstreamState == DownstreamState::Faulted ||
-        m_context.upstreamState == UpstreamState::Faulted)
-    {
-        return E_FAIL;
-    }
-
-    if (m_context.IsDrainingCancellation())
-    {
+    case SpeechStatePolicy::BeginCancellationAction::AlreadyCancelling:
         return E_UNEXPECTED;
-    }
-
-    speakId = m_context.token.speakId;
-    m_context.TransitionToCancelling(cancellationDeadline);
-    m_frameAssembler.Reset();
-#if defined(_DEBUG)
-    CoreLog(L"[CancelTrace] speak_id=%llu cancelling_published tick=%llu entry_to_publish_ms=%llu raw=%llu delivered=%llu.",
-        speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
-        m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
-#endif
-    return S_OK;
+    case SpeechStatePolicy::BeginCancellationAction::Faulted:
+        return E_FAIL;
+    case SpeechStatePolicy::BeginCancellationAction::TransitionToCancelling:
+        speakId = decision.speakId;
+        m_context.TransitionToCancelling(decision.deadlineTick);
+        m_frameAssembler.Reset();
+#if defined(_DEBUG)
+        CoreLog(L"[CancelTrace] speak_id=%llu cancelling_published tick=%llu entry_to_publish_ms=%llu raw=%llu delivered=%llu.",
+            speakId, GetTickCount64(), GetTickCount64() - cancellationEntryTick,
+            m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
+#endif
+        return S_OK;
+    }
+    return E_FAIL;
 }
+
 HRESULT SpeechWorker::FinishCancellation(uint64_t speakId,
@@ -854,9 +843,14 @@ bool SpeechWorker::UpdateAfterAudioDeliveryLocked(
         if (!writeAccepted)
         {
             CoreLog(L"[SpeechWorker] SAPI rejected an audio write; cancelling active synthesis.");
-            outCancellationToSend = m_context.token.speakId;
-            m_context.TransitionToCancelling(GetTickCount64() + CancellationTimeoutMs);
-            m_frameAssembler.Reset();
+            const auto decision = SpeechStatePolicy::EvaluateBeginCancellation(
+                m_context, GetTickCount64() + CancellationTimeoutMs);
+            if (decision.action == SpeechStatePolicy::BeginCancellationAction::TransitionToCancelling)
+            {
+                outCancellationToSend = decision.speakId;
+                m_context.TransitionToCancelling(decision.deadlineTick);
+                m_frameAssembler.Reset();
+            }
         }
         else
         {
```

## Concurrency & Invariant Verification
1. **Continuous Mutex Hold:** In all four rewired methods (`Start`, `Stop`, `BeginCancellationLocked`, and `UpdateAfterAudioDeliveryLocked`), fact capture, policy evaluation, and decision application are executed under a single continuous lock of `m_requestMutex`.
2. **Lock Order & Non-Blocking Evaluation:** No locks are taken inside `SpeechStatePolicy` functions (they are pure and stateless).
3. **Out-of-Lock IPC:** All named pipe operations (`SendCancellation`) remain strictly outside `m_requestMutex`.
4. **Behavior Preservation:**
   - In `Start`: full `m_context.Reset()` occurs on Accept; generation increment happens under lock; progress tick updated.
   - In `Stop`: partial reset via `ResetToIdleLocked()` preserves retained context fields while resetting the assembler and notifying waiters.
   - In `BeginCancellationLocked`: HRESULT mapping preserved; assembler reset; debug trace retained.
   - In `UpdateAfterAudioDeliveryLocked`: token validation and byte counters unaffected; write rejection triggers cancellation with full `CancellationTimeoutMs` sent asynchronously outside lock without synchronous wait.
5. **Focused Gate Evidence:**
   - 53/53 tests PASSED under 30-second watchdog.
   - MockProvider quiesced cleanly.
   - `git diff --check` emits zero whitespace errors.
