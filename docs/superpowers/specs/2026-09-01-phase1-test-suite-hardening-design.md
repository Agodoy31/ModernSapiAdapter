# Phase 1 Design Specification: Test Suite Hardening, Reusable Fixtures & Deterministic Synchronization

**Date:** 2026-09-01  
**Status:** Proposed / Review  
**Target:** `CoreEngine.Tests` (C++20 / GoogleTest / x64 Windows 11)

---

## 1. Objectives & Architectural Context

Following the completion and verification of Phase 0 (COM Lifecycle and Correctness Hardening), Phase 1 addresses structural debt in the test harness `CoreEngine.Tests`.

### Goals:
1. **Zero Flakiness:** Eliminate all timing-dependent `Sleep()` polling loops and replace them with predicate-based synchronization (`WaitForCondition`).
2. **Fresh Fixtures (`TMOD-01`, `TMOD-04`):** Migrate manual test double instantiation to standardized, fresh fixtures (`PipeServerWorkerFixture`, `EngineInitializedFixture`) to guarantee zero state leakage across test boundaries.
3. **Focused Translation Units (`TMOD-01`, `TMOD-02`):** Decompose monolithic test files (>350 lines) into granular translation units adhering to Single Responsibility Principle.
4. **Formatting & Bracing Consistency (`BMVP-03`):** Enforce strict multi-line `{ ... }` bracing across all test loops, branches, and teardown blocks.
5. **Continuous Dual-Target Validation:** Maintain 100% passing test suites across both `x64 Debug` and `x64 Release` configurations throughout each transformation.

---

## 2. Detailed Component Breakdown

### 2.1 Standardized Fixtures (`TestFixtureBase.h`)
- **`PipeServerWorkerFixture`**: Provides an isolated IPC environment containing:
  - `ControlPipeTestServer server;`
  - `PipeClient client;`
  - `winrt::com_ptr<CSapiEngine> engine;`
  - `winrt::com_ptr<MockSpTTSEngineSite> mockSite;`
  - `std::unique_ptr<SpeechWorker> worker;`
  - Helper methods: `Initialize(WORD blockAlign = 2)`, `Start(uint64_t speakId)`, `WaitForFault(DWORD timeoutMs)`.
- **`EngineInitializedFixture`**: Provides a fully initialized SAPI engine configured with a mock voice token (`MockSpObjectToken`) and pre-queried wave format.

### 2.2 Deterministic Synchronization
- Use `WaitForCondition(Predicate&& pred, DWORD timeoutMs = 1000, DWORD pollIntervalMs = 5)` exclusively for asynchronous event inspection.
- Ban bare `Sleep(N)` calls used for synchronization.

### 2.3 Translation Unit Decomposition Plan

| Source Monolith | New Translation Units | Responsibility / Scope |
| :--- | :--- | :--- |
| `SessionLifecycleTests.cpp` (728 lines) | `ComLifetimeTests.cpp` | `DllGetClassObject`, `DllCanUnloadNow`, `LockServer`, `CreateInstance`, reactivation. |
| | `ProviderSessionTests.cpp` | `SetObjectToken`, `GetObjectToken`, `GetOutputFormat`, session auto-rebuild on timeout. |
| | `ControlStreamTests.cpp` | `PipeClient::ReadControlMessage` chunking, framing, UTF-8 recovery, compaction. |
| `FaultRecoveryTests.cpp` (539 lines) | `TransportFaultTests.cpp` | Control/Audio pipe disconnects, access denied, write timeouts, rollback on thread fail. |
| | `ProtocolFaultTests.cpp` | Invalid cancellation boundaries, misaligned cancellation totals, fatal provider logs. |
| | `WorkerFaultTests.cpp` | Worker thread state machine faults, fault visibility ordering, frame assembly errors. |
| `CancellationTests.cpp` (359 lines) | `SapiAbortTests.cpp` | SAPI `GetActions` abort polling, prompt cancellation without waiting for audio thread. |
| | `WriteRejectionCancellationTests.cpp` | SAPI output site write rejection triggering request cancellation & draining. |
| | `CancellationTimeoutTests.cpp` | Provider ignoring cancellation triggering watchdog session fault & quarantine. |
| `AudioStreamingTests.cpp` (350 lines) | `PcmFrameAssemblerTests.cpp` | Low-level byte framing, fractional carry, awkward 24-bit stereo boundary tests. |
| | `AudioStreamingTests.cpp` | `CSapiEngine::Speak` synthesis complete boundaries, non-contiguous source offsets. |
| `SpeechProtocolUtilsTests.cpp` (365 lines) | `ProtocolParsingTests.cpp` | JSON message parsing (`synthesis_complete`, `word_boundary`, `bookmark`, `log`). |
| | `RequestContextTests.cpp` | `RequestContext` state machine, transitions, semantic predicates. |

---

## 3. Project Configuration Updates (`CoreEngine.Tests.vcxproj`)

Update `CoreEngine.Tests.vcxproj` and `CoreEngine.Tests.vcxproj.filters` to include all newly partitioned `.cpp` files while removing the retired monolithic files, ensuring clean `/MP` parallel compilation and proper PCH inclusion (`pch.h`).

---

## 4. Subagent-Driven Execution Plan

Phase 1 will be executed in 4 sequential tasks with implementer and adversarial reviewer subagents:
- **Task 1: Fixtures & Deterministic Synchronization Sweep**
  - Migrate all remaining ad-hoc setups in `CancellationTests.cpp`, `AudioStreamingTests.cpp`, `FaultRecoveryTests.cpp`.
  - Replace any remaining ad-hoc sleeps with `WaitForCondition`.
- **Task 2: Translation Unit Decomposition (Part 1: Lifecycle & Streaming)**
  - Partition `SessionLifecycleTests.cpp` and `AudioStreamingTests.cpp`.
  - Update `.vcxproj` / `.vcxproj.filters`.
- **Task 3: Translation Unit Decomposition (Part 2: Faults, Cancellation & Protocols)**
  - Partition `FaultRecoveryTests.cpp`, `CancellationTests.cpp`, `SpeechProtocolUtilsTests.cpp`.
  - Update `.vcxproj` / `.vcxproj.filters`.
- **Task 4: Formatting, Bracing & Final Dual-Target Validation**
  - Complete `BMVP-03` bracing sweep across all test files.
  - Run full test suite in `x64 Debug` and `x64 Release`.
  - Final adversarial code review signoff.

