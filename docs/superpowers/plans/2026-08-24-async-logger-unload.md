# Unload-Safe Asynchronous Debug Logger Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Preserve CoreEngine's asynchronous Debug file logging while guaranteeing that its worker thread is drained and joined before the COM DLL becomes unloadable.

**Architecture:** Convert `AsyncLogger` from a destructed function-local static into a lazily allocated, intentionally non-destructed control object with an explicit `Stopped`, `Running`, `Stopping`, and stop-failure lifecycle. `DllCanUnloadNow` performs the safe shutdown outside the loader lock and returns `S_OK` only after shutdown succeeds and the C++/WinRT module lock remains zero.

**Tech Stack:** C++20, Win32, C++/WinRT COM module locking, STL thread/condition-variable primitives, GoogleTest, MSBuild x64.

## Global Constraints

- Windows 11 only and strict x64 compilation for this task; do not build x86 or ARM64.
- Preserve asynchronous Debug logging and the existing `%APPDATA%\ModernSapiAdapter\CoreEngine.log` format and location.
- Do not add a third-party logger or runtime dependency.
- Release logging must remain compiled out.
- No thread synchronization may run from a static destructor or `DllMain` teardown.
- All logger entry points reached from exported COM functions must contain exceptions.
- Shared logger state is protected by one mutex; the worker performs file I/O without holding that mutex.

---

### Task 1: Prove explicit drain, repeated shutdown, and restart behavior

**Files:**
- Create: `CoreEngine.Tests/AsyncLoggerTests.cpp`
- Modify: `CoreEngine.Tests/CoreEngine.Tests.vcxproj`

**Interfaces:**
- Consumes: existing `AsyncLogger::GetInstance()` and `AsyncLogger::Log(const std::wstring&)`.
- Produces: required public `bool AsyncLogger::Shutdown() noexcept` behavior for Task 2.

- [ ] **Step 1: Add a Debug-only test fixture that resolves the real log file**

```cpp
#include "pch.h"
#include "../CoreEngine/AsyncLogger.h"

#if defined(_DEBUG)
namespace
{
std::filesystem::path CoreEngineLogPath()
{
    PWSTR roamingPath = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &roamingPath)))
    {
        return {};
    }

    const std::filesystem::path path = std::filesystem::path(roamingPath) /
        L"ModernSapiAdapter" / L"CoreEngine.log";
    CoTaskMemFree(roamingPath);
    return path;
}

std::string UniqueMarker(const char* suffix)
{
    return "AsyncLoggerTests-" + std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + "-" + suffix;
}

bool FileContains(const std::filesystem::path& path, const std::string& marker)
{
    std::ifstream stream(path);
    const std::string contents(
        std::istreambuf_iterator<char>(stream),
        std::istreambuf_iterator<char>());
    return stream && contents.find(marker) != std::string::npos;
}
}
#endif
```

- [ ] **Step 2: Write failing tests for drain, idempotence, and restart**

```cpp
#if defined(_DEBUG)
TEST(AsyncLoggerTests, ShutdownDrainsEveryAcceptedMessage)
{
    auto* logger = AsyncLogger::GetInstance();
    ASSERT_NE(logger, nullptr);
    const auto first = UniqueMarker("drain-first");
    const auto second = UniqueMarker("drain-second");

    logger->Log(std::wstring(first.begin(), first.end()));
    logger->Log(std::wstring(second.begin(), second.end()));

    ASSERT_TRUE(logger->Shutdown());
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), first));
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), second));
}

TEST(AsyncLoggerTests, ConcurrentShutdownCallsAreIdempotent)
{
    auto* logger = AsyncLogger::GetInstance();
    ASSERT_NE(logger, nullptr);
    logger->Log(L"AsyncLoggerTests concurrent shutdown");
    std::atomic_bool first{false};
    std::atomic_bool second{false};

    std::thread firstCaller([&] { first = logger->Shutdown(); });
    std::thread secondCaller([&] { second = logger->Shutdown(); });
    firstCaller.join();
    secondCaller.join();

    EXPECT_TRUE(first.load());
    EXPECT_TRUE(second.load());
}

TEST(AsyncLoggerTests, LoggingAfterShutdownStartsANewSession)
{
    auto* logger = AsyncLogger::GetInstance();
    ASSERT_NE(logger, nullptr);
    ASSERT_TRUE(logger->Shutdown());
    const auto marker = UniqueMarker("restart");

    logger->Log(std::wstring(marker.begin(), marker.end()));

    ASSERT_TRUE(logger->Shutdown());
    EXPECT_TRUE(FileContains(CoreEngineLogPath(), marker));
}
#endif
```

The production mutation these tests catch is removing the queue drain, permitting two callers to join the same worker, or leaving the logger permanently stopped after an unloadability query.

- [ ] **Step 3: Include the test file in the x64 test project**

Add this entry beside `SapiEngineTests.cpp`:

```xml
<ClCompile Include="AsyncLoggerTests.cpp" />
```

