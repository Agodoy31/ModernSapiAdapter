# Unload-Safe Asynchronous Debug Logger Design

Date: 2026-08-24

## Problem

CoreEngine's Debug-only `AsyncLogger` is a function-local static object. Its destructor signals its worker thread and calls `join()`. The C runtime may invoke that destructor while processing DLL detach under the Windows loader lock. Waiting for a worker thread there can deadlock because thread termination and DLL thread-detach notifications also require the loader lock.

Detaching the worker is not safe: it could continue executing CoreEngine code after the DLL has been unmapped. Replacing the implementation with another asynchronous logger such as spdlog would not remove the lifecycle requirement; asynchronous logging libraries also require an explicit shutdown before a Windows DLL unloads.

## Goals

- Preserve asynchronous Debug logging so file I/O does not distort speech and cancellation timing.
- Drain and flush accepted messages before a normal COM unload.
- Ensure no logger worker is running when `DllCanUnloadNow` returns `S_OK`.
- Ensure static destruction never waits for another thread.
- Make shutdown safe when called repeatedly or concurrently.
- Allow logging to restart if COM queries unloadability but keeps the DLL loaded and later activates CoreEngine again.
- Leave Release behavior and its zero-cost logging macros unchanged.

## Non-goals

- Replacing the logging format or file location.
- Adding log rotation or a general-purpose logging dependency.
- Guaranteeing graceful cleanup when a host forcibly unloads a DLL contrary to the COM unload contract.
- Changing provider IPC or synthesis behavior.

## Design

### Lifetime

`AsyncLogger::GetInstance()` will return a process-allocated singleton whose C++ destructor is never registered for static teardown. This intentionally retains only the small logger control object. Normal logger resources remain explicitly managed: the worker is stopped, the queue is drained, and the file is flushed and closed before unload.

Avoiding the static destructor is necessary even after adding explicit shutdown because process termination does not necessarily call `DllCanUnloadNow` first. The operating system reclaims the small control allocation during process exit.

### State machine

The logger will have three lifecycle states protected by its lifecycle mutex:

- `Stopped`: no worker and no open log stream.
- `Running`: worker and stream are active; messages may be queued.
- `Stopping`: shutdown owns the transition; new messages are not admitted to the old queue.

The initial state is `Stopped`. The first `Log()` call starts the stream and worker lazily, outside DLL initialization. A later call after a completed shutdown may start a fresh session.

Only one transition may create or remove a worker. `Shutdown()` is idempotent: callers observing `Stopped` return immediately, while callers observing `Stopping` wait for that transition to complete rather than attempting a second join.

### Logging path

`Log()` formats the existing timestamped message and takes the lifecycle/queue synchronization needed to verify `Running` and enqueue it. The caller never performs file I/O. If the logger is `Stopped`, `Log()` initializes a new session before enqueueing. If shutdown is already in progress, the call waits for the state transition and then restarts or enqueues into the new session; it never writes into a queue whose worker is exiting.

The existing worker continues to serialize file writes in FIFO order. Shutdown signals the worker only after closing admission to the current queue. The worker drains every accepted message, flushes and closes the stream, and exits.

### COM unload integration

`DllCanUnloadNow` will follow this order in Debug builds:

1. If the C++/WinRT module lock is nonzero, return `S_FALSE` without stopping the logger.
2. If it is zero, call `AsyncLogger::Shutdown()` outside `DllMain` and wait for the drain to finish.
3. Recheck the module lock after shutdown.
4. Return `S_OK` only when the second check is still zero; otherwise return `S_FALSE` and permit the next log call to restart the logger.

The second module-lock check prevents an activation that overlaps shutdown from being ignored. `DllCanUnloadNow` must not enqueue another log message after stopping the logger.

Release builds retain their current behavior because `AsyncLogger` and this shutdown hook are Debug-only.

### Error handling

- Failure to open the log file leaves the logger usable as a non-throwing no-op for that session; it must not break COM activation.
- Thread creation failure is contained and leaves the state `Stopped`; logging never propagates an exception through a COM boundary.
- Shutdown remains safe if initialization only partially completed.
- Logger methods used by exported COM entry points remain non-throwing.

## Verification

- Unit/lifecycle test: concurrent producers preserve complete lines while shutdown drains them.
- Unit/lifecycle test: repeated and concurrent `Shutdown()` calls complete without a duplicate join or deadlock.
- Unit/lifecycle test: a log after shutdown restarts the worker and reaches the file.
- COM unload smoke test: activate and release CoreEngine, request unused-library cleanup, and verify the Debug DLL can unload without hanging.
- Build the x64 Debug and Release configurations; ARM64 is intentionally excluded.
- Confirm Release logging remains compiled out and no new runtime dependency is introduced.

## Rejected alternatives

- Synchronous file logging: safe but would contaminate the latency measurements for which Debug logging exists.
- Detached worker: risks executing code or accessing state after module unload.
- spdlog or another async package: still needs explicit pre-unload shutdown and adds dependency/build surface without addressing the core lifecycle problem.
- ETW-only logging: robust and fast, but would remove the accessible plain-text diagnostic workflow.
