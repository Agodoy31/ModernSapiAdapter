# In-Flight SAPI Write Boundary Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prevent `synthesis_complete` from faulting a valid request while its final `ISpTTSEngineSite::Write` is still in progress.

**Architecture:** Preserve separate raw-pipe and delivered-to-SAPI byte counters. Add one request-mutex-protected boolean representing the single audio thread's active SAPI write batch, and make terminal validation wait for that batch before deciding whether a raw/delivered mismatch is stable.

**Tech Stack:** C++20, classic SAPI 5 COM, C++/WinRT, WIL, GoogleTest, Windows named pipes.

## Global Constraints

- Windows 11 only; x64 verification is required and x86 is banned.
- CoreEngine remains a headless unmanaged SAPI 5 COM proxy DLL.
- The provider IPC protocol, PCM format, SAPI buffering, event mapping, and cancellation timeouts must not change.
- Never hold `m_requestMutex` across `ISpTTSEngineSite::Write` or another external COM call.
- All new shared state is owned by `SpeechWorker` and accessed only while `m_requestMutex` is held.
- Heavy SDK and STL includes remain in the existing precompiled headers.
- The regression must deterministically reproduce the interleaving; arbitrary sleeps cannot be the synchronization mechanism.

---

### Task 1: Defer terminal validation while the final SAPI write is active

**Files:**
- Modify: `CoreEngine.Tests/MockSapiInterfaces.h`
- Modify: `CoreEngine.Tests/SapiEngineTests.cpp`
- Modify: `CoreEngine/SpeechWorker.h`
- Modify: `CoreEngine/SpeechWorker.cpp`

**Interfaces:**
- Consumes: `SpeechWorker::Start`, `SpeechWorker::WaitUntilFinished`, `ControlPipeTestServer`, `MockSpTTSEngineSite`, and the existing raw/delivered/terminal counters.
- Produces: a request-mutex-protected `bool m_sapiWriteBatchActive` and deterministic mock-site write-entry/release synchronization used only by tests.

- [ ] **Step 1: Add deterministic write blocking to the test double**

Extend `MockSpTTSEngineSite` with test-only methods equivalent to:

```cpp
void PauseNextWrite();
bool WaitForWritePause(DWORD timeoutMs);
void ReleaseWrite();
```

Protect the gate with its own mutex and condition variable. `Write` must announce entry and wait only when armed. Cleanup must be idempotent so a failed assertion can release and join safely. Keep the existing `writeDelayMs` behavior for older tests.

- [ ] **Step 2: Write the failing completion/write overlap regression**

Add `SapiEngineTests.SynthesisCompleteWaitsForFinalSapiWriteToFinish` using a four-byte aligned PCM request:

```cpp
mockSite->PauseNextWrite();
ASSERT_TRUE(server.WriteAudio({ 0x10, 0x20, 0x30, 0x40 }));
ASSERT_TRUE(mockSite->WaitForWritePause(1000));
ASSERT_TRUE(server.WriteControl(
    "{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n"));
```

Run `WaitUntilFinished` on a joined thread with condition-based completion tracking. Before releasing the write, assert that the wait has not returned and that the worker has not faulted. Release the write, join without detaching, then assert `S_OK`, four accepted bytes, and a non-faulted worker. Use a scope guard that releases the mock gate before joining on every failure path.

- [ ] **Step 3: Run the focused test and verify RED**

Build the x64 Debug test project, then run:

```powershell
bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe --gtest_filter=SapiEngineTests.SynthesisCompleteWaitsForFinalSapiWriteToFinish
```

Expected before the production fix: the wait returns `E_FAIL` while the mock write is paused because terminal validation sees `raw == declared` and `delivered < declared`.

- [ ] **Step 4: Implement the minimum synchronized production fix**

Add this state beside the existing request counters in `SpeechWorker.h`:

```cpp
bool m_sapiWriteBatchActive = false;
```

Document why `m_requestMutex` protects it. In `AudioThreadProc`, set it under `m_requestMutex` when a non-empty framed batch is committed to downstream delivery, before releasing the mutex for `OnAudioData`. Clear it immediately after reacquiring the mutex following the batch, before updating delivered bytes and calling `CheckTerminalBoundaryLocked`.

In `CheckTerminalBoundaryLocked`, retain successful completion when declared, raw, and delivered totals match with no carry. A mismatch at `raw == declared` is a fault only when `m_sapiWriteBatchActive` is false. Provider overrun and delivered overrun remain immediate faults.

- [ ] **Step 5: Run focused regression and boundary-fault tests GREEN**

Run the new regression together with:

```text
SapiEngineTests.MisalignedSynthesisCompleteTotalFaultsTheWorker
SapiEngineTests.MissingSynthesisCompleteTotalFaultsTheWorker
SapiEngineTests.DuplicateSynthesisCompleteTotalFaultsTheWorker
SapiEngineTests.TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames
SapiEngineTests.SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks
SapiEngineTests.SynthesisCompleteWhileCancellingCompletesPromptly
```

Expected: all pass; the new regression returns `S_OK`, while stable malformed boundaries still fault.

- [ ] **Step 6: Verify the complete x64 build and test surface**

Run the x64 Debug solution build, all `CoreEngine.Tests`, and a direct x64 Release rebuild of `CoreEngine/CoreEngine.vcxproj`. Expected: every command exits zero with zero compiler warnings/errors and no lingering `CoreEngine.Tests.exe` process.

- [ ] **Step 7: Commit the task**

Stage only the four task files and commit with:

```text
fix: wait for in-flight SAPI writes at completion
```

Record the RED and GREEN commands and results in the SDD task report.
