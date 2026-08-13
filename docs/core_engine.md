# CoreEngine C++ Technical Reference

`CoreEngine.dll` is the unmanaged, in-process SAPI 5 COM proxy for Modern SAPI Adapter. It implements `ISpTTSEngine` and `ISpObjectWithToken`, translates SAPI text fragments into UTF-8 JSON, exchanges control records with an external provider over a named pipe, and streams provider-native PCM from a separate audio pipe into `ISpTTSEngineSite`.

CoreEngine does not contain a synthesizer and does not resample audio. The provider declares its native PCM format in the `info` response; CoreEngine advertises that format to SAPI through `GetOutputFormat`. SAPI and its audio stack own any downstream format conversion.

The wire contract is defined in [ipc_protocol.md](ipc_protocol.md). This document describes the current CoreEngine implementation.

## Build and COM model

| Property | Current value |
| :--- | :--- |
| Platform | Windows 11, x64 or ARM64; x86 is unsupported |
| Language | C++20 |
| COM implementation | C++/WinRT `winrt::implements` |
| IPC JSON | `nlohmann::json`, encoded directly as UTF-8 |
| Pipe transport | Two byte-mode Windows named pipes using overlapped I/O |
| CLSID | `{91CD243C-63F7-441F-AE2F-45057005CB6D}` |
| Registered threading model | `Both` |

`dllmain.cpp` exports the standard in-process COM entry points:

- `DllGetClassObject` validates and clears its output pointer, returns a class factory only for the CoreEngine CLSID, and rejects aggregation where COM requires it.
- `DllCanUnloadNow` uses the C++/WinRT module lock, so live factories, engine objects, and balanced `LockServer` calls keep the DLL resident.
- `DllRegisterServer` and `DllUnregisterServer` create and remove the CLSID registration.

The class factory and engine participate in the module lock. `LockServer(TRUE)` adds a server lock and `LockServer(FALSE)` releases it. This prevents the host from unloading CoreEngine while COM-visible state remains active.

## Provider session lifecycle

`CSapiEngine` owns one provider session as an ordered pair:

1. `std::unique_ptr<PipeClient> m_pClient`
2. `std::unique_ptr<SpeechWorker> m_pWorker`

The worker holds a non-owning pointer to the client. Teardown therefore destroys the worker first, allowing its threads to stop and join before the pipe handles disappear.

`SetObjectToken` reads the provider executable path, protocol pipe name, and voice identifier from the SAPI token. Session creation then:

1. Connects both provider pipes, launching the provider immediately when its control pipe is absent.
2. Sends `{"command":"info"}`.
3. requires a matching `{"response":"info"}` object within the control-operation deadline.
4. Validates `sample_rate`, `bits_per_sample`, and `channels`, including range, overflow, byte divisibility, and block alignment.
5. Publishes the client, worker, and negotiated `WAVEFORMATEX` only after the complete candidate session is valid.

A failed candidate is never partially published. An access-denied pipe error fails immediately. Provider launch readiness is bounded to 1,000 ms and is probed every 10 ms; ordinary control writes and the initialization response are bounded to 1,500 ms.

The provider process is not killed when a CoreEngine object is destroyed. CoreEngine closes its session handles, while providers are expected to enforce their own no-client idle shutdown policy. This allows a multi-client provider to outlive an individual COM probe or engine instance.

### Session quarantine and recovery

Transport failures, expired deadlines, malformed active-request records, inconsistent terminal byte totals, stray audio after a completed request, and SAPI write rejection can make byte attribution unsafe. `SpeechWorker::EnterFaultedState` quarantines that session by:

- publishing a fault-pending barrier under the request mutex so no new request can start;
- blocking new event admission;
- moving the request state to `Faulted`;
- waking synchronous callers; and
- calling `PipeClient::Cancel()` to cancel outstanding overlapped pipe I/O.

Faulted sessions are discard-only. `CSapiEngine::Speak` retires the old worker and client and constructs a fresh session on the next request. Recovery is deliberately deferred until a later `Speak`; the failing request returns an HRESULT instead of hiding an unbounded reconnect attempt.

## `CSapiEngine`

### `SetObjectToken` and `GetObjectToken`

`SetObjectToken` serializes token/session replacement against `Speak`, stores the COM token in a `winrt::com_ptr`, loads provider settings, and establishes the initial provider session. `GetObjectToken` follows COM output-pointer rules and returns the retained token reference.

### `GetOutputFormat`

`GetOutputFormat` returns `SPDFID_WaveFormatEx` and a `CoTaskMemAlloc`-allocated copy of the provider-negotiated `WAVEFORMATEX`. The values are not hard-coded to 24 kHz mono; they come from the active provider's `info.audio_format` response.

### `Speak`

