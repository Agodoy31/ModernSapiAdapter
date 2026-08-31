# Design Specification: RequestContext Encapsulation & Concurrency Hardening

**Date:** 2026-08-31  
**Status:** APPROVED  
**Standard Conformance:** `STD-ARCH-2026-01` (`NLLE-01`–`NLLE-05`, `PMOD-01`–`PMOD-05`, `BMVP-01`–`BMVP-04`)  
**Scope:** `CoreEngine` (`SpeechWorkerTypes.h`, `SpeechWorker.h`, `SpeechWorker.cpp`, `SapiEngine.cpp`, `pch.cpp`, `CoreEngine.Tests`)

---

## 1. Problem Statement & Motivation

Prior iterations modularized file boundaries, event parsing, and loop decompositions in `CoreEngine`. However, the internal state of `SpeechWorker` remained represented as loose, independent member fields (`m_activeSpeakId`, `m_rawAudioBytesRead`, `m_deliveredAudioBytes`, `m_upstreamTerminalBytes`, `m_upstreamState`, `m_downstreamState`, `m_upstreamFinished`, `m_faultPending`, `m_cancellationDeadlineTick`, `m_requestCompletionHr`).

This loose representation created two specific concurrency and lifecycle risks:

1. **Unbound Audio Batch Delivery Race:**
   `AudioThreadProc` assembled audio frames under `m_requestMutex`, released the mutex to invoke SAPI's COM `ISpTTSEngineSite::Write`, and reacquired `m_requestMutex` to update delivery totals. If an asynchronous cancellation reset the worker to `Idle` and a subsequent `Speak` request began while the previous SAPI write was in-flight, the completed batch from request $N$ would be erroneously credited to request $N+1$.

2. **Terminal Inactivity Watchdog Gap:**
   `SpeechWorker::WaitUntilFinished` checked for provider inactivity timeouts only while `m_upstreamState == UpstreamState::Active`. When `synthesis_complete` transitioned upstream state to `UpstreamState::Completed`, downstream remained in `DownstreamState::Speaking` until all audio was drained. If a provider failed to send all declared audio bytes or disconnected without fault publication, `WaitUntilFinished` polled indefinitely without triggering an inactivity timeout.

3. **Format & Dead Code Hygiene:**
   `CoreLog` in `CoreEngine/pch.cpp` reused `va_list` across `_vscwprintf` and `_vsnwprintf_s` without `va_copy`, and passed an off-by-one buffer size. `DownstreamState::Drained` and unused function parameters (`void* pSite` in `SpeechWorker::Start`) persisted from legacy iterations.

---

## 2. Proposed Architecture & Component Design

### 2.1 Domain Value Types (`SpeechWorkerTypes.h`)

We introduce two pure, Level-0 domain types with zero COM/SAPI dependencies:

```cpp
#pragma once
#include <cstdint>
#include <string_view>
#include <winerror.h>

/**
 * @struct RequestToken
 * @brief Immutable identity token binding audio chunks and control events to a specific request generation.
 */
struct RequestToken
{
    uint64_t speakId = 0;
    uint64_t generation = 0;

    [[nodiscard]] constexpr bool IsValid() const noexcept
    {
        return speakId != 0 && generation != 0;
    }

    [[nodiscard]] constexpr bool Matches(const RequestToken& other) const noexcept
    {
        return speakId == other.speakId && generation == other.generation;
    }
};

/**
 * @struct RequestContext
 * @brief Cohesive value struct encapsulating the discrete mutable state of an active utterance.
 */
struct RequestContext
{
    RequestToken token{};
    UpstreamState upstreamState = UpstreamState::Idle;
    DownstreamState downstreamState = DownstreamState::Idle;
    uint64_t rawAudioBytesRead = 0;
    uint64_t deliveredAudioBytes = 0;
    uint64_t upstreamTerminalBytes = 0;
    bool upstreamFinished = false;
    bool faultPending = false;
    ULONGLONG cancellationDeadlineTick = 0;
    HRESULT completionHr = S_OK;

    void Reset() noexcept
    {
        token.speakId = 0;
        upstreamState = UpstreamState::Idle;
        downstreamState = DownstreamState::Idle;
        rawAudioBytesRead = 0;
        deliveredAudioBytes = 0;
        upstreamTerminalBytes = 0;
        upstreamFinished = false;
        faultPending = false;
        cancellationDeadlineTick = 0;
        completionHr = S_OK;
    }
};
```

