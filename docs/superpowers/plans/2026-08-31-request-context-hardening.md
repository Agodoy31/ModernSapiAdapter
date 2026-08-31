# RequestContext Encapsulation & Concurrency Hardening Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Encapsulate loose request fields in `SpeechWorker` into a cohesive generation-bound `RequestContext`, tag audio deliveries to prevent cross-request batch corruption, and harden watchdog liveness for stalled terminal audio draining.

**Architecture:** Introduce Level-0 `RequestToken` and `RequestContext` value structs in `SpeechWorkerTypes.h`. Bind every audio batch ingest/delivery to `RequestToken` so out-of-mutex SAPI writes cannot contaminate subsequent utterances. Extend the `WaitUntilFinished` progress watchdog to cover terminal audio draining. Fix `CoreLog` string formatting and remove unused legacy parameters.

**Tech Stack:** C++20, Win32 / SAPI 5 COM, Windows Implementation Library (WIL), GoogleTest, MSBuild (x64 / ARM64).

## Global Constraints
- Target Environment: Windows 11 strictly 64-bit (`x64` / `ARM64`).
- Standards: `STD-ARCH-2026-01` (`NLLE-01`–`NLLE-05`, `PMOD-01`–`PMOD-05`, `BMVP-01`–`BMVP-04`, `BTW-01`–`BTW-03`).
- Screen Reader Accessibility: Maximum indentation depth $\le 2$, explicit multi-line `{ ... }` braces on all control flow blocks.
- Threading Invariant: Zero SAPI COM calls (`pOutputSite->Write`, `pOutputSite->AddEvents`, `pOutputSite->GetActions`) held under `m_requestMutex` or `m_sessionMutex`.

---

### Task 1: Domain Value Types (`RequestToken`, `RequestContext`) & Implementation Hygiene

**Files:**
- Modify: `CoreEngine/SpeechWorkerTypes.h`
- Modify: `CoreEngine/pch.h`
- Modify: `CoreEngine/pch.cpp`
- Test: `CoreEngine.Tests/SessionLifecycleTests.cpp`

**Interfaces:**
- Consumes: Standard types `<cstdint>`, `<string_view>`.
- Produces: `struct RequestToken`, `struct RequestContext`, `enum class UpstreamState`, `enum class DownstreamState` (without `Drained`).

- [ ] **Step 1: Update `SpeechWorkerTypes.h` with `RequestToken` and `RequestContext`**
  Add `RequestToken` and `RequestContext` to `CoreEngine/SpeechWorkerTypes.h`. Remove `DownstreamState::Drained`.

```cpp
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

- [ ] **Step 2: Fix `CoreLog` string formatting in `pch.cpp`**
  Use `va_copy` to safely compute string length before `_vsnwprintf_s`, and allocate `len + 1` characters:

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

- [ ] **Step 3: Compile and verify Task 1**
  Run: `& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" "ModernSapiAdapter.slnx" /p:Configuration=Debug /p:Platform=x64 /t:CoreEngine /m`

---

### Task 2: Encapsulate State into `m_context` & Generation-Bound Audio Batch Pipeline

**Files:**
- Modify: `CoreEngine/SpeechWorker.h`
- Modify: `CoreEngine/SpeechWorker.cpp`
- Modify: `CoreEngine/SapiEngine.cpp`

**Interfaces:**
- Consumes: `RequestToken`, `RequestContext` from `SpeechWorkerTypes.h`.
- Produces: `SpeechWorker::Start(uint64_t speakId)`, `AudioIngestResult` (with `RequestToken`), generation-bound `UpdateAfterAudioDeliveryLocked`.

- [ ] **Step 1: Update `SpeechWorker.h`**
  - Replace loose member variables (`m_activeSpeakId`, `m_rawAudioBytesRead`, `m_deliveredAudioBytes`, `m_upstreamTerminalBytes`, `m_upstreamState`, `m_downstreamState`, `m_upstreamFinished`, `m_faultPending`, `m_cancellationDeadlineTick`, `m_requestCompletionHr`) with `RequestContext m_context;` and `uint64_t m_generationCounter{0};`.
  - Update `AudioIngestResult` to include `RequestToken token{};`.
  - Update `UpdateAfterAudioDeliveryLocked` to accept `const RequestToken& batchToken`.
  - Change `Start` signature to `bool Start(uint64_t speakId);`.

