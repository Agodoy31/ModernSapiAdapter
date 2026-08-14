# Provider Development Guide

This guide explains how to build an external text-to-speech provider for Modern SAPI Adapter. It is language-neutral at the process and wire boundaries. The final section uses the current .NET 10 Azure provider as a C# reference and identifies both patterns worth copying and choices that should be improved in a new provider.

The wire authority is [ipc_protocol.md](ipc_protocol.md). This guide explains how to implement that contract; it does not replace it. CoreEngine behavior is described in [core_engine.md](core_engine.md), and provider packaging and voice registration are described in [sapi_manager.md](sapi_manager.md) and [sapi_registry.md](sapi_registry.md).

The labels used throughout this guide have precise meanings:

- **Required** means current CoreEngine or SapiManager interoperability depends on it.
- **Recommended** means production hardening that is not represented by a wire field.
- **.NET 10 example** is one efficient C# implementation strategy, not a protocol requirement.
- **Known limitation** identifies current behavior that a provider author must understand but should not copy as an ideal design.

## The provider in one page

A provider is a standalone Windows process. It owns the TTS SDK or backend and exposes two named-pipe servers for each connected client:

- a duplex control pipe carrying newline-delimited UTF-8 JSON;
- an outbound audio pipe carrying headerless raw PCM.

CoreEngine is the client of both pipes. It translates SAPI fragments into `sapi_speak`, forwards the provider's PCM to SAPI, converts provider events into SAPI events, and sends `cancel` when SAPI aborts an utterance. SapiManager is a second kind of client: it uses a control-only connection to query `info` and `voices`, then sends `shutdown` after discovery.

A minimum usable provider must:

1. Start without command-line arguments and create the SID-scoped pipe names within 1,000 ms.
2. Accept more than one client session without mixing their audio or events.
3. Answer `info` without initializing an expensive speech backend.
4. Answer `voices` on a control-only connection.
5. Accept one active `sapi_speak` per session and stream the declared PCM format.
6. Preserve every `speak_id` and normalize boundary coordinates to the original SAPI UTF-16 source.
7. End every successful request with exactly one aligned `synthesis_complete` byte boundary.
8. Handle `cancel` by discarding uncommitted audio, waiting for any in-flight audio write, and returning exactly one `synthesis_cancelled` committed-byte boundary within 500 ms.
9. Write no PCM or normal events for a request after its terminal cancellation acknowledgement.
10. Shut itself down after a configurable no-client idle period; CoreEngine intentionally does not terminate it when one COM object disappears.
11. Protect both pipes with an explicit per-user security descriptor, remote-client rejection, connected-client identity validation, and an intentional mandatory-integrity policy.

The central design rule is simple: treat each client and each synthesis request as an owned state machine, not as a collection of unrelated callbacks.

## Package and process contract

SapiManager installs a provider package containing an executable and `manifest.json`. The manifest ID becomes the stable `ProviderPipeName` stored in each SAPI voice token. CoreEngine launches the configured executable with:

- no provider-specific command-line arguments;
- the provider directory as its working directory;
- a hidden window and no console.

A minimal package manifest is:

```json
{
  "id": "ExampleTtsProvider",
  "name": "Example TTS Provider",
  "executable": "ExampleTtsProvider.exe",
  "version": "1.0.0",
  "publisher": "Example Publisher",
  "description": "Provides Example voices to Modern SAPI Adapter."
}
```

Keep `id` stable across upgrades and make `executable` match the packaged file exactly.

SapiManager also launches the executable during discovery, but currently does not set the provider working directory. Resolve packaged resources relative to the executable/application base directory and configuration from explicit data directories; never rely on the inherited current directory.

**Required:** the executable must know its stable pipe prefix without receiving it from CoreEngine. Do not generate a new pipe name per process, request, or test.

**Recommended:** use one provider host per Windows user and allow that host to serve multiple pipe instances concurrently. A per-user instance gate can prevent duplicate listeners, but a duplicate launch must exit harmlessly without disrupting the existing process.

