# Phase 1: Test Suite Hardening & Partitioning Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Refactor and harden the `CoreEngine.Tests` suite by migrating to standardized fresh fixtures, eliminating timing-dependent sleeps with deterministic synchronization, decomposing oversized test files into focused single-responsibility translation units, and verifying dual-target builds.

**Architecture:** Standardized RAII fixtures (`PipeServerWorkerFixture`, `EngineInitializedFixture`) and predicate-based polling (`WaitForCondition`) ensure zero cross-test interference and eliminate flakiness. Translation units are cleanly partitioned by domain responsibility (COM lifetime, provider session, transport faults, protocol faults, worker state, audio streaming, SAPI abort, cancellation).

**Tech Stack:** C++20 (`/std:c++20`), GoogleTest (v140), MSVC MSBuild, Windows Implementation Library (WIL), C++/WinRT.

## Global Constraints

- Windows 11 target exclusively, strict 64-bit compilation (`x64` / `ARM64`); 32-bit (`x86`) is banned.
- All code in C++20 (`/std:c++20`) using existing precompiled header (`pch.h`) and include conventions.
- Zero timing-only sleeps in tests; all asynchronous assertions must use `WaitForCondition` or deterministic event barriers.
- All changes must build with 0 warnings, 0 errors, and pass 100% of tests in both `x64 Debug` and `x64 Release`.
- Multi-line braces `{ ... }` required on all conditionals, loops, and cleanup blocks (`BMVP-03`).
- Work must be performed in an isolated git worktree with directory junctions for `packages/`, `build/`, `bin/`, and `obj/`.

---

### Task 1: Fixtures & Deterministic Synchronization Sweep

**Files:**
- Modify: `CoreEngine.Tests/TestFixtureBase.h`
- Modify: `CoreEngine.Tests/CancellationTests.cpp`
- Modify: `CoreEngine.Tests/FaultRecoveryTests.cpp`
- Modify: `CoreEngine.Tests/AudioStreamingTests.cpp`

**Interfaces:**
- Consumes: `PipeServerWorkerFixture`, `EngineInitializedFixture`, `WaitForCondition` from `TestFixtureBase.h`.
- Produces: Standardized fixture lifecycle across all tests with zero raw `Sleep` calls for synchronization.

- [ ] **Step 1: Inspect and verify `TestFixtureBase.h` helper methods**
Ensure `PipeServerWorkerFixture` has:
```cpp
bool Initialize(WORD blockAlign = 2);
bool Start(uint64_t speakId);
bool WaitForFault(DWORD timeoutMs = 1000);
```
Ensure `WaitForCondition` is used across all test fixtures.

- [ ] **Step 2: Replace manual setup in `CancellationTests.cpp` with `PipeServerWorkerFixture`**
Migrate raw socket/pipe/worker setup blocks to `PipeServerWorkerFixture` and replace any `Sleep(10)` or `Sleep(5)` busy loops with `WaitForCondition`.

- [ ] **Step 3: Replace manual setup and ad-hoc sleeps in `FaultRecoveryTests.cpp`**
Standardize on `PipeServerWorkerFixture` and `WaitForCondition`.

- [ ] **Step 4: Build and run test suite in x64 Debug & Release**
Run: MSBuild `CoreEngine.Tests.vcxproj` and execute `CoreEngine.Tests.exe`.
Expected: 100% PASS in both Debug and Release.

- [ ] **Step 5: Commit changes**
```bash
git add CoreEngine.Tests/TestFixtureBase.h CoreEngine.Tests/CancellationTests.cpp CoreEngine.Tests/FaultRecoveryTests.cpp CoreEngine.Tests/AudioStreamingTests.cpp
git commit -m "refactor(tests): standardize test fixtures and deterministic synchronization"
```

---

### Task 2: Translation Unit Decomposition (Part 1: Lifecycle & Streaming)