### 2.2 Generation-Bound Audio Pipeline

The audio ingest and delivery pipeline is refactored to pass `RequestToken` explicitly:

1. **`AudioIngestResult`:**
   ```cpp
   struct AudioIngestResult
   {
       RequestToken token{};
       bool protocolBoundaryFailed = false;
       bool shouldDeliverAudio = false;
       PcmFrameBatch spansToWrite{};
   };
   ```

2. **Ingest Phase (`SpeechWorker::IngestAudioChunkLocked`):**
   - Captures `result.token = m_context.token`.
   - Assembles frames into `result.spansToWrite`.
   - Advances `m_context.rawAudioBytesRead`.

3. **Out-of-Mutex SAPI Delivery:**
   - Calls `m_pEngine->OnAudioData(...)` with raw byte spans outside `m_requestMutex`.

4. **Delivery Verification Phase (`SpeechWorker::UpdateAfterAudioDeliveryLocked`):**
   - Takes `const RequestToken& batchToken, size_t deliveredBytes, bool writeAccepted, uint64_t& outCancellationToSend`.
   - Validates `if (!m_context.token.Matches(batchToken)) { return false; }`.
   - If token matches and write was accepted, adds `deliveredBytes` to `m_context.deliveredAudioBytes` and evaluates terminal boundaries.
   - If token mismatch is detected (due to intercurrent cancellation and new utterance start), the stale delivery result is discarded safely without mutating the new request.

### 2.3 Comprehensive Watchdog Liveness (`SpeechWorker::WaitUntilFinished`)

The polling loop in `WaitUntilFinished` is hardened against stalled terminal draining:

```cpp
const bool isActivelySynthesizing = (m_context.upstreamState == UpstreamState::Active);
const bool isAwaitingTerminalAudio = (m_context.upstreamFinished && m_context.downstreamState == DownstreamState::Speaking);

if ((isActivelySynthesizing || isAwaitingTerminalAudio) &&
    HasSynthesisInactivityTimedOut(now, lastProgress, SynthesisInactivityTimeoutMs))
{
    const uint64_t speakId = m_context.token.speakId;
    lock.unlock();
    CoreLog(L"[SpeechWorker] Provider made no progress for %llu ms during speak_id %llu (upstreamFinished=%d); quarantining session.",
        SynthesisInactivityTimeoutMs, speakId, isAwaitingTerminalAudio ? 1 : 0);
    EnterFaultedState();
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
```

### 2.4 Implementation Hygiene Fixes

1. **`CoreLog` Formatting (`CoreEngine/pch.cpp`):**
   ```cpp
   void CoreLog(const wchar_t* fmt, ...)
   {
   #ifdef _DEBUG
       va_list args;
       va_start(args, fmt);

       va_list argsCopy;
       va_copy(argsCopy, args);
       const int len = _vscwprintf(fmt, argsCopy);
       va_end(argsCopy);

       if (len > 0)
       {
           std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
           _vsnwprintf_s(&buffer[0], buffer.size(), _TRUNCATE, fmt, args);
           buffer.resize(static_cast<size_t>(len));
           AsyncLogger::GetInstance().Log(buffer);
       }

       va_end(args);
   #else
       (void)fmt;
   #endif
   }
   ```
2. **Dead Code Elimination:**
   - Remove `DownstreamState::Drained` from `SpeechWorkerTypes.h` and exhaustive switches.
   - Update `SpeechWorker::Start(uint64_t speakId)` signature to remove unused `void* pSite`.

---

## 3. Verification Plan

### 3.1 Automated Regression Testing
- Execute targeted MSBuild on `CoreEngine` and `CoreEngine_Tests` (Debug x64 & Release x64).
- Verify all 84 Debug unit tests and 61 Release unit tests pass with zero failures.

### 3.2 Concurrency & Stale Batch Regression Test
- Verify in `AudioStreamingTests.cpp` and `CancellationTests.cpp` that in-flight audio delivery from an aborted request does not cross-talk or corrupt subsequent utterances.
- Verify in `FaultRecoveryTests.cpp` that stalled terminal draining triggers a clean `ERROR_TIMEOUT` and session recovery rather than hanging indefinitely.