**Known SapiManager limitation:** current discovery expects the particular process it launched to remain alive, so it reports failure when a singleton's duplicate process exits even if an existing host is healthy. Discovery also sends a process-wide `shutdown` afterward. Until SapiManager is corrected to attach safely to an existing host, test installation and discovery with no provider host already running. Do not weaken runtime multi-client isolation or terminate the existing host merely to hide this management bug.

**Recommended:** load configuration and initialize logging at process startup, but create expensive SDK engines, models, or network connections lazily. `info` should be a pure capability response. Voice discovery may require the backend, but ordinary pipe availability must not.

**Required:** support x64 and/or ARM64 packages explicitly. x86 is not supported by Modern SAPI Adapter. Declare whether a managed package is framework-dependent or self-contained instead of relying on SDK defaults.

## Pipe names, instances, and security

For the current user SID and provider ID, create:

```text
\\.\pipe\<ProviderPipeName>\<UserSID>\control
\\.\pipe\<ProviderPipeName>\<UserSID>\audio
```

The control pipe is duplex. The audio pipe is outbound from provider to CoreEngine. Both use byte mode. Byte mode means a read may return half a JSON record, several JSON records, half a PCM frame, or several PCM buffers. Neither pipe preserves application message boundaries.

Create another server instance as soon as a pair has been accepted. Do not make the global listener wait for an established client to finish. Each accepted pair becomes an independent `ClientSession` with its own control reader, serialized control writer, audio writer, request state, and cancellation source.

### Control-only discovery

SapiManager connects only the control pipe, sends `info`, sends `voices`, and finally sends `shutdown`. Therefore:

- **Recommended:** begin reading control commands while an audio connection is still pending so discovery does not pay the audio-pairing grace period.
- **Required:** service `info`, `voices`, and `shutdown` without an audio pipe.
- **Required:** never admit synthesis when no audio pipe is available. The current wire contract has no generic command-rejection response, so disconnect an invalid direct client after an optional diagnostic `log`; do not invent an undocumented response shape.
- **Recommended:** accept the audio endpoint concurrently rather than delaying all control handling for an audio grace period.

**Current SapiManager compatibility:** its discovery reader expects the next control record to be the requested response and does not correctly preserve coalesced records. While answering `info` or `voices`, send that response as the next and only record; do not precede it with a `log` event or batch it with another record. CoreEngine's reader does not have this restriction. This is a management-client limitation that should eventually be fixed, not a general byte-pipe framing rule.

### Pairing the two pipes

**Known limitation:** the current protocol has no explicit cross-pipe session nonce. Pairing solely by connection order is ambiguous when one process opens multiple sessions concurrently.

**Recommended for the current protocol:** compare the control and audio client process IDs with `GetNamedPipeClientProcessId`, keep pairing windows short, and reject rather than guess when two sessions from the same process are ambiguous. PID comparison cannot distinguish concurrent sessions owned by one process. A future nonce handshake would be stronger, but it would be a protocol change and is not required by the current specification.

### Access control

The SID in the pipe name partitions names; it is not authorization by itself.

**Required:** apply an explicit pipe security descriptor granting the intended user and necessary system identities only, reject remote clients, validate connected-client identity, and set an intentional mandatory-integrity policy. An elevated provider still needs to accept the same user's medium-integrity CoreEngine without admitting unintended low-integrity writes. Treat failure to resolve the current SID or establish this policy as a startup failure rather than falling back to a shared name.

**Recommended:** use first-instance protection where the provider process model permits it, and fail safely if another server already owns the pipe name.

## Control framing and validation

Each control record is one UTF-8 JSON object terminated by LF:

```text
{"command":"info"}\n
```

An implementation must preserve incomplete bytes between reads and continue parsing after one newline. It should tolerate CRLF by removing one trailing carriage return. Never assume one pipe read equals one JSON record.

**Required:** serialize control writes. A response, boundary event, log event, and terminal event must never interleave their UTF-8 bytes.