- [ ] **Step 4: Build the x64 Debug tests and verify RED**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' ModernSapiAdapter.slnx /m /t:Build /p:Configuration=Debug /p:Platform=x64
```

Expected: compilation fails because `AsyncLogger` has no `Shutdown()` member. A linker, include, or package failure is not the expected RED and must be corrected before continuing.

- [ ] **Step 5: Commit the failing lifecycle contract tests**

```powershell
git add CoreEngine.Tests/AsyncLoggerTests.cpp CoreEngine.Tests/CoreEngine.Tests.vcxproj
git commit -m "test: define async logger shutdown contract"
```

---

### Task 2: Implement the unload-safe asynchronous lifecycle

**Files:**
- Modify: `CoreEngine/AsyncLogger.h`
- Modify: `CoreEngine/AsyncLogger.cpp`
- Modify: `CoreEngine/pch.h`
- Modify: `CoreEngine/pch.cpp`
- Modify: `CoreEngine.Tests/pch.h`
- Modify: `CoreEngine.Tests/pch.cpp`

**Interfaces:**
- Consumes: the tests and `bool AsyncLogger::Shutdown() noexcept` contract from Task 1.
- Produces: lazy `GetInstance()`, non-throwing `Log()`, explicit drain-and-join shutdown, and safe restart.

- [ ] **Step 1: Replace destructor-driven lifetime with explicit state**

Use this private model in `AsyncLogger.h`; keep all STL includes in `pch.h` per project policy:

```cpp
class AsyncLogger
{
public:
    static AsyncLogger* GetInstance() noexcept;
    void Log(const std::wstring& message) noexcept;
    [[nodiscard]] bool Shutdown() noexcept;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

private:
    enum class State
    {
        Stopped,
        Running,
        Stopping,
        StopFailed
    };

    AsyncLogger() noexcept = default;
    ~AsyncLogger() = default;

    bool StartLocked() noexcept;
    void WorkerThread() noexcept;

    std::queue<std::wstring> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    State m_state = State::Stopped;
    bool m_stopRequested = false;
    std::wofstream m_file;
};
```

The `StopFailed` state preserves the worker object if an unexpected `join()` failure occurs; it prevents logging from restarting over a worker whose termination was not proven.

- [ ] **Step 2: Make singleton destruction impossible**

Implement `GetInstance()` with one intentionally retained, non-throwing allocation:

```cpp
AsyncLogger* AsyncLogger::GetInstance() noexcept
{
    static AsyncLogger* instance = new (std::nothrow) AsyncLogger();
    return instance;
}
```

No logger destructor is registered with the CRT, so DLL detach cannot call `join()`. The allocation contains only process-lifetime control state and is reclaimed by Windows at process exit. Add `<new>` to `CoreEngine/pch.h`; a null result disables Debug logging safely.

- [ ] **Step 3: Implement lazy startup and exception containment**

`StartLocked()` runs with `m_mutex` held. It resolves the existing roaming log path, opens the stream, clears `m_stopRequested`, and creates `m_worker`. It catches path, stream, and thread failures, closes any partially opened stream, and returns `false` with the state still `Stopped`. `Log()` catches all allocation/formatting failures, waits while the state is `Stopping`, drops messages in `StopFailed`, starts from `Stopped`, enqueues only in `Running`, unlocks, and notifies the worker.

The startup sequence must follow this order:

```cpp
m_file.open(logPath, std::ios::out | std::ios::app);
m_stopRequested = false;
try
{
    m_worker = std::thread(&AsyncLogger::WorkerThread, this);
    m_state = State::Running;
    return true;
}
catch (...)
{
    if (m_file.is_open()) m_file.close();
    return false;
}
```

- [ ] **Step 4: Implement a drain-before-exit worker**

The worker waits for either data or `m_stopRequested`, removes exactly one message while holding the mutex, and writes it after releasing the mutex. It exits only when stop is requested and the queue is empty:

```cpp
for (;;)
{
    std::wstring message;
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] { return m_stopRequested || !m_queue.empty(); });
        if (m_queue.empty())
        {
            if (m_stopRequested) break;
            continue;
        }
        message = std::move(m_queue.front());
        m_queue.pop();
    }

    if (m_file.is_open())
    {
        m_file << message << L'\n';
        m_file.flush();
    }
}

