# SpeechWorker Phase A (Request-State Policy) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extract deterministic request lifecycle decisions, boundary evaluations, cancellation state transitions, timeout conditions, and error-handling policies from `SpeechWorker` into a pure, stateless Level-0 leaf unit (`SpeechStatePolicy`).

**Architecture:** `SpeechWorker` retains sole ownership of threads, mutexes, condition variables, mutable request context, named pipe I/O, COM callbacks, and session quarantine. `SpeechStatePolicy` is introduced as a `noexcept`, allocation-free, bounded, stateless leaf module. `SpeechWorker` holds `m_requestMutex` continuously across input capture, policy evaluation, token validation, and decision application.

**Tech Stack:** C++20, Windows 11 SDK (10.0.26100.0), MSBuild, GoogleTest, WIL (Windows Implementation Libraries), Win32 Named Pipes, SAPI 5.

---

## Global Constraints

- **Platform Target:** Windows 11 exclusively. Strict 64-bit compilation (`x64` locally; `ARM64` exemption documented below). 32-bit (`x86`) is strictly prohibited.
- **ARM64 Dependency Exception:** The build environment lacks cross-compilation toolchains for ARM64 dependencies; targeted validation executes on x64 (Debug and Release).
- **Zero Production Concurrency Changes:** No new threads, queues, locks, sleeps, condition variables, polling intervals, or timeouts.
- **Lock Discipline:** `SpeechWorker` holds `m_requestMutex` continuously across input capture, policy evaluation, token validation, and result application.
- **Permitted Production Lock Order:** Only `m_eventForwardMutex` followed by `m_requestMutex` is permitted; reverse acquisition is strictly forbidden.
- **Zero Locked I/O or Callbacks:** No named pipe I/O, COM calls, or external callbacks may occur while holding `m_requestMutex` or `m_eventForwardMutex`.
- **Pure Policy Contract:** `SpeechStatePolicy` functions accept immutable inputs, use no out-parameters, return owned typed decision structs, are `noexcept`, allocation-free, stateless, and bounded.
- **Tool Discipline:** File inspection and modifications must use native MCP tools (`view_file`, `write_to_file`, `replace_file_content`). Shell commands (`cat`, `Set-Content`) are prohibited.
- **Formatting:** Strict Allman bracing on all blocks across modified and new code. Zero trailing whitespace.

---

## Baseline Evidence & Test Inventory

### Pre-Change Baseline Metrics
- **Configuration:** Debug|x64
- **Test Executable:** `bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe`
- **Total Test Count:** 193 tests across 12 test cases.
- **Pre-Change Suite Status:** 193 / 193 PASSED (Elapsed: ~15.1 seconds under 30-second watchdog).

### Focused Test Filter
```text
--gtest_filter=SpeechStatePolicyTests.*:SpeechProtocolUtilsTests.RequestContext*:AudioTerminalBoundaryTests.*:CancellationTimeoutTests.*:SapiAbortTests.*:WriteRejectionCancellationTests.*:WorkerFaultTests.*:ProtocolFaultTests.*:SpeechEventTests.* --gtest_color=no
```

### Required Real-Pipe and Callback-Boundary Tests
1. `SapiEngineTests.SpeakWaitsForSynthesisCompleteByteBoundary` (exercises real pipe, audio streaming, `OnAudioData`, `Write`).
2. `SapiEngineTests.RealControlPipeBoundaryAndBookmarkEndToEnd` (exercises real control pipe, JSON parsing, `OnSpeechEvent`, `AddEvents`).
3. `SapiEngineTests.OutputSiteAbortCancelsTheActiveRequest` (exercises real SAPI abort polling and prompt cancellation).
4. `SapiEngineTests.AddEventsBlockingDoesNotDelayWorkerFaultPublication` (exercises lock hierarchy `m_eventForwardMutex` then `m_requestMutex` and fault isolation).

### 30-Second Watchdog Command
```powershell
$sw = [System.Diagnostics.Stopwatch]::StartNew()
$p = Start-Process -FilePath "D:\Projects\ModernSapiAdapter\.worktrees\phase7a-speech-state-policy-design\bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe" -ArgumentList "<FILTER>" -PassThru -NoNewWindow
$exited = $p.WaitForExit(30000)
$sw.Stop()
if (-not $exited) { $p.Kill(); Write-Host "TIMEOUT" } else { Write-Host "ExitCode: $($p.ExitCode), ElapsedMs: $($sw.ElapsedMilliseconds)" }
```

### MockProvider Quiescence Verification
Between test runs, any orphaned or lingering provider processes must be verified quiesced:
```powershell
Get-Process -Name "MockProvider" -ErrorAction SilentlyContinue | Wait-Process -Timeout 5 -ErrorAction SilentlyContinue
```