**Required:** keep each outbound record on one line and append exactly one LF. CoreEngine caps an inbound record at 16 MiB; providers should enforce a smaller documented request limit where practical and must bound fragment counts and text sizes to protect memory.

Validate before converting numeric values to narrower runtime types. Reject missing, negative, fractional, overflowing, or wrongly typed values where the protocol requires a non-negative integer. Store `speak_id` in at least an unsigned 64-bit representation; CoreEngine generates a monotonically increasing 64-bit value. Do not silently replace invalid offsets with zero.

CoreEngine serializes one request per session. If a direct client sends a second `sapi_speak` while the first is active or draining, do not queue it implicitly or replace the active request. Because the current protocol has no standardized busy response, close the offending session after an optional diagnostic `log`. A conforming direct client waits for the current request boundary or cancellation acknowledgement.

**Recommended:** separate framing, JSON decoding, schema validation, and command dispatch. This makes malformed-input tests deterministic and prevents backend code from receiving partially valid commands.

## Capability and voice queries

### `info`

CoreEngine sends `info` for every fresh provider session. Return the complete documented response: `response: "info"`, nonempty `provider_name` and `version`, `supports_ssml`, and a valid PCM `audio_format` containing positive integral `sample_rate`, `bits_per_sample`, and `channels`. Current CoreEngine consumes only the response discriminator and audio format, but management tools and conformance tests may consume the metadata.

The format is provider-selected. It is not implicitly 24 kHz, 16-bit, or mono. CoreEngine advertises the provider's format to SAPI and does not resample it.

Compute:

```text
block_align = channels * bits_per_sample / 8
bytes_per_second = sample_rate * block_align
```

The channel/bit product must be byte-divisible, block alignment must fit a Windows `WORD`, and bytes per second must fit a `DWORD`. All audio and terminal byte totals must use this exact format.

CoreEngine also expects the same format after a faulted session reconnects. Do not change format based on the selected voice or request. If a backend has mixed native formats, normalize inside the provider to one advertised provider format or expose it through a separate provider identity.

### `voices`

Return `response: "voices"` and a catalog containing stable voice IDs, display names, BCP-47 language tags, gender values, and vendor names.

Voice IDs become durable registry data. Do not use array positions or short-lived SDK handles. A later `sapi_speak.voice_id` must resolve the same voice after provider restart and package upgrade, or fail explicitly. Normalize gender values to lowercase `male`, `female`, or `neutral` for portability; current SapiManager stores the string verbatim rather than enforcing the enum.

### `ssml_speak`

The wire specification reserves `ssml_speak` for direct modern clients. Current CoreEngine does not send it. A provider may support it, but SAPI source-coordinate rules for `sapi_speak` do not apply to an arbitrary direct SSML document.

## Recommended architecture

A production provider is easiest to reason about when responsibilities are explicit.

### `ProviderHost`

Owns the process instance gate, immutable configuration snapshot, logger, listener, global cancellation, shared lazy backend, and no-client shutdown policy. It is the only component allowed to dispose the shared backend.

### `DualPipeListener`

Owns server-instance creation, security descriptors, control/audio acceptance, pairing, and continued admission of new clients. It hands each accepted pair to one `ClientSession`.

### `ClientSession`

Owns one control reader, one serialized control writer, one audio egress writer, and at most one active `RequestSession`. It tracks query operations separately from synthesis so teardown can await both without disposing process-wide resources.

### `ProtocolCodec`

Owns newline framing, UTF-8 parsing, wire DTOs, numeric validation, record-size limits, and command dispatch. It has no dependency on a TTS SDK.

### `RequestSession`

Owns `speak_id`, request cancellation, event ordering, audio ownership, accepted and committed counters, and lifecycle transitions. Legal paths are:

```text
Active -> CompletionDeclared -> Drained
Active -> Cancelling -> Cancelled
CompletionDeclared -> Cancelling -> Cancelled
Active -> Faulting -> Faulted
```