**Files:**
- Create: `CoreEngine.Tests/ComLifetimeTests.cpp`
- Create: `CoreEngine.Tests/ProviderSessionTests.cpp`
- Create: `CoreEngine.Tests/ControlStreamTests.cpp`
- Create: `CoreEngine.Tests/PcmFrameAssemblerTests.cpp`
- Modify: `CoreEngine.Tests/AudioStreamingTests.cpp`
- Delete: `CoreEngine.Tests/SessionLifecycleTests.cpp`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj.filters`

**Interfaces:**
- Consumes: `CoreEngineDllFixture.h`, `MockSpObjectToken.h`, `TestFixtureBase.h`.
- Produces: Cleanly separated translation units for COM lifecycle, provider session management, pipe control stream framing, and PCM frame assembly.

- [ ] **Step 1: Extract `ComLifetimeTests.cpp`**
Extract `DllGetClassObject`, `DllCanUnloadNow`, `LockServer`, `CreateInstance`, and unload-reactivation tests from `SessionLifecycleTests.cpp` into `ComLifetimeTests.cpp`.

- [ ] **Step 2: Extract `ProviderSessionTests.cpp`**
Extract `SetObjectToken`, `GetObjectToken`, `GetOutputFormat`, and session recovery/rebuild tests into `ProviderSessionTests.cpp`.

- [ ] **Step 3: Extract `ControlStreamTests.cpp`**
Extract `PipeClient::ReadControlMessage` chunking, framing, UTF-8 recovery, and compaction tests into `ControlStreamTests.cpp`.

- [ ] **Step 4: Extract `PcmFrameAssemblerTests.cpp` from `AudioStreamingTests.cpp`**
Move `PcmFrameAssemblerTests` fixture and test cases into `PcmFrameAssemblerTests.cpp`. Keep `CSapiEngine::Speak` streaming tests in `AudioStreamingTests.cpp`.

- [ ] **Step 5: Update `CoreEngine.Tests.vcxproj` and `CoreEngine.Tests.vcxproj.filters`**
Add the new `.cpp` files to `<ItemGroup>` and remove `SessionLifecycleTests.cpp`.

- [ ] **Step 6: Build and run test suite in x64 Debug & Release**
Verify compilation and test execution across both configurations.

- [ ] **Step 7: Commit changes**
```bash
git add CoreEngine.Tests/ComLifetimeTests.cpp CoreEngine.Tests/ProviderSessionTests.cpp CoreEngine.Tests/ControlStreamTests.cpp CoreEngine.Tests/PcmFrameAssemblerTests.cpp CoreEngine.Tests/AudioStreamingTests.cpp CoreEngine.Tests/CoreEngine.Tests.vcxproj CoreEngine.Tests/CoreEngine.Tests.vcxproj.filters
git rm CoreEngine.Tests/SessionLifecycleTests.cpp
git commit -m "refactor(tests): partition session lifecycle and audio streaming translation units"
```

---

### Task 3: Translation Unit Decomposition (Part 2: Faults, Cancellation & Protocols)

**Files:**
- Create: `CoreEngine.Tests/TransportFaultTests.cpp`
- Create: `CoreEngine.Tests/ProtocolFaultTests.cpp`
- Create: `CoreEngine.Tests/WorkerFaultTests.cpp`
- Create: `CoreEngine.Tests/SapiAbortTests.cpp`
- Create: `CoreEngine.Tests/WriteRejectionCancellationTests.cpp`
- Create: `CoreEngine.Tests/CancellationTimeoutTests.cpp`
- Create: `CoreEngine.Tests/ProtocolParsingTests.cpp`
- Create: `CoreEngine.Tests/RequestContextTests.cpp`
- Delete: `CoreEngine.Tests/FaultRecoveryTests.cpp`
- Delete: `CoreEngine.Tests/CancellationTests.cpp`
- Delete: `CoreEngine.Tests/SpeechProtocolUtilsTests.cpp`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj.filters`

**Interfaces:**
- Consumes: `TestFixtureBase.h`, `MockSpTTSEngineSite.h`, `SpeechWorkerTypes.h`.
- Produces: Partitioned fault recovery, cancellation, and speech protocol translation units.

- [ ] **Step 1: Partition `FaultRecoveryTests.cpp`**
- `TransportFaultTests.cpp`: Pipe disconnects, permission denied, pipe write timeouts, control thread rollback.
- `ProtocolFaultTests.cpp`: Malformed cancellation bounds, misaligned totals, log severity handling.
- `WorkerFaultTests.cpp`: Worker thread state faults, fault publication, assembly faults.

- [ ] **Step 2: Partition `CancellationTests.cpp`**
- `SapiAbortTests.cpp`: SAPI site aborts, re-entrant GetActions polling.
- `WriteRejectionCancellationTests.cpp`: Audio write rejections triggering cancellation & draining.
- `CancellationTimeoutTests.cpp`: Watchdog timeouts on ignored cancellation.

- [ ] **Step 3: Partition `SpeechProtocolUtilsTests.cpp`**
- `ProtocolParsingTests.cpp`: JSON event parsing.
- `RequestContextTests.cpp`: RequestContext state transitions and predicates.

- [ ] **Step 4: Update `CoreEngine.Tests.vcxproj` and `CoreEngine.Tests.vcxproj.filters`**
Register all new `.cpp` files and remove retired files.

- [ ] **Step 5: Build and run test suite in x64 Debug & Release**
Verify compilation and test execution.

- [ ] **Step 6: Commit changes**
```bash
git add CoreEngine.Tests/TransportFaultTests.cpp CoreEngine.Tests/ProtocolFaultTests.cpp CoreEngine.Tests/WorkerFaultTests.cpp CoreEngine.Tests/SapiAbortTests.cpp CoreEngine.Tests/WriteRejectionCancellationTests.cpp CoreEngine.Tests/CancellationTimeoutTests.cpp CoreEngine.Tests/ProtocolParsingTests.cpp CoreEngine.Tests/RequestContextTests.cpp CoreEngine.Tests/CoreEngine.Tests.vcxproj CoreEngine.Tests/CoreEngine.Tests.vcxproj.filters
git rm CoreEngine.Tests/FaultRecoveryTests.cpp CoreEngine.Tests/CancellationTests.cpp CoreEngine.Tests/SpeechProtocolUtilsTests.cpp
git commit -m "refactor(tests): partition fault recovery, cancellation, and protocol translation units"
```

---

### Task 4: Formatting, Bracing & Final Dual-Target Validation

**Files:**
- Modify: All `.cpp` and `.h` files in `CoreEngine.Tests/`

**Interfaces:**
- Consumes: All partitioned test files.
- Produces: 100% compliant multi-line bracing (`BMVP-03`) and green dual-target test validation.

- [ ] **Step 1: Bracing & formatting sweep (`BMVP-03`)**
Add explicit `{ ... }` multi-line braces to any remaining single-line conditionals, loops, or early returns across all test files.

- [ ] **Step 2: Dual-target build & test execution**
Build and run:
- `x64 Debug` build: 100% pass (128+ tests).
- `x64 Release` build: 100% pass (97+ tests).

- [ ] **Step 3: Commit and run adversarial review**
```bash
git commit -am "style(tests): complete BMVP-03 bracing and formatting sweep across all test units"
```
Execute adversarial code review with strict checklist.

