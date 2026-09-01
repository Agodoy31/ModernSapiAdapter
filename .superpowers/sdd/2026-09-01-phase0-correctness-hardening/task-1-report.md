# Task 1 Report: Harden COM unload decision

## Changed files

- `CoreEngine/DllUnloadPolicy.h`: added allocation-free, callable-driven three-gate unload policy.
- `CoreEngine/dllmain.cpp`: delegates `DllCanUnloadNow` to the policy; Debug safely shuts down the logger and Release returns success without referencing or constructing it.
- `CoreEngine/CoreEngine.vcxproj` and `CoreEngine/CoreEngine.vcxproj.filters`: registered the new header.
- `CoreEngine.Tests/DllUnloadPolicyTests.cpp`: added deterministic tests for initial lock refusal, shutdown failure, activation during shutdown, and normal unload.
- `CoreEngine.Tests/CoreEngine.Tests.vcxproj`: registered the new test source.

`CoreEngine.Tests/CoreEngine.Tests.vcxproj.filters` does not exist in this worktree, so no test-project filter mapping was created.

## TDD evidence

### RED

After adding the tests, the focused command was:

```text
bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe --gtest_filter=DllUnloadPolicyTests.*
```

The legacy one-gate policy produced the expected failures:

- `RefusesUnloadWhenLoggerShutdownFails`: expected `S_FALSE`, got `S_OK`.
- `RefusesUnloadWhenActivationOccursDuringLoggerShutdown`: expected `S_FALSE` and two lock reads, got `S_OK` and one read.
- `AllowsUnloadWhenLoggerShutdownCompletesWithNoModuleLocks`: expected two reads and one shutdown call, but observed one read and no shutdown call.

The initial-lock-refusal test passed, confirming that the existing first gate remained in place.

### GREEN

Targeted x64 Debug build, launched through `ProcessStartInfo` with a case-insensitively deduplicated environment:

```text
MSBuild.exe CoreEngine.Tests\CoreEngine.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

Result: build succeeded with 0 warnings and 0 errors.

Focused verification:

```text
bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe --gtest_filter=DllUnloadPolicyTests.*:SapiEngineTests.DllCanUnloadNow*:*LockServer*
```

Result: 8 tests from 2 suites passed. This includes all four policy tests and the four affected `DllCanUnloadNow`/`LockServer` lifetime tests.

## Self-review

- The policy orders its guard clauses exactly as specified: initial lock count, logger shutdown result, post-shutdown lock count.
- The helper takes callables by reference and allocates no memory.
- The DLL snapshots the non-copyable C++/WinRT module lock as `std::uint32_t` before passing it to the pure zero-lock predicate.
- Debug preserves asynchronous logger shutdown behavior; Release selects a constexpr success callback with no `AsyncLogger` reference.
- No DLL exports or production-only test hooks were added.
- `git diff --cached --check` completed without whitespace errors before commit.

## BMVP review gate

Vector C confirmed the new policy’s three gates are serially readable, explicitly braced, and limited to shallow control flow. It also identified pre-existing unbraced guard clauses elsewhere in `dllmain.cpp`; these were not introduced by this task and were left unchanged to preserve scope.

Vectors A and B identified broader existing/lifecycle-level concerns that exceed Task 1's specified two-reader/one-shutdown-callback interface:

- A concurrent factory creation or `LockServer(TRUE)` can occur after the final zero-lock snapshot and before `S_OK` is returned; eliminating that TOCTOU window requires an admission/serialization mechanism not exposed by the required helper contract.
- In Debug, an in-flight logger call can restart the logger after shutdown; a fully closed logger lifecycle requires coordinated logger admission control.
- Logger shutdown remains synchronous and has no bounded-wait/cancellation mechanism.

Per controller direction, these findings are documented for the required review gate and were not expanded into out-of-scope synchronization or logger lifecycle changes.

## Commit

`c0370498d5045fe163a06fc906ce43b55c64edc6` — `fix(CoreEngine): harden COM unload decision`

## Fix round 1: modular COM admission and logger quiescence

### Modified files

- Replaced `CoreEngine/DllUnloadPolicy.h` and its tests with `DllEntryAdmission.h/.cpp` and deterministic lease-admission tests.
- Updated `CoreEngine/dllmain.cpp` to acquire entry leases before logging or class-factory construction, coordinate close/drain/quiescence, and retain closed admission only on `S_OK`.
- Updated `CoreEngine/AsyncLogger.h/.cpp` with bounded unload quiescence, rejection-resume behavior, and a test-only terminal-state probe compiled only into `CoreEngine.Tests`.
- Updated the CoreEngine and test project registrations, `AsyncLoggerTests.cpp`, the COM DLL fixture, and `SessionLifecycleTests.cpp` for the intentional closed-on-`S_OK` lifetime behavior and post-approval factory rejection.

### RED evidence

The first focused x64 Debug build after adding the admission/logger tests failed as intended because `DllEntryAdmission`, `BeginUnloadQuiescence`, and `ResumeAfterUnloadRejected` did not yet exist.

During self-review, the bounded-timeout test was strengthened to require logging to recover only after its blocked writer reached the terminal state. Before the recovery implementation, the targeted test produced:

```text
AsyncLoggerTests.QuiescenceTimeoutReturnsFalseWithoutAbandoningTheWorker
ContainsMarker(writtenMessages, restartedMessage)
  Actual: false