Send at most one `synthesis_complete` declaration and at most one `synthesis_cancelled` acknowledgement. Cancellation received while declared audio is still draining may legitimately produce both records in that order; the cancellation acknowledgement supersedes the earlier completion declaration for CoreEngine. Make state decisions and byte-counter snapshots atomic with respect to audio admission. Validation should happen before `Active` whenever possible.

### `AudioEgress`

Owns a bounded FIFO queue and the only writer to the audio pipe. It accepts explicit buffer ownership, applies backpressure when full, releases buffers exactly once, and publishes writer failure to the request state machine.

### `SpeechEngine`

Defines backend-neutral voice and synthesis models. It emits raw PCM and normalized internal events and returns `Completed`, `Cancelled`, or `Failed`. Vendor SDK types should not leak into this interface.

An optional engine-resource pool may reuse expensive SDK objects, but pool capacity, warm-up, health checking, and idle disposal are backend policies rather than IPC behavior.

## Translating `sapi_speak`

CoreEngine sends an ordered list of text, silence, and bookmark fragments. Text fragments include raw SAPI volume, pitch, and rate plus `source_offset`, the fragment's absolute UTF-16 offset in the original SAPI source.

Preserve a source map while converting fragments into backend text or SSML. The map should record, for every emitted text span:

- the backend-visible range;
- the serialized SSML range if the SDK may report XML positions;
- the original absolute UTF-16 source range;
- XML entity expansion or normalization that changes offsets.

Use unique private bookmark tokens internally. This allows two bookmarks with the same public name to remain distinct and makes duplicate SDK callbacks suppressible.

If a backend boundary cannot be mapped unambiguously, suppress that boundary and send a warning `log` containing its native offset. Never guess, and especially never fall back to zero. PCM and later valid events must continue.

Silence fragments contribute actual zero-valued PCM frames and therefore count toward terminal audio bytes. Do not implement silence only as a delay.

## Audio streaming and backpressure

The audio pipe contains headerless PCM only. Never write a WAV or RIFF header.

Providers may write any positive number of bytes, and CoreEngine reconstructs complete PCM frames across arbitrary pipe reads. Nevertheless, a provider should enqueue and write complete frames whenever its backend permits. Every successful terminal total must be divisible by `block_align`.

Do not buffer the whole utterance. Screen-reader latency depends on forwarding PCM as soon as it becomes usable. Conversely, do not use an unbounded queue: a slow or disconnected reader could turn a long utterance into unlimited memory growth.

Use a bounded queue sized in milliseconds of audio rather than a fixed byte count:

```text
queue_capacity_bytes = bytes_per_second * target_buffer_ms / 1000
```

Choose the smallest capacity that absorbs normal SDK callback jitter. Enforce it as a byte budget, not merely an item count: SDK chunks can vary greatly in size. Acquire the byte budget before accepting ownership and release it exactly once after write, discard, or failure. A fixed-size chunker may instead make item capacity equivalent to bytes. When the queue is full, apply asynchronous backpressure to the backend adapter if possible. If the SDK callback cannot block safely, copy into a bounded pool and fail the request when the limit is exhausted; never grow without bound.

Track two counters:

- `accepted_bytes`: bytes whose ownership has transferred into the final audio egress queue;
- `committed_bytes`: bytes whose asynchronous audio-pipe write has reported successful completion. If the runtime adds a user-space buffer above the named pipe, flush that buffer successfully before counting the bytes. Merely enqueueing or beginning a write does not commit them.

These counters serve different terminal events and must not be substituted for one another.

## Normal completion

On successful synthesis:

1. Stop accepting backend PCM for the request.
2. Seal and drain the request's normal event sequence onto the control pipe.
3. Verify `accepted_bytes` is block-aligned.
4. Atomically transition from `Active` to `CompletionDeclared`.
5. Send one `synthesis_complete` containing `total_audio_bytes = accepted_bytes`.
6. Allow the FIFO audio writer to continue draining the already accepted queue.
7. Transition to `Drained` only after all `accepted_bytes` have committed, unless cancellation has already won.