`Speak` is serialized by `m_speakMutex` because SAPI hosts may invoke engine methods from multiple threads while one provider session can carry only one active CoreEngine request.

For each call, CoreEngine:

1. Retains the current `ISpTTSEngineSite` under `m_siteMutex`.
2. Rebuilds the provider session if it is absent or quarantined.
3. Allocates a monotonically increasing 64-bit `speak_id`.
4. Converts SAPI UTF-16 fragments to UTF-8 only at the IPC boundary.
5. Builds a `sapi_speak` object with text, source offsets, silence, bookmarks, volume, pitch, and rate.
6. Calls `SpeechWorker::Start` before dispatch so early provider output belongs to a known request.
7. Sends the JSON record with the 1,500 ms control-operation deadline.
8. Waits for normal completion, SAPI abort, inactivity, or a fault.

If dispatch itself fails, the worker quarantines the session. CoreEngine does not reuse a control pipe whose request write failed or timed out.

### Audio and event callbacks

`OnAudioData` calls `ISpTTSEngineSite::Write` under `m_siteMutex`. A write is accepted only when the HRESULT succeeds and SAPI reports that every requested byte was written. `S_OK` with zero or a short byte count is a rejected write and initiates cancellation/fault handling.

`OnSpeechEvent` consumes `nlohmann::json` and maps validated records to `SPEVENT`:

| Provider event | SAPI event | Mapping |
| :--- | :--- | :--- |
| `word_boundary` | `SPEI_WORD_BOUNDARY` | UTF-16 source offset/length and frame-aligned audio offset |
| `sentence_boundary` | `SPEI_SENTENCE_BOUNDARY` | UTF-16 source offset/length and frame-aligned audio offset |
| `bookmark_reached` | `SPEI_TTS_BOOKMARK` | `CoTaskMemAlloc` bookmark string and frame-aligned audio offset |
| `synthesis_complete` | Internal terminal | Declares exact normal PCM byte total |
| `synthesis_cancelled` | Internal terminal | Declares exact committed PCM byte total after cancellation |
| `log` | Internal diagnostic | Logged; active error/fatal records terminate or fault as appropriate |

Audio offsets arrive in milliseconds. CoreEngine converts them using the negotiated sample rate, channel count, and bits per sample, performs the arithmetic in 64 bits, and aligns the resulting byte offset down to a complete PCM frame.

Required numeric fields are accepted only when they are non-negative integral JSON values within the destination range. Compatible unsigned, signed, and exactly integral floating representations are supported; negative, fractional, non-finite, and out-of-range values are rejected before conversion.

## `SpeechWorker`

`SpeechWorker` owns an audio thread and a control thread. Both are long-lived for the provider session and use a request state machine rather than a single speaking flag:

```text
Idle -> Speaking -> Idle
          |          ^
          v          |
      Cancelling ----+

Any usable state -> Faulted
```

`Start` accepts a request only from a clean `Idle` state. `Faulted` is terminal for that worker instance.

### Normal completion

The provider may send `synthesis_complete` before its audio writer has drained. CoreEngine records `total_audio_bytes` and continues forwarding PCM immediately. The request reaches `Idle` only when all of the following are true:

- raw bytes read equal the declared total;
- bytes accepted by SAPI equal the declared total; and
- the frame assembler has no partial-frame carry.

Missing, duplicate, misaligned, non-integral, or contradictory terminal totals quarantine the session. If a pipe read straddles the declared terminal boundary, CoreEngine forwards only the declared complete frames and faults/discards the overrun.

### Cancellation

`WaitUntilFinished` polls `ISpTTSEngineSite::GetActions` every 10 ms. When it observes `SPVES_ABORT`, it changes `Speaking` to `Cancelling` while it still owns `m_requestMutex`. This is the cancellation linearization point: after publication, the audio thread cannot admit additional PCM for SAPI delivery.

Only after that atomic state transition does CoreEngine release the request lock, send `cancel`, and wait for `synthesis_cancelled`. One absolute 500 ms deadline covers:

- acquisition of the serialized control-write mutex;
- JSON serialization and the overlapped cancel write;
- receipt of `synthesis_cancelled`; and
- draining exactly `audio_bytes_written` raw bytes already committed by the provider.

PCM read while `Cancelling` is counted and discarded, never forwarded to SAPI. Completion requires exact equality between bytes consumed and the provider's declared committed-byte boundary. A timeout or mismatch quarantines the session.

Event forwarding has a final admission check immediately before the SAPI callback. It requires `Speaking`, no pending/visible fault, and a matching `speak_id`. An event that already entered SAPI before cancellation may finish, but queued or late events are not newly admitted after the cancellation boundary.

### Inactivity watchdog

