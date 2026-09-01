# Async Logger Current-Main Port Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Port the already validated unload-safe Debug logger contract onto the refactored current `main` without restoring obsolete test organization.

**Architecture:** Retain the existing asynchronous FIFO logger, but replace destructor-owned shutdown with an explicitly managed, restartable lifecycle. The logger control object remains process-lifetime allocated, while `DllCanUnloadNow` drains and joins the worker before reporting the COM DLL unloadable.

**Tech Stack:** C++20, Win32, C++/WinRT module locking, STL threading, GoogleTest, MSBuild x64.

## Global Constraints

- Windows 11 and x64 only; do not build x86 or ARM64.
- Preserve asynchronous Debug logging and the existing log path and timestamp format.
- Keep Release logging compiled out and add no runtime dependency.
- Never wait for the logger worker from `DllMain` or a static destructor.
- Preserve the current modular test layout.

---

### Task 1: Port the logger lifecycle contract tests

**Files:**
- Create: `CoreEngine.Tests/AsyncLoggerTests.cpp`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj`

**Interfaces:**
- Consumes: current `AsyncLogger::GetInstance()` and `Log()`.
- Produces: required `AsyncLogger* GetInstance() noexcept` and `bool Shutdown() noexcept` contract.

- [ ] Add the four real-file tests from commit `069256d`/`8c1caa7`: drain accepted messages, concurrent idempotent shutdown, concurrent producer drain, and restart after shutdown.
- [ ] Register `AsyncLoggerTests.cpp` beside the current modular test sources.
- [ ] Build Debug x64 and verify RED because current `AsyncLogger` has no explicit shutdown API.
- [ ] Commit the failing contract as `test: port async logger shutdown contract`.

### Task 2: Port the explicit logger lifecycle

**Files:**
- Modify: `CoreEngine/AsyncLogger.h`
- Modify: `CoreEngine/AsyncLogger.cpp`
- Modify: `CoreEngine/pch.h`
- Modify: `CoreEngine/pch.cpp`
- Modify: `CoreEngine.Tests/pch.h`
- Modify: `CoreEngine.Tests/pch.cpp`

**Interfaces:**
- Produces: nullable process-lifetime singleton, lazy startup, FIFO drain, idempotent concurrent shutdown, restart, and non-throwing logging boundaries.

- [ ] Port the `Stopped`, `Running`, `Stopping`, and `StopFailed` state machine from commit `8c1caa7`.
- [ ] Keep all logger state under one mutex; release it before file I/O and before joining.
- [ ] Make `CoreLog` non-throwing and preserve the current safe `va_copy` formatting behavior.
- [ ] Build and run the dedicated logger tests to verify GREEN.
- [ ] Run the entire native Debug suite to catch lifecycle regressions.
- [ ] Commit as `fix: port explicit async logger lifecycle`.

### Task 3: Integrate with the current COM lifecycle suite

**Files:**
- Modify: `CoreEngine/dllmain.cpp`
- Modify: `CoreEngine.Tests/SessionLifecycleTests.cpp`

**Interfaces:**
- Consumes: `AsyncLogger::Shutdown() noexcept` and `winrt::get_module_lock()`.
- Produces: `DllCanUnloadNow` returns `S_OK` only after the Debug worker is proven stopped and the module lock remains zero.

- [ ] Extend the existing real-DLL lifecycle test in `SessionLifecycleTests.cpp` with a second activation/unload cycle to prove restart.
- [ ] Add the two module-lock checks around `Shutdown()` in `DllCanUnloadNow`; do not log after shutdown.
- [ ] Run the focused lifecycle tests and full native Debug suite.
- [ ] Commit as `fix: stop debug logger before COM unload`.

### Task 4: Final release-candidate verification

**Files:**
- Review only: all files changed above.

- [ ] Run `git diff --check main...HEAD` and confirm no unrelated changes.
- [ ] Build Debug x64 and run all native and managed tests.
- [ ] Build Release x64 and record the CoreEngine DLL path.
- [ ] Audit that no static destructor owns a worker, no join holds the mutex, and Release has no logger shutdown dependency.
- [ ] Request code review before integration.