The completion declaration establishes the final byte boundary; it does not mean those bytes have all reached CoreEngine. This early declaration keeps latency low and allows synthesis resources to return to a pool without waiting for SAPI playback.

For example, if the negotiated format is 24,000 Hz, 16-bit mono, `block_align` is 2. If the final queue accepted 62,400 bytes, send:

```json
{"event":"synthesis_complete","speak_id":41,"total_audio_bytes":62400}
```

No later PCM may be accepted for request 41, and no later normal speech event may be sent for it. The audio writer may still commit the already accepted bytes in FIFO order.

Keep the request cancellable until the final declared byte has committed. A cancellation during this drain interval supersedes normal completion from CoreEngine's perspective.

## Cancellation

Cancellation is a synchronization barrier, not merely a request to an SDK.

When `cancel` arrives:

1. Atomically transition the matching request to `Cancelling`; reject later PCM and normal events immediately.
2. Start asynchronous backend cleanup without blocking further reads from the control pipe.
3. Wait for backend callbacks that can still enqueue output to stop or become rejected.
4. Acquire exclusive ownership of the audio writer boundary.
5. Let an already in-flight write finish, then discard every queued but uncommitted chunk for that request.
6. Snapshot the aligned `committed_bytes` count.
7. Atomically win the `Cancelled` terminal transition.
8. Send one `synthesis_cancelled` containing that committed count.
9. Release the per-session synthesis gate and accept the replacement `sapi_speak` immediately.

Example:

```json
{"event":"synthesis_cancelled","speak_id":41,"audio_bytes_written":18432}
```

After this record, the provider must write no PCM and no normal event for request 41. Events already written to the control pipe before the provider received cancellation may still be in flight; CoreEngine drops stale events after it publishes its local cancellation state.

The entire CoreEngine cancellation transaction—sending `cancel`, receiving this acknowledgement, and draining exactly the declared bytes—has one 500 ms budget. Healthy providers should normally acknowledge in tens of milliseconds, leaving room for scheduling and pipe drain.

Continue reading control records while cleanup runs, but gate synthesis dispatch until the acknowledgement has been sent and the old request's engine lease, event sequence, and audio queue have been released. Equally, do not hold a global engine lock while waiting; cancellation of one client must not stall unrelated sessions.

## Event ordering and timing

Within one request, serialize normalized word, sentence, bookmark, log, and terminal events through one request-local sequence. This prevents concurrent SDK callbacks from reordering records on the control pipe.

All normal events must precede the request's terminal event. Once cancellation is visible, discard pending normal events. A callback that cannot be mapped correctly is omitted rather than allowed to delay or corrupt the rest of the request.

`audio_offset_ms` is relative to the beginning of the provider's final PCM stream. If the provider trims, inserts, or otherwise changes audio duration, adjust event offsets by the exact same transformation. CoreEngine converts milliseconds to a whole-frame byte position using the negotiated format.

**Known limitation:** control and audio are independent byte streams. The current protocol's terminal byte barriers give exact end-of-request synchronization, but they do not attach every boundary event to an audio-byte release point. Current CoreEngine does not repair arbitrarily late provider metadata. Providers should emit normalized events as promptly as their SDK allows and must not assume that sending an event after its associated PCM will preserve SAPI event-before-audio ordering.

## Failures and logs

Logs use `info`, `warning`, `error`, or `fatal`, include the related `speak_id`, and should contain technical `message` text plus optional user-safe `friendly_text`.

Validate requests before admitting synthesis whenever possible. A reusable `error` outcome is safe only after the provider has quiesced the request's event and audio producers and proven that no request bytes remain queued or in flight. For a failure after audio has started:

- stop accepting PCM and events;
- quiesce the audio writer;
- report the failure;
- close or quarantine the session if exact remaining audio cannot be proven.

**Known limitation:** the current protocol has exact terminal byte barriers for success and cancellation but no separate byte-counted failure terminal. A midstream `log` with severity `error` is not an audio-drain boundary. If exact quiescence cannot be proven, send the diagnostic if possible and close the session so CoreEngine rebuilds it on a later request. Do not send an error log and then continue writing PCM as though the session were synchronized. For an unrecoverable provider failure, send `fatal` if possible, close the pipes, and exit according to the host health policy.