if (m_file.is_open())
{
    m_file.flush();
    m_file.close();
}
```

- [ ] **Step 5: Implement idempotent shutdown without holding the mutex across join**

`Shutdown()` waits out another owner's `Stopping` transition. For `Running` or `StopFailed`, it sets `Stopping` and `m_stopRequested`, notifies the worker, and calls `m_worker.join()` without holding `m_mutex`. On success it returns to `Stopped`; on exception it publishes `StopFailed`. Both outcomes notify lifecycle waiters. If already `Stopped`, return `true`.

- [ ] **Step 6: Make the Debug logging boundary non-throwing**

Declare and define `CoreLog(const wchar_t* fmt, ...) noexcept` in the CoreEngine and test precompiled-header pairs. Add `<iterator>` to `CoreEngine.Tests/pch.h`. In `CoreEngine/pch.cpp`, preserve `va_end` and wrap formatting plus singleton access in `try`/`catch (...)`; call `Log()` only when `GetInstance()` returns non-null. The test stub remains behaviorally unchanged apart from swallowing allocation failures rather than throwing through production callers.

- [ ] **Step 7: Build and run the x64 Debug tests to verify GREEN**

Run the Debug build command from Task 1, followed by the generated GoogleTest executable under `bin/CoreEngine.Tests/x64/Debug`. Expected: all tests pass, including the three new `AsyncLoggerTests` cases.

- [ ] **Step 8: Commit the lifecycle implementation**

```powershell
git add CoreEngine/AsyncLogger.h CoreEngine/AsyncLogger.cpp CoreEngine/pch.h CoreEngine/pch.cpp CoreEngine.Tests/pch.h CoreEngine.Tests/pch.cpp
git commit -m "fix: make async logger shutdown explicit"
```

---

### Task 3: Gate COM unload on successful logger shutdown

**Files:**
- Modify: `CoreEngine/dllmain.cpp`
- Modify: `CoreEngine.Tests/SapiEngineTests.cpp`

**Interfaces:**
- Consumes: `AsyncLogger::Shutdown() noexcept` from Task 2 and `winrt::get_module_lock()`.
- Produces: `DllCanUnloadNow()` returns `S_OK` only when the COM count is zero and the Debug worker is stopped.

- [ ] **Step 1: Extend the existing DLL lifetime test with a post-log unload cycle**

Update `DllCanUnloadNowTracksFactoryLifetime` so it executes two complete factory/unloadability cycles. The second cycle proves that the logger can restart after the first `S_OK` query and be shut down again:

```cpp
EXPECT_EQ(module.CanUnloadNow(), S_OK);

winrt::com_ptr<IClassFactory> restartedFactory;
ASSERT_EQ(module.GetClassFactory(restartedFactory.put()), S_OK);
EXPECT_EQ(module.CanUnloadNow(), S_FALSE);
restartedFactory = nullptr;
EXPECT_EQ(module.CanUnloadNow(), S_OK);
```

The production mutation this catches is stopping the logger permanently or returning `S_OK` while a class factory remains live.

- [ ] **Step 2: Build and run the x64 Debug tests and verify RED**

The test may expose the old destructor teardown or restart behavior only during DLL unload. If it passes against the old implementation, retain it as integration coverage but rely on Task 1's compile-time RED for the new lifecycle API; do not manufacture an implementation-coupled assertion.

- [ ] **Step 3: Integrate shutdown into `DllCanUnloadNow`**

Include `AsyncLogger.h` and use this ordering:

```cpp
STDAPI DllCanUnloadNow(void)
{
    if (winrt::get_module_lock()) return S_FALSE;

#ifdef _DEBUG
    if (auto* logger = AsyncLogger::GetInstance(); logger && !logger->Shutdown()) return S_FALSE;
#endif

    return winrt::get_module_lock() ? S_FALSE : S_OK;
}
```

Do not add a `CoreLog` call after shutdown. The second module-lock read is the overlap guard required by the design.

- [ ] **Step 4: Run the complete x64 Debug test suite**

Build the Debug solution and execute `CoreEngine.Tests.exe`. Expected: all tests pass with no hang, crash, or warning introduced by logger shutdown.

- [ ] **Step 5: Build x64 Release and verify zero Debug dependency**

Run:

```powershell
& 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe' ModernSapiAdapter.slnx /m /t:Build /p:Configuration=Release /p:Platform=x64
```

Expected: build succeeds with zero errors; `DllCanUnloadNow` compiles without referencing `AsyncLogger` in Release.

- [ ] **Step 6: Commit COM unload integration**

```powershell
git add CoreEngine/dllmain.cpp CoreEngine.Tests/SapiEngineTests.cpp
git commit -m "fix: stop debug logger before COM unload"
```

---

### Task 4: Final verification and review

**Files:**
- Review only: all files changed by Tasks 1-3.

**Interfaces:**
- Consumes: complete implementation.
- Produces: reviewed release candidate ready for integration.

- [ ] **Step 1: Run formatting and repository checks**

```powershell
git diff --check main...HEAD
git status --short
```

Expected: no whitespace errors and no unrelated changes.

- [ ] **Step 2: Run final x64 verification from a clean build state**

Build Debug and Release x64, then run the Debug GoogleTest executable. Record test totals, failures, and the exact output DLL paths.

- [ ] **Step 3: Perform the concurrency and resource-lifecycle audit**

Confirm all of the following from the final diff:

- `m_state`, `m_stopRequested`, `m_worker`, `m_queue`, and `m_file` have one documented owner and all cross-thread access is synchronized.
- File writes occur without `m_mutex` held.
- `Shutdown()` never holds `m_mutex` while joining.
- No destructed static object owns the worker.
- `DllCanUnloadNow` returns `S_FALSE` after any unproven shutdown.
- Release code neither starts nor stops a logger.

- [ ] **Step 4: Request code review and address only verified findings**

Use the requesting-code-review skill, rerun affected tests after any correction, and retain the TDD evidence in the handoff.