An active `Speaking` request faults after 1,500 ms without matching provider progress. Valid PCM and valid active-request control events refresh progress; stale or malformed events do not. There is no total utterance-duration limit while legitimate progress continues. Idle reads may remain blocked indefinitely and are released by `CancelIoEx` during teardown or quarantine.

## PCM frame assembly

Named pipes operate in byte mode, so one `ReadFile` result is not a PCM packet and may end in the middle of a sample frame. `PcmFrameAssembler` uses the negotiated `nBlockAlign` to:

- pass aligned input through without copying when no carry exists;
- retain at most one incomplete frame across reads;
- emit only complete frame spans to SAPI; and
- reset carry at every request, cancellation, fault, and terminal boundary.

CoreEngine neither pads nor resamples PCM. A terminal byte total that is not divisible by `nBlockAlign` is a provider protocol error.

## `PipeClient`

### Pipe paths and launch behavior

Pipe paths follow the protocol-defined schema:

```text
\\.\pipe\<ProviderPipeName>\<UserSID>\control
\\.\pipe\<ProviderPipeName>\<UserSID>\audio
```

`Connect` first attempts both pipes. If the control pipe is absent, it launches the configured provider immediately with `CREATE_NO_WINDOW`. If the control pipe is already open but the audio pipe is not yet ready, it waits within the same bounded readiness window instead of launching a duplicate provider. Provider exit and access denial fail immediately.

### Control records

Control messages are newline-delimited UTF-8 JSON. The pipe remains in byte read mode; record boundaries are reconstructed in user space.

`ReadControlMessageUtf8` retains bytes following the first newline for the next call, supports fragmented records and CRLF, compacts consumed storage after 4 KiB, and caps one control record at 16 MiB. Search offsets prevent repeatedly rescanning the entire retained buffer. The extracted `std::string_view` is parsed into `nlohmann::json` before the retained buffer may change.

`SendControlMessage` serializes a `nlohmann::json` object with `.dump()`, appends `\n`, and performs one complete overlapped write. A `std::timed_mutex` serializes writers within each caller's absolute deadline. Short writes are errors.

### Overlapped operation safety

Timed operations use manual-reset events and `OVERLAPPED`. On timeout, CoreEngine calls `CancelIoEx` for that operation and reaps it before stack storage is destroyed. `PipeClient::Cancel` cancels all outstanding operations on both handles so worker teardown cannot leave a kernel operation referencing dead memory.

## Threading and lock responsibilities

| Lock | Responsibility |
| :--- | :--- |
| `m_speakMutex` | Serializes complete `Speak` calls and token/session replacement |
| `m_sessionMutex` | Protects the client/worker pair and negotiated format |
| `m_siteMutex` | Protects the retained SAPI site and serializes `Write`/`AddEvents` access |
| `m_requestMutex` | Protects request state, byte counters, terminals, and frame assembly |
| `m_eventForwardMutex` | Serializes final event admission against fault publication while allowing COM re-entrancy |
| `m_controlWriteMutex` | Serializes newline JSON writes under the caller's deadline |

The request mutex is never intentionally held while cancellation transport or its terminal wait runs. Cancellation state is published under the lock first, then blocking IPC happens after unlocking.

## Exception and host-safety boundaries

Public COM methods are `noexcept` and translate C++ failures to HRESULTs. Worker thread entries catch unexpected exceptions so malformed provider data or allocation failures quarantine the provider session instead of escaping a `std::thread` entry and terminating the SAPI host. COM apartment initialization on worker threads is paired with guaranteed uninitialization during both normal and exceptional exit.

JSON parsing catches `nlohmann::json::exception`. Protocol field validation does not depend on throwing casts. Invalid UTF-8, malformed JSON, pipe failures, and impossible active-request records fail closed.

## Debug logging

Debug builds use `AsyncLogger` and timestamp each record at enqueue time. Logs are written asynchronously to:

```text
%APPDATA%\ModernSapiAdapter\CoreEngine.log
```

Release builds compile the debug logger and detailed cancellation tracing out. The cancellation trace records request dispatch, abort observation, atomic cancellation publication, control-write duration, provider terminal receipt, byte-boundary completion, session recovery, and selected slow SAPI event callbacks.

## Provider-process responsibility

CoreEngine owns a COM object and one IPC session; it does not own the global lifetime of a multi-client provider. Providers must:

- support the fixed SID-scoped pipe naming scheme;
- enforce their own idle/no-client shutdown policy;
- produce PCM in the format returned by `info`;
- obey the exact `synthesis_complete` and `synthesis_cancelled` byte-boundary contracts; and
- stop emitting PCM and normal events for a request after its cancellation acknowledgement.

See [ipc_protocol.md](ipc_protocol.md) for the authoritative provider-facing requirements.