Never swallow an audio-writer exception. Once the final egress writer fails, reject new audio, fault the request, wake teardown, and prevent a successful completion event from being sent for data that was never deliverable.

## Deadlines and responsiveness

Design against CoreEngine's real budgets:

| Operation | CoreEngine budget |
| :--- | :--- |
| Provider pipe readiness after launch | 1,000 ms total |
| One synchronous control write | 1,500 ms |
| Initialization `info` response read | 1,500 ms |
| Active request without matching progress | 1,500 ms |
| Complete cancellation transaction | 500 ms total |

These are failure boundaries, not batching recommendations. A healthy local provider should create pipes, answer `info`, and process `cancel` much faster.

Keep the control reader independent from backend synthesis and the audio writer. Never wait for a long synthesis call on the command-reading loop. Do not hold a shared mutex across SDK calls, pipe I/O, logging flushes, or request completion waits.

Count only matching, recognized request activity as progress. Stale IDs, malformed events, and unrelated client work must not keep a stuck request alive.

## Multi-client lifetime and shutdown

One provider process may serve CoreEngine sessions from several applications at once. Per-session `speak_id` values are not globally unique, so key request state by session plus `speak_id`, not `speak_id` alone.

Disconnecting one client must:

- stop admission for that session;
- cancel and await its active request;
- stop and await its audio writer;
- dispose its two pipes;
- leave other sessions and the shared backend usable.

Process shutdown must reverse ownership order: stop new accepts, stop new engine operations, cancel and await sessions, dispose sessions, dispose the shared backend and pools, flush bounded logs, then release the instance gate.

**Recommended:** arm a configurable shutdown timer only when there are no active sessions, no connection handoff in progress, and no active synthesis. Cancel it when a new client arrives and recheck the conditions atomically before exiting. Backend pool cleanup is not process idle shutdown; a process with an empty pool can still remain orphaned forever.

Treat `shutdown` as a process-wide management request, not an unconditional kill command. SapiManager sends it after a control-only discovery probe. Honor it immediately only when no other session, connection handoff, or synthesis is active. Otherwise defer or ignore it and let the ordinary no-client idle policy stop the process later; discovery must never terminate a provider that is currently serving a screen reader. Because the current wire command has no authorization field, validate the pipe client identity before honoring it.

## Diagnostics without hot-path damage

Correlate every operational marker with session ID, `speak_id`, provider PID, client PID, voice ID, architecture, and negotiated format where applicable.

Useful monotonic timestamps are:

- process start and pipe-ready;
- control and audio acceptance/pairing;
- command received;
- backend dispatch;
- first backend PCM;
- first PCM committed to the pipe;
- first normalized event;
- completion accepted-byte boundary;
- cancellation received, backend stopped, writer barrier acquired, and acknowledgement sent;
- request teardown and engine-lease return.

For cancellation, log accepted bytes, committed bytes, queue depth, writer-lock wait, last write duration, discarded bytes, and acknowledgement latency. These markers separate backend delay from pipe backpressure and CoreEngine/SAPI delay.

Never log credentials, decrypted configuration, or raw user speech by default. Full JSON and generated SSML contain sensitive text and should require an explicit diagnostic setting with clear retention behavior.

## .NET 10 C# implementation profile

This section is recommended for managed providers. It describes an efficient implementation shape, not additional wire requirements.

### Source-generated protocol metadata

Define a closed DTO graph and generate `System.Text.Json` metadata at build time:

```csharp
[JsonSourceGenerationOptions(
    DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull)]
[JsonSerializable(typeof(CommandMessage))]
[JsonSerializable(typeof(EventMessage))]
[JsonSerializable(typeof(InfoResponse))]
[JsonSerializable(typeof(VoicesResponse))]
internal sealed partial class ProviderJsonContext : JsonSerializerContext
{
}
```

