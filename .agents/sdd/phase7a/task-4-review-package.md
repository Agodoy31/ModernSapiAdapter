# Task 4 Review Package: Rewire Terminal, Failure, Boundary, and Timeout Lifecycle

## Diffstat
```text
 CoreEngine/SpeechWorker.cpp | 114 ++++++++++++++++++--------------------------
 1 file changed, 48 insertions(+), 66 deletions(-)
```

## Full Diff
```diff
diff --git a/CoreEngine/SpeechWorker.cpp b/CoreEngine/SpeechWorker.cpp
index 68863b5..f26e4e5 100644
--- a/CoreEngine/SpeechWorker.cpp
+++ b/CoreEngine/SpeechWorker.cpp
@@ -522,25 +522,24 @@ HRESULT SpeechWorker::WaitUntilFinished(ISpTTSEngineSite* pOutputSite)
             lastTerminalWaitLogTick = now;
         }
 #endif
-        if (m_context.IsDrainingCancellation() &&
-            HasCancellationTimedOut(now, m_context.cancellationDeadlineTick))
-        {
-            const uint64_t speakId = m_context.token.speakId;
-            lock.unlock();
-            CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
-                CancellationTimeoutMs, speakId);
-            EnterFaultedState();
-            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
-        }
-
         const ULONGLONG lastProgress = m_lastProviderProgressTick.load(std::memory_order_acquire);
-        const bool isActivelySynthesizing = m_context.IsActivelySynthesizing();
         const bool isAwaitingTerminalAudio = m_context.IsAwaitingTerminalAudio();

-        if ((isActivelySynthesizing || isAwaitingTerminalAudio) &&
-            HasSynthesisInactivityTimedOut(now, lastProgress, SynthesisInactivityTimeoutMs))
-        {
-            const uint64_t speakId = m_context.token.speakId;
+        const auto timeoutDecision = SpeechStatePolicy::EvaluateTimeouts(
+            m_context, now, lastProgress, SynthesisInactivityTimeoutMs);
+
+        if (timeoutDecision.condition == SpeechStatePolicy::TimeoutCondition::CancellationTimeout)
+        {
+            const uint64_t speakId = timeoutDecision.speakId;
+            lock.unlock();
+            CoreLog(L"[SpeechWorker] Provider did not complete cancellation within %lu ms for speak_id %llu; quarantining session.",
+                CancellationTimeoutMs, speakId);
+            EnterFaultedState();
+            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
+        }
+        else if (timeoutDecision.condition == SpeechStatePolicy::TimeoutCondition::InactivityTimeout)
+        {
+            const uint64_t speakId = timeoutDecision.speakId;
             lock.unlock();
             CoreLog(L"[SpeechWorker] Provider made no progress for %llu ms during speak_id %llu (awaiting_terminal=%d); quarantining session.",
                 SynthesisInactivityTimeoutMs, speakId, isAwaitingTerminalAudio ? 1 : 0);
@@ -626,9 +625,7 @@ bool SpeechWorker::IsCancellingTerminalReachedLocked() const noexcept

 bool SpeechWorker::IsWaitTerminalLocked() const noexcept
 {
-    return m_context.downstreamState == DownstreamState::Idle ||
-           m_context.downstreamState == DownstreamState::Faulted ||
-           m_exit.load();
+    return SpeechStatePolicy::IsWaitTerminal(m_context, m_exit.load());
 }

 bool SpeechWorker::ShouldForwardEventLocked(uint64_t speakId, bool isLog) const noexcept
@@ -669,48 +666,38 @@ void SpeechWorker::ResetToIdleLocked() noexcept

 bool SpeechWorker::CheckTerminalBoundaryLocked()
 {
-    if (m_context.upstreamState == UpstreamState::Failed)
-    {
-        ResetToIdleLocked();
-        return false;
-    }
-
-    if (!m_context.upstreamFinished)
-    {
-        return false;
-    }
-
-    switch (m_context.downstreamState)
-    {
-    case DownstreamState::Speaking:
-    {
-        if (HasSpeakingAudioOverrunLocked())
-        {
-            CoreLog(L"[SpeechWorker] Provider audio overrun: raw=%llu delivered=%llu declared=%llu",
-                m_context.rawAudioBytesRead, m_context.deliveredAudioBytes, m_context.upstreamTerminalBytes);
-            return true;
-        }
-        else if (IsSpeakingTerminalReachedLocked())
-        {
-#if defined(_DEBUG)
-            CoreLog(L"[ThreadTrace] speak_id=%llu terminal_boundary_reached tick=%llu declared=%llu raw=%llu delivered=%llu.",
-                m_context.token.speakId, GetTickCount64(), m_context.upstreamTerminalBytes,
-                m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
-#endif
-            ResetToIdleLocked();
-            return false;
-        }
-        else
-        {
-            return false;
-        }
-    }
-
-    case DownstreamState::Cancelling:
-    {
-        if (IsCancellingTerminalReachedLocked())
-        {
-#if defined(_DEBUG)
+    SpeechStatePolicy::TerminalBoundaryFacts facts{};
+    facts.speakingAudioOverrun = HasSpeakingAudioOverrunLocked();
+    facts.speakingTerminalReached = IsSpeakingTerminalReachedLocked();
+    facts.cancellationTerminalReached = IsCancellingTerminalReachedLocked();
+
+    const auto decision = SpeechStatePolicy::EvaluateTerminalBoundary(m_context, facts);
+    switch (decision.action)
+    {
+    case SpeechStatePolicy::TerminalBoundaryAction::Continue:
+        return false;
+
+    case SpeechStatePolicy::TerminalBoundaryAction::UtteranceFailedReset:
+        ResetToIdleLocked();
+        return false;
+
+    case SpeechStatePolicy::TerminalBoundaryAction::OverrunFault:
+        CoreLog(L"[SpeechWorker] Provider audio overrun: raw=%llu delivered=%llu declared=%llu",
+            m_context.rawAudioBytesRead, m_context.deliveredAudioBytes, m_context.upstreamTerminalBytes);
+        return true;
+
+    case SpeechStatePolicy::TerminalBoundaryAction::NormalCompleteReset:
+#if defined(_DEBUG)
+        CoreLog(L"[ThreadTrace] speak_id=%llu terminal_boundary_reached tick=%llu declared=%llu raw=%llu delivered=%llu.",
+            m_context.token.speakId, GetTickCount64(), m_context.upstreamTerminalBytes,
+            m_context.rawAudioBytesRead, m_context.deliveredAudioBytes);
+#endif
+        ResetToIdleLocked();
+        return false;
+
+    case SpeechStatePolicy::TerminalBoundaryAction::CancelDrainReset:
+#if defined(_DEBUG)
+        {
             const ULONGLONG cancellationStartTick = m_context.cancellationDeadlineTick >= CancellationTimeoutMs
                 ? m_context.cancellationDeadlineTick - CancellationTimeoutMs
                 : 0;
@@ -718,18 +705,9 @@ bool SpeechWorker::CheckTerminalBoundaryLocked()
                 m_context.token.speakId, GetTickCount64(),
                 cancellationStartTick == 0 ? 0 : GetTickCount64() - cancellationStartTick,
                 m_context.upstreamTerminalBytes, m_context.rawAudioBytesRead);
-#endif
-            ResetToIdleLocked();
-            return false;
-        }
-        else
-        {
-            return false;
-        }
-    }
-
-    case DownstreamState::Idle:
-    case DownstreamState::Faulted:
+        }
+#endif
+        ResetToIdleLocked();
         return false;
     }

@@ -970,40 +948,51 @@ bool SpeechWorker::HandleTerminalEventLocked(
     bool hasValidTerminalBytes,
     std::string_view eventStr)
 {
-    if (m_context.upstreamFinished)
-    {
+    const bool isFrameAligned = (m_frameAssembler.BlockAlign() != 0) &&
+        (terminalAudioBytes % m_frameAssembler.BlockAlign() == 0);
+    const auto kind = (eventType == ProviderEventType::SynthesisComplete)
+        ? SpeechStatePolicy::UpstreamTerminalKind::Completed
+        : SpeechStatePolicy::UpstreamTerminalKind::Cancelled;
+
+    const auto decision = SpeechStatePolicy::EvaluateUpstreamTerminal(
+        m_context,
+        kind,
+        terminalAudioBytes,
+        hasValidTerminalBytes,
+        isFrameAligned);
+
+    switch (decision.action)
+    {
+    case SpeechStatePolicy::UpstreamTerminalAction::DuplicateFault:
         CoreLog(L"[SpeechWorker] Duplicate terminal event for speak_id %llu.", eventSpeakId);
         TransitionRequestToFaultedLocked();
         return true;
-    }
-
-    if (!hasValidTerminalBytes)
-    {
+
+    case SpeechStatePolicy::UpstreamTerminalAction::InvalidBytesFault:
         CoreLog(L"[SpeechWorker] terminal event for speak_id %llu has an invalid audio bytes value.", eventSpeakId);
         TransitionRequestToFaultedLocked();
         return true;
-    }
-
-    if (terminalAudioBytes % m_frameAssembler.BlockAlign() != 0)
-    {
+
+    case SpeechStatePolicy::UpstreamTerminalAction::MisalignedBytesFault:
         CoreLog(L"[SpeechWorker] terminal event for speak_id %llu is not PCM-frame aligned.", eventSpeakId);
         TransitionRequestToFaultedLocked();
         return true;
-    }
-
-    m_context.upstreamTerminalBytes = terminalAudioBytes;
-    m_context.upstreamFinished = true;
-    m_context.upstreamState = (eventType == ProviderEventType::SynthesisComplete)
-        ? UpstreamState::Completed
-        : UpstreamState::Cancelled;
+
+    case SpeechStatePolicy::UpstreamTerminalAction::Apply:
+        m_context.upstreamTerminalBytes = decision.terminalAudioBytes;
+        m_context.upstreamFinished = true;
+        m_context.upstreamState = decision.targetState;
 #if defined(_DEBUG)
         CoreLog(L"[ThreadTrace] speak_id=%llu terminal_received tick=%llu event=%hs declared=%llu raw=%llu delivered=%llu carry=%u downstream=%u.",
             eventSpeakId, GetTickCount64(), eventStr.data(), m_context.upstreamTerminalBytes,
             m_context.rawAudioBytesRead, m_context.deliveredAudioBytes,
             m_frameAssembler.HasCarry() ? 1u : 0u,
             static_cast<unsigned>(m_context.downstreamState));
 #endif
         return CheckTerminalBoundaryLocked();
+    }
+
+    return false;
 }

 bool SpeechWorker::HandleLogEventLocked(
@@ -1025,10 +1014,11 @@ bool SpeechWorker::HandleLogEventLocked(

     if (severity == "error")
     {
+        const auto decision = SpeechStatePolicy::EvaluateUtteranceFailure(m_context);
         m_context.upstreamState = UpstreamState::Failed;
         m_context.upstreamFinished = true;
         m_context.completionHr = E_FAIL;
-        m_context.upstreamTerminalBytes = m_context.rawAudioBytesRead;
+        m_context.upstreamTerminalBytes = decision.terminalAudioBytes;
         return CheckTerminalBoundaryLocked();
     }
```

## Concurrency & Invariant Verification
1. **Continuous Mutex Hold:** Fact capture, policy evaluation, and decision application remain under continuous `m_requestMutex` hold.
2. **Duplicate Terminal Precedence:** `EvaluateUpstreamTerminal` evaluates `upstreamFinished` first, intercepting duplicate terminal events before validating byte values or PCM alignment. This ensures duplicate diagnostics and two-stage fault sequences take precedence over malformed byte diagnostics.
3. **Timeout Precedence:** `EvaluateTimeouts` ensures draining cancellation expiry takes precedence over synthesis inactivity expiry.
4. **Out-of-Lock Operations:** Timeout error logging and `EnterFaultedState()` calls occur strictly after unlocking `lock.unlock()`.
5. **Focused Gate Evidence:**
   - 53/53 tests PASSED under 30-second watchdog.
   - MockProvider quiesced cleanly.
   - `git diff --check` emits zero whitespace errors.