- [ ] **Step 2: Update `SpeechWorker.cpp` implementation**
  - In `SpeechWorker::Start(uint64_t speakId)`:
    ```cpp
    bool SpeechWorker::Start(uint64_t speakId)
    {
        std::lock_guard<std::mutex> lock(m_requestMutex);
        if (m_context.upstreamState != UpstreamState::Idle ||
            m_context.downstreamState != DownstreamState::Idle ||
            m_context.faultPending)
        {
            return false;
        }

        m_context.Reset();
        m_context.token.speakId = speakId;
        m_context.token.generation = ++m_generationCounter;
        m_frameAssembler.Reset();
        m_lastProviderProgressTick.store(GetTickCount64(), std::memory_order_release);
        m_context.upstreamState = UpstreamState::Active;
        m_context.downstreamState = DownstreamState::Speaking;
        return true;
    }
    ```
  - In `SpeechWorker::IngestAudioChunkLocked`:
    Stamp `result.token = m_context.token;`.
  - In `SpeechWorker::UpdateAfterAudioDeliveryLocked`:
    ```cpp
    if (!m_context.token.Matches(batchToken))
    {
        // Stale audio delivery from an aborted request; safely ignore without mutating new request state.
        return false;
    }
    ```
  - In `SpeechWorker::ControlThreadProc`:
    Match event `speak_id` with `m_context.token.speakId`.
  - In `SapiEngine.cpp:Speak`:
    Update `worker->Start(speakId)` call.

- [ ] **Step 3: Compile and run test suite**
  Run: `& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" "ModernSapiAdapter.slnx" /p:Configuration=Debug /p:Platform=x64 "/t:CoreEngine;CoreEngine_Tests" /m`
  Run: `& "bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe"`

---

### Task 3: Watchdog Liveness Hardening in `WaitUntilFinished`

**Files:**
- Modify: `CoreEngine/SpeechWorker.cpp`

**Interfaces:**
- Consumes: `m_context.upstreamFinished`, `m_context.downstreamState`, `m_context.upstreamState`.
- Produces: Hardened timeout detection for stalled terminal draining.

- [ ] **Step 1: Update `WaitUntilFinished` polling loop**
  Update `WaitUntilFinished` so `HasSynthesisInactivityTimedOut` monitors both active synthesis and pending terminal audio draining:

```cpp
const bool isActivelySynthesizing = (m_context.upstreamState == UpstreamState::Active);
const bool isAwaitingTerminalAudio = (m_context.upstreamFinished && m_context.downstreamState == DownstreamState::Speaking);

if ((isActivelySynthesizing || isAwaitingTerminalAudio) &&
    HasSynthesisInactivityTimedOut(now, lastProgress, SynthesisInactivityTimeoutMs))
{
    const uint64_t speakId = m_context.token.speakId;
    lock.unlock();
    CoreLog(L"[SpeechWorker] Provider made no progress for %llu ms during speak_id %llu (awaiting_terminal=%d); quarantining session.",
        SynthesisInactivityTimeoutMs, speakId, isAwaitingTerminalAudio ? 1 : 0);
    EnterFaultedState();
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}
```

- [ ] **Step 2: Compile and run test suite**
  Run: `& "bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe"`

---

### Task 4: Unit Test Suite Hardening & New Concurrency Regression Tests

**Files:**
- Modify: `CoreEngine.Tests/AudioStreamingTests.cpp`
- Modify: `CoreEngine.Tests/FaultRecoveryTests.cpp`

**Interfaces:**
- Tests: `AudioStreamingTests`, `FaultRecoveryTests`.

- [ ] **Step 1: Add Stale Audio Batch Rejection Test (`AudioStreamingTests.cpp`)**
  Verify that when an in-flight SAPI write completes after an abort, the stale batch does not increment the delivered byte counter of the next request.

- [ ] **Step 2: Add Stalled Terminal Audio Drain Timeout Test (`FaultRecoveryTests.cpp`)**
  Verify that if a provider sends `synthesis_complete` with `total_audio_bytes: 10000` but stops sending audio chunks after 2,000 bytes, `WaitUntilFinished` times out after 1,500ms and returns `HRESULT_FROM_WIN32(ERROR_TIMEOUT)` with faulted session recovery.

- [ ] **Step 3: Run Full Dual Debug / Release Test Suite**
  Debug: `& "bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe"` (Verify all pass).
  Release: `& "bin\CoreEngine.Tests\x64\Release\CoreEngine.Tests.exe"` (Verify all pass).