Pass the generated `JsonTypeInfo<T>` to every production serialization call. Add a test configuration that disables reflection fallback so a missing generated type fails during development rather than silently becoming a cold-start regression.

Avoid `StreamReader.ReadLineAsync` and intermediate JSON strings on the steady-state control path. Read UTF-8 into a reusable pooled buffer or `PipeReader`, locate LF bytes, and deserialize directly from UTF-8. For writes, reuse an `ArrayBufferWriter<byte>`, serialize through `Utf8JsonWriter`, append LF, and write the resulting memory under the session's control-writer lock:

```csharp
buffer.Clear();
using (var json = new Utf8JsonWriter(buffer))
{
    JsonSerializer.Serialize(json, message, typeInfo);
    json.Flush();
}

buffer.GetSpan(1)[0] = (byte)'\n';
buffer.Advance(1);
await controlPipe.WriteAsync(buffer.WrittenMemory, cancellationToken);
```

The buffer belongs to the serialized writer and cannot be reused until the asynchronous write completes.

### Owned, bounded audio buffers

Use a bounded `Channel<OwnedAudioChunk>` or equivalent queue together with an atomic byte budget unless every chunk is fixed-size. Rent memory from `MemoryPool<byte>` or `ArrayPool<byte>` when the SDK does not already transfer buffer ownership. `MemoryPool<byte>` returns an `IMemoryOwner<byte>`; an `ArrayPool<byte>` buffer needs its own exactly-once owner wrapper or an explicit array-plus-pool representation. One component owns each buffer at a time, and every terminal, queue rejection, cancellation, writer failure, and disconnect path returns it exactly once.

Avoid `ReadOnlyMemory<byte>.ToArray()` and per-chunk wrapper allocation in the hot path. A small value-type descriptor may hold request identity, length, and an `IMemoryOwner<byte>` reference, but measure boxing and channel behavior before claiming zero allocation.

### Async and generated logging

Use asynchronous named-pipe APIs and cancellation tokens, but do not create linked cancellation sources or `Task.Run` operations mechanically for every callback. Prefer direct async state machines and `ValueTask` only for operations that commonly complete synchronously and whose callers follow the consumption rules.

Use source-generated logging or level-aware interpolated-string handlers so disabled debug messages do not evaluate formatting or allocate. High-frequency PCM and boundary logs should be sampled or aggregated.

### Honest allocation goals

Source generation removes reflection metadata work; it does not make class-based JSON DTOs allocation-free. Strings, command objects, fragment lists, and fragment objects still allocate for nontrivial requests. The realistic goal is to eliminate avoidable framing, intermediate-string, buffer-copy, and logging allocations, and to approach zero provider-controlled allocation for PCM chunks when buffer ownership can transfer directly. Claim allocation-free control parsing only if a custom borrowed-span parser has been measured. Startup, request objects, SDK callbacks, networking, model loading, and vendor result objects may still allocate.

Measure three layers separately:

1. transport and protocol with a fake engine;
2. orchestration with deterministic reusable PCM and events;
3. the real SDK/backend.

The difference identifies which allocations the provider owns. For asynchronous work, use EventPipe or PerfView allocation stacks and process-wide GC counters rather than `GC.GetAllocatedBytesForCurrentThread`.

### AzureTtsProvider lessons

The current Azure project demonstrates:

- .NET 10 and source-generated protocol metadata;
- lazy shared engine creation;
- per-connection single-active-request orchestration;
- request-local serialized event callbacks;
- source/SSML offset normalization and duplicate bookmark suppression;
- separate accepted and committed audio counters;
- cancellation synchronized with the audio writer;
- pooled Azure synthesizer contexts;
- deferred debug formatting.

Do not copy these current limitations into a new provider:

- a string allocation for every inbound and outbound control record;
- a copied array and queue object for every PCM callback;
- an unbounded audio queue;
- timing/order-only dual-pipe pairing;
- a one-second control-only pairing delay;
- backend or translation failures that produce a log but no reliable terminal outcome;
- swallowed audio-writer exceptions;
- fire-and-forget worker tasks that teardown does not await;
- no no-client provider-process shutdown;
- backend SDK types exposed through the nominally neutral engine interface.
- 32-bit `speak_id` DTO fields even though CoreEngine's counter is 64-bit.