Expected: true
```

This demonstrated that a completed timed-out drain remained permanently quiescent instead of permitting a later log call to join the completed worker and restart diagnostics.

### GREEN evidence

Targeted build, launched through `ProcessStartInfo` with a case-insensitively deduplicated environment:

```text
MSBuild.exe CoreEngine.Tests\CoreEngine.Tests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m /clp:ErrorsOnly
```

Result: succeeded (x64 Debug, exit code 0).

Focused verification:

```text
bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe --gtest_filter=DllEntryAdmissionTests.*:AsyncLoggerTests.*:SapiEngineTests.DllCanUnloadNow*:*LockServer*:SapiEngineTests.DllGetClassObjectRejectsNewAdmissionAfterUnloadApproval
```

Result: 13 tests from 3 suites passed in 857 ms.

### Self-review

- Entry admission owns only its mutex, state, condition variable, and lease count; no coordinator lock is held across logging, allocation, QI, or COM callbacks.
- `DllCanUnloadNow` orders the required pre-lock check, admission close/drain, Debug logger quiescence, final lock check, and deliberate closed state on `S_OK`.
- Release contains no `AsyncLogger` reference. Test-only logger access is conditionally compiled only for the test project and adds no DLL export.
- A timed-out logger drain does not detach its worker; it remains an unload blocker until it exits, then later logging joins that worker before restarting.
- `git diff --check` completed without whitespace errors. Touched control-flow blocks use explicit braces.

### Concerns

- The repair intentionally leaves entry admission closed after `S_OK`; the COM DLL fixture now records that approval and calls `FreeLibrary` rather than issuing a second `DllCanUnloadNow` against an admission state that a normal COM caller would already unload.
- A logger timeout returns `S_FALSE` and never abandons/detaches the worker. Diagnostic restart is deferred until that worker reaches its terminal state.
- No broader changes were made to provider IPC, speech-worker shutdown, SAPI ABI, registry behavior, roadmap, merge state, or Tasks 2/3.

### Follow-up commit

`fix(CoreEngine): serialize COM unload admission` (final commit hash is reported to the controller with completion status).

## Fix round 2: admission-timeout recovery and real-DLL race coverage

### RED evidence

The new real-DLL integration test first failed as expected because no pause hook existed after `DllGetClassObject` acquired its entry lease:

```text
DllCanUnloadNowRefusesUnloadAfterAnAdmittedFactoryPublishesAModuleLock
barrier.WaitForEntryPaused(1000)
  Actual: false
Expected: true
```

After adding the deterministic barrier test, the focused admission test exposed the timeout-state defect:

```text
DllEntryAdmissionTests.TimedOutClosingReopensAdmission
admission.TryEnter().has_value()
  Actual: false
Expected: true
```

The failed wait had left admission in `Closing`, rejecting later factory admission.

### GREEN evidence

Targeted x64 Debug build, launched through `ProcessStartInfo` with the case-insensitively deduplicated environment, succeeded:

```text
MSBuild.exe CoreEngine.Tests\CoreEngine.Tests.vcxproj /t:Build /p:Configuration=Debug /p:Platform=x64 /m /clp:ErrorsOnly
```

Final focused verification (corrected count and filter):

```text
bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe --gtest_filter=DllEntryAdmissionTests.*:AsyncLoggerTests.*:SapiEngineTests.DllCanUnloadNow*:*LockServer*:SapiEngineTests.DllGetClassObjectRejectsNewAdmissionAfterUnloadApproval
```

Result: 15 tests from 3 suites passed in 985 ms.

### Modified files and self-review

- `DllEntryAdmission.cpp` now atomically restores `Open` and notifies waiters when its entry-drain wait times out. A concurrent close still returns `false` without reopening another caller's active close.
- `DllEntryAdmissionTestHooks.h` provides Debug-only internal named-event barriers with no DLL export. `dllmain.cpp` pauses only after an entry lease is held, and `DllEntryAdmission.cpp` signals the exact `Closing` transition.
- `SessionLifecycleTests.cpp` releases the entry barrier through `wil::scope_exit`, waits for `Closing`, then proves the resumed factory path publishes a module lock and forces `DllCanUnloadNow` to return `S_FALSE`.
- `DllEntryAdmissionTests.cpp` now verifies that a timed-out close restores normal entry admission.
- `git diff --check` completed without whitespace errors. No Tasks 2/3 files, roadmap, merge, provider IPC, or ABI exports were changed.

### Concerns

- The named synchronization barriers are Debug-only internal test instrumentation; they are inert in Release and not exported. They use the current process ID, so concurrent CoreEngine admission-race tests in one process would need serialization.
- An entry-drain timeout now reopens admission inside its owning component; `DllCanUnloadNow` therefore preserves a concurrent caller's `Closing` state rather than unconditionally reopening it.

### Follow-up commit

`fix(CoreEngine): repair admission timeout recovery` (final commit hash is reported to the controller with completion status).