### Immutable Evidence Directory
All characterization, red/green transcripts, watchdog runs, and final reports must be stored under:
`.agents/sdd/phase7a/`

---

## Task Breakdown

### Task 1: Create `SpeechStatePolicy` Leaf Unit and Direct Test Suite

**Files:**
- Create: `CoreEngine/SpeechStatePolicy.h`
- Create: `CoreEngine/SpeechStatePolicy.cpp`
- Create: `CoreEngine.Tests/SpeechStatePolicyTests.cpp`
- Modify: `CoreEngine/CoreEngine.vcxproj`
- Modify: `CoreEngine/CoreEngine.vcxproj.filters`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj`

**Interfaces:**
- Consumes: `SpeechWorkerTypes.h` (`RequestContext`, `RequestToken`, `UpstreamState`, `DownstreamState`), `SpeechProtocolUtils.h` (predicates).
- Produces: Namespace `SpeechStatePolicy` with typed decisions:
  - `StartDecision EvaluateStart(...)`
  - `TerminalBoundaryDecision EvaluateTerminalBoundary(...)`
  - `BeginCancellationDecision EvaluateBeginCancellation(...)`
  - `StopDecision EvaluateStop(...)`
  - `TerminalEventDecision EvaluateTerminalEvent(...)`
  - `LogEventDecision EvaluateLogEvent(...)`
  - `AudioIngestHeaderDecision EvaluateAudioIngestHeader(...)`
  - `AudioDeliveryResultDecision EvaluateAudioDeliveryResult(...)`
  - `TimeoutDecision EvaluateTimeouts(...)`
  - `bool ShouldForwardEvent(...)`
  - `bool IsAudioDeliveryEligible(...)`
  - `bool IsWaitTerminal(...)`

- [ ] **Step 1: Write `SpeechStatePolicy.h`**
  Declare pure types and functions under `namespace SpeechStatePolicy` as specified in `.agents/specs/2026-09-16-speechworker-phase-a-request-policy-design.md`.

- [ ] **Step 2: Write failing tests in `CoreEngine.Tests/SpeechStatePolicyTests.cpp`**
  Cover every policy function:
  - `EvaluateStart`: Idle vs Active vs Faulted vs FaultPending.
  - `EvaluateTerminalBoundary`: Normal completion, audio overrun, carry buffer check, cancelling drain, utterance error reset.
  - `EvaluateBeginCancellation`: Speaking to Cancelling, Already Idle, Already Cancelling, Faulted.
  - `EvaluateStop`: Speaking/Cancelling to ResetAndCancel, Idle to NoAction.
  - `EvaluateTerminalEvent`: Duplicate terminal fault, invalid bytes fault, misaligned bytes fault, valid completion, valid cancelled.
  - `EvaluateLogEvent`: Fatal vs Error vs Info/Warn.
  - `EvaluateAudioIngestHeader`: Speaking with/without terminal declared remaining, Cancelling, Idle unexpected audio, Faulted.
  - `EvaluateAudioDeliveryResult`: Token match/mismatch, write accepted vs rejected, cancelling drain.
  - `EvaluateTimeouts`: Inactivity timeout, cancellation timeout, none.
  - Query predicates: `ShouldForwardEvent`, `IsAudioDeliveryEligible`, `IsWaitTerminal`.

- [ ] **Step 3: Register files in project files**
  Add `SpeechStatePolicy.h` and `SpeechStatePolicy.cpp` to `CoreEngine.vcxproj` and `CoreEngine.vcxproj.filters`. Add `SpeechStatePolicyTests.cpp` to `CoreEngine.Tests.vcxproj`.

- [ ] **Step 4: Build and capture red transcript**
  Target MSBuild on `CoreEngine.Tests.vcxproj` (unresolved external symbols expected before implementation). Record in `.agents/sdd/phase7a/red-transcript.txt`.

- [ ] **Step 5: Implement `SpeechStatePolicy.cpp`**
  Implement all functions with strict Allman formatting, `noexcept`, zero heap allocations, zero locks.

- [ ] **Step 6: Build and run `SpeechStatePolicyTests.*`**
  Verify all new unit tests pass under the 30-second watchdog. Record green transcript in `.agents/sdd/phase7a/task1-green-transcript.txt`.

---

### Task 2: Rewire `SpeechWorker` to Delegate State Decisions to `SpeechStatePolicy`

**Files:**
- Modify: `CoreEngine/SpeechWorker.h`
- Modify: `CoreEngine/SpeechWorker.cpp`

**Interfaces:**
- Consumes: `SpeechStatePolicy` functions and decision types.
- Produces: Behaviorally identical `SpeechWorker` with reduced internal decision branching and formal locked policy boundaries.

- [ ] **Step 1: Rewire `SpeechWorker::Start`**
  Capture `m_context` under `m_requestMutex`. Call `SpeechStatePolicy::EvaluateStart(m_context, speakId, m_generationCounter + 1)`. If accepted, apply mutations, increment `m_generationCounter`, reset assembler, and update progress tick.

- [ ] **Step 2: Rewire `SpeechWorker::Stop`**
  Call `SpeechStatePolicy::EvaluateStop(m_context)`. If `ResetAndCancel`, apply reset to idle, reset assembler, notify `m_requestChanged`, unlock, and call `SendCancellation`.

- [ ] **Step 3: Rewire `SpeechWorker::BeginCancellationLocked`**
  Call `SpeechStatePolicy::EvaluateBeginCancellation(m_context, cancellationDeadline)`. Apply state transition and assembler reset based on decision struct.

- [ ] **Step 4: Rewire `SpeechWorker::CheckTerminalBoundaryLocked`**
  Call `SpeechStatePolicy::EvaluateTerminalBoundary(m_context, m_frameAssembler.HasCarry())`. Apply `ResetToIdleLocked()` or return overrun protocol fault.

- [ ] **Step 5: Rewire `SpeechWorker::HandleTerminalEventLocked`**
  Call `SpeechStatePolicy::EvaluateTerminalEvent(m_context, eventType, terminalAudioBytes, hasValidTerminalBytes, m_frameAssembler.BlockAlign())`. On fault, call `TransitionRequestToFaultedLocked()`. On success, apply terminal fields and evaluate terminal boundary.

- [ ] **Step 6: Rewire `SpeechWorker::HandleLogEventLocked`**
  Call `SpeechStatePolicy::EvaluateLogEvent(logSeverity)`. On fatal, return fault. On error, apply failure fields and evaluate terminal boundary.

- [ ] **Step 7: Rewire `SpeechWorker::IngestAudioChunkLocked`**
  Call `SpeechStatePolicy::EvaluateAudioIngestHeader(m_context, bytesRead)`. Apply progress tick, framing, assembler call, and empty span boundary checks.

- [ ] **Step 8: Rewire `SpeechWorker::UpdateAfterAudioDeliveryLocked`**
  Call `SpeechStatePolicy::EvaluateAudioDeliveryResult(m_context, batchToken, writeAccepted, cancellationDeadline)`. Apply delivered bytes, write rejection cancellation transition, or boundary checks.

- [ ] **Step 9: Rewire query helpers**
  Update `ShouldForwardEventLocked`, `IsAudioDeliveryEligibleLocked`, and `IsWaitTerminalLocked` to delegate to `SpeechStatePolicy`.

- [ ] **Step 10: Build and run focused test filter**
  Build `CoreEngine.Tests.vcxproj` in Debug|x64 and run the focused test filter under the 30-second watchdog. Record green transcript in `.agents/sdd/phase7a/task2-green-transcript.txt`.

---

### Task 3: Whole-Branch Hardening, Quiescence, and Evidence Packaging

**Files:**
- Output: `.agents/sdd/phase7a/characterization.txt`
- Output: `.agents/sdd/phase7a/full-debug-suite-watchdog.txt`
- Output: `.agents/sdd/phase7a/full-release-suite-watchdog.txt`
- Output: `.agents/sdd/phase7a/git-diff-check.txt`
- Output: `.agents/sdd/phase7a/report.md`

- [ ] **Step 1: Execute 10 consecutive runs of the focused test filter**
  Run all focused tests 10 consecutive times under 30-second watchdogs in Debug|x64. Verify 100% pass rate with zero flakes or hangs.

- [ ] **Step 2: Full Debug|x64 Test Suite Execution**
  Run complete suite under 30-second watchdog. Verify total test count equals baseline plus new tests (193 baseline + new unit tests). Ensure clean exit code 0.

- [ ] **Step 3: Full Release|x64 Build and Test Suite Execution**
  Build `CoreEngine.Tests.vcxproj` in Release|x64. Run complete suite under 30-second watchdog. Ensure clean exit code 0.

- [ ] **Step 4: Verify MockProvider Quiescence**
  Confirm zero orphaned `MockProvider.exe` processes exist between and after runs.

- [ ] **Step 5: Verify formatting and git diff hygiene**
  Run `git diff --check` across the branch against `main`. Ensure zero whitespace errors or formatting issues.

- [ ] **Step 6: Author SDD completion report**
  Compile `.agents/sdd/phase7a/report.md` summarizing changes, test metrics, timings, and evidence files.

---

## Review and Gate Requirements

Before implementation can begin:
1. Two independent Gemini Pro subagents must review the Phase A package:
   - **Reviewer 1:** Concurrency, state transitions, and lock-ordering invariants.
   - **Reviewer 2:** Contract completeness and test coverage.
2. Raw review verdicts must be captured and any identified defects resolved.
3. The audit, specification, plan, and raw reviews must be submitted to Codex for the mandatory pre-code contract gate.
