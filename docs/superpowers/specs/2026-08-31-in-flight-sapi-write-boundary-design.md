# In-Flight SAPI Write Boundary Design

## Problem

CoreEngine currently records provider PCM as raw audio before calling `ISpTTSEngineSite::Write`, then records it as delivered only after that call returns. The provider control thread can receive `synthesis_complete` during the call. When the final provider bytes have already been read, `CheckTerminalBoundaryLocked` sees `raw == declared` and `delivered < declared` and incorrectly treats the temporary gap as a protocol violation.

Interactive JAWS traces proved this race for isolated `m`, `q`, `r`, and `t` requests. In every failure, the raw-minus-delivered gap exactly matched the final write still in progress; Core returned `E_FAIL` before that write subsequently returned `S_OK` with the full requested byte count.

## Selected Design

Track whether the audio thread has a SAPI write batch in flight. The audio thread sets this state while holding `m_requestMutex` before it releases the mutex to call the engine site. After all writes in the batch return, it reacquires the mutex, clears the state, updates delivered-byte accounting when the request is still speaking, and rechecks the terminal boundary.

`CheckTerminalBoundaryLocked` will treat `raw == declared && delivered < declared` as pending while a SAPI write is in flight. It will retain the existing protocol fault when no write is active and the raw, delivered, declared, or carried-frame state genuinely disagrees.

A boolean is sufficient because `SpeechWorker` owns exactly one audio thread and only that thread calls `OnAudioData`. A counter would imply unsupported parallel writers and add unnecessary state.

## Alternatives Rejected

Moving raw-byte accounting until after `Write` returns would conflate provider-pipe consumption with downstream delivery. Cancellation boundaries and protocol validation require those to remain distinct.

Holding `m_requestMutex` across `ISpTTSEngineSite::Write` would serialize the control thread behind an external COM callback. That would delay cancellation and risk reentrancy deadlocks.

Treating every raw/delivered mismatch as pending would hide genuine provider boundary defects and could leave `Speak` waiting until the inactivity timeout.

## End-to-End Data Flow

1. The provider writes headerless PCM to the audio pipe and sends `synthesis_complete` with the exact total byte count.
2. CoreEngine's audio thread reads PCM, increments `m_rawAudioBytesRead`, frames it, and marks the resulting SAPI write batch active under `m_requestMutex`.
3. The audio thread releases the mutex and calls `ISpTTSEngineSite::Write` for the batch.
4. The control thread may receive and record `synthesis_complete` concurrently. If the final write is active, terminal evaluation remains pending.
5. The audio thread reacquires the mutex, clears the active-write state, updates `m_deliveredAudioBytes` for accepted writes, and reevaluates completion.
6. `Speak` returns `S_OK` only when declared, raw, and delivered bytes agree with no carried PCM. It faults only for a stable mismatch with no write capable of closing the gap.

## Shared State and Synchronization

The new active-write state is read and written only while `m_requestMutex` is held. The audio thread is its sole writer. The control thread and synchronous `Speak` path observe it only through existing request-state operations protected by the same mutex.

The state describes the complete `PcmFrameBatch`, not an individual span, so a terminal event cannot observe a false idle interval between two spans produced from one pipe read.

## Cancellation and Failure Behavior

If SAPI requests `SPVES_ABORT` during an active write, cancellation may transition the request to `Cancelling` while the COM call remains outside the mutex. When the write returns, the audio thread clears the active-write state but does not add bytes to speaking-delivery totals after cancellation, preserving existing purge semantics.

If a write fails or writes only part of its requested data, the audio thread clears the active-write state before entering the existing cancellation path. The new state must never remain set after a normal return or handled exception.

Provider overrun, underrun, misalignment, and carried-frame mismatches remain faults once no write is active. Existing cancellation and inactivity timeouts are unchanged.

## Ownership and Teardown

`SpeechWorker` owns the active-write state together with the audio thread and all request counters. No new handle, allocation, thread, or external resource is introduced. Worker teardown continues to cancel pipe I/O and join the audio thread; the state cannot outlive its owner.

## Verification Strategy

Add a deterministic regression test that blocks the final SAPI write after Core has read it, delivers `synthesis_complete`, and verifies that `Speak` has not returned or faulted. Releasing the write must produce `S_OK`, exact delivered-byte accounting, and a reusable session.

Retain existing tests that prove stable terminal mismatches fault. Run the focused regression, the complete x64 Debug CoreEngine test suite, an x64 Debug solution build, and a direct x64 Release CoreEngine rebuild. Finally, install the Debug candidate and repeat the interactive trimmed-letter sequence through JAWS.

## Scope

This change is entirely within CoreEngine's downstream synchronization. It does not change the provider protocol, provider implementation, audio format, resampling behavior, event mapping, cancellation timeout, or SAPI output buffering.