## Conformance checklist

Treat direct pipe tests, CoreEngine tests, and installed SAPI tests as different layers. A direct provider script is not an end-to-end SAPI test.

### P0: required before release

- Cold launch creates the correctly named pipes within 1,000 ms.
- Duplicate launch leaves the existing host usable.
- A control-only client receives valid `info` and `voices`; the expected response is the next isolated record for compatibility with current SapiManager.
- LF, CRLF, fragmented, coalesced, malformed, invalid UTF-8, and oversized control records are bounded and handled deterministically.
- Source-generated JSON runs with reflection fallback disabled, for example with `JsonSerializerIsReflectionEnabledByDefault=false`.
- Every numeric field is tested with missing, wrong-type, negative, fractional, and overflow values.
- The advertised PCM format passes block-alignment and overflow checks.
- A normal synthesis drains exactly `total_audio_bytes`, with no underrun, overrun, partial frame, duplicate terminal, or trailing PCM.
- Audio reads are varied across awkward byte boundaries, including formats whose block alignment is not two or four.
- Text fragments use noncontiguous UTF-16 source offsets, surrogate pairs, XML-sensitive characters, silence, and repeated bookmark names.
- Unmappable boundaries are suppressed and logged without stopping PCM.
- Cancellation is repeated before first PCM, after first PCM, on a boundary, under a slow reader, and after normal completion while queued audio still drains.
- Every cancellation completes within 500 ms, declares exactly the committed aligned bytes, and emits no later PCM or normal event.
- Two real process-level clients synthesize concurrently without cross-session audio, events, cancellation, or teardown.
- Control/audio PID mismatch and ambiguous concurrent same-process pairing are rejected without crossing data between sessions.
- Disconnecting one client leaves another usable.
- Unauthorized and remote clients are rejected; first-instance squatting fails safely; intended medium/elevated same-user combinations connect under the documented mandatory-integrity policy.
- No-client idle shutdown is armed, cancelled by reconnection, and does not exit while any session or synthesis remains active.
- The Release package installs through SapiManager, exposes at least one registered voice, and speaks through real SAPI.
- A representative screen reader performs keyboard echo, rapid navigation cancellation, and long read-all speech.

### P1: production quality

- Record cold and warm p50, p95, and p99 for pipe-ready, `info`, first backend PCM, first pipe commit, completion, and cancellation acknowledgement.
- Run slow-reader backpressure tests and prove the audio queue has a fixed maximum.
- Vary PCM chunk sizes and prove the byte budget is never exceeded and every rented buffer returns exactly once after success, rejection, cancellation, writer failure, and disconnect.
- Run long synthesis and thousands of short interrupted requests without unbounded heap or handle growth.
- Establish transport-only, orchestration-only, and SDK-inclusive allocation baselines after warm-up.
- Verify no recurring Gen 2 or large-object-heap churn originates from control records or ordinary PCM chunks.
- Verify logs rotate, remain bounded, omit secrets and speech text by default, and retain enough IDs and timestamps to correlate CoreEngine and provider traces.
- Publish and validate every declared architecture independently.

## Known current ecosystem limitations

Provider authors should account for these without treating them as new wire requirements:

- The current protocol does not provide a cross-pipe pairing nonce.
- It has exact success and cancellation byte barriers but no byte-counted failure terminal.
- It does not guarantee per-boundary cross-pipe event/audio ordering.
- SID-scoped pipe names do not replace an explicit security descriptor and identity validation.
- Current CoreEngine uses `info`, `sapi_speak`, and `cancel`; SapiManager uses `info`, `voices`, and `shutdown`; direct `ssml_speak` is not part of the CoreEngine path.

When one of these limitations must be solved in the wire contract, update [ipc_protocol.md](ipc_protocol.md) first, then update this guide and the conformance tests together.
