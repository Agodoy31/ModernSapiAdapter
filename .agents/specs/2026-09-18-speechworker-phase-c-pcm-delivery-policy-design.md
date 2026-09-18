# Phase 7C PCM Delivery Policy Design

Date: 2026-09-18

Status: Approved design for implementation

Baseline: `d0e42d3dce82b35c398726f77c7775f712888943`

## Objective

Reduce `SpeechWorker` nesting by extracting deterministic PCM-delivery decisions into a pure leaf module without changing audio bytes, SAPI calls, callback timing, synchronization, request lifecycle, cancellation behavior, or provider protocol.

## Files

Add:

- `CoreEngine/PcmDeliveryPolicy.h`
- `CoreEngine/PcmDeliveryPolicy.cpp`
- `CoreEngine.Tests/PcmDeliveryPolicyTests.cpp`

Modify only as required:

- `CoreEngine/SpeechWorker.h`
- `CoreEngine/SpeechWorker.cpp`
- CoreEngine and CoreEngine.Tests project files
- test fixture support and focused integration tests for the zero-byte successful-write characterization

## Policy Contract

Implement this declared API exactly. Any proposed signature, type, or dependency-boundary change is a stop condition requiring Codex review.

```cpp
namespace PcmDeliveryPolicy
{
enum class ChunkAction : uint8_t
{
    AssembleSpeakingAudio,
    DrainCancellationAudio,
    FaultUnexpectedIdleAudio,
    DrainFaultedSessionAudio,
};

struct ChunkDecision
{
    ChunkAction action;
    uint32_t bytesToFrame;
    bool refreshProgress;
    bool countRawBytes;
};

enum class SpanAdmission : uint8_t
{
    Allow,
    Reject,
};

enum class BatchAction : uint8_t
{
    IgnoreStaleOrInactive,
    CreditAndCheckBoundary,
    CreditAndBeginCancellation,
    CheckCancellationBoundary,
};

struct BatchDecision
{
    BatchAction action;
    uint64_t acceptedBytesToCredit;
};

struct TerminalBoundaryFacts
{
    bool speakingAudioOverrun;
    bool speakingTerminalReached;
    bool cancellationTerminalReached;
};

[[nodiscard]] ChunkDecision EvaluateChunkIngest(
    const RequestContext& context,
    uint32_t bytesRead) noexcept;

[[nodiscard]] SpanAdmission EvaluateSpanAdmission(
    const RequestContext& context,
    const RequestToken& batchToken) noexcept;

[[nodiscard]] BatchDecision EvaluateBatchOutcome(
    const RequestContext& context,
    const RequestToken& batchToken,
    uint64_t fullyAcceptedBytes,
    bool sapiWriteRejected) noexcept;

[[nodiscard]] TerminalBoundaryFacts EvaluateTerminalBoundaryFacts(
    const RequestContext& context,
    bool assemblerHasCarry) noexcept;

[[nodiscard]] bool IsTerminalByteCountFrameAligned(
    uint64_t bytes,
    uint16_t blockAlign) noexcept;
}
```

`EvaluateChunkIngest` has the precondition `bytesRead > 0`; `AudioThreadProc` retains its existing zero-byte early continue.

## Decision Semantics

### Chunk ingest

- `Speaking`: refresh progress, count the full chunk, and frame either the full chunk or the remaining prefix before a known terminal byte boundary.
- `Cancelling`: refresh progress, count and discard the full chunk, then permit the caller to check cancellation completion.
- `Idle`: classify nonempty audio as an unexpected protocol fault; do not count or frame it.
- `Faulted`: drain without framing, counting, progress refresh, or additional fault publication.
- Remaining-byte subtraction and clipping must avoid underflow and preserve current behavior when raw bytes are already at or beyond the declared terminal count. This phase does not invent saturation or fault behavior for cumulative-counter overflow.

### Span admission

Allow only when the active token and generation match the batch token, downstream state is `Speaking`, and `faultPending` is false. The caller evaluates this separately under `m_requestMutex` immediately before every span write.

### Batch outcome

- A stale token or an `Idle`/`Faulted` state ignores the batch and credits nothing.
- A matching `Speaking` request credits the fully accepted prefix. If a write was rejected, begin cancellation; otherwise check the normal terminal boundary.
- A matching request already in `Cancelling` credits no delivered bytes and checks the cancellation drain boundary without reissuing cancellation.
- `Idle` or `Faulted` performs no batch mutation.
- `faultPending` blocks new span admission but does not independently suppress post-callback accounting. If the token still matches and the fresh state is `Speaking`, preserve the current accepted-prefix credit and boundary/rejection path.
- The policy receives only the total bytes from fully accepted spans. It never interprets HRESULTs or calls SAPI.

### Terminal position

The policy returns the three facts consumed by `SpeechStatePolicy`, with the exact current definitions:

| Fact | Exact definition |
|---|---|
| `speakingAudioOverrun` | `rawAudioBytesRead > upstreamTerminalBytes || deliveredAudioBytes > upstreamTerminalBytes` |
| `speakingTerminalReached` | `rawAudioBytesRead == upstreamTerminalBytes && deliveredAudioBytes == upstreamTerminalBytes && !assemblerHasCarry` |
| `cancellationTerminalReached` | `rawAudioBytesRead >= upstreamTerminalBytes` |

These are arithmetic facts only. `SpeechStatePolicy` remains the authority that selects a lifecycle transition from the current state and facts, and `SpeechWorker` applies it.

### Alignment

`blockAlign == 0` is invalid. Otherwise, a terminal byte count is valid only when divisible by `blockAlign`. This is arithmetic classification only; the caller owns faulting and logging.

## Ownership and Locking

`SpeechWorker` retains:

- both threads and all pipe reads;
- `PcmFrameAssembler` and all span/buffer lifetime;
- `m_requestMutex`, state mutation, counters, progress clocks, and condition variables;
- terminal decision application through the existing state policy;
- cancellation IPC, fault publication, logging, and test synchronization hooks;
- the call to `CSapiEngine::OnAudioData`.

`CSapiEngine` retains `ISpTTSEngineSite::Write` and the full-write check.

Policy calls that consume request state occur while `m_requestMutex` is held, and state mutation selected by the decision is applied before that lock is released. The only intentional unlock during classification is around the existing SAPI callback, after which state is sampled again. For `CreditAndBeginCancellation`, byte credit, the state transition, assembler reset, and capture of the cancellation ID occur under the lock; `SendCancellation` remains outside the lock.

## Non-Goals

- No stateful audio-pipeline class.
- No assembler or buffer ownership transfer.
- No changed timeout, polling interval, queue, thread, retry, log volume, or provider message.
- No changed SAPI write batching or callback signature.
- No fix for the deferred span-lifetime or site check/use findings.
- No ARM64 or x86 build in this phase.

## Stop Conditions

Stop and return to Codex if implementation would:

- weaken token and generation validation before each individual write;
- hold `m_requestMutex` across a SAPI callback;
- apply a policy decision after releasing and reacquiring the state lock;
- classify a batch from pre-callback rather than fresh state;
- move state mutation, buffers, I/O, timing, callbacks, cancellation, faults, or logging into the policy;
- alter byte accounting, clipping, callback ordering, terminal semantics, or rejection behavior;
- require fixing either deferred concurrency finding.

## Verification Contract

Use x64 Debug and Release only. Target `CoreEngine.Tests`, never the whole solution. Run tests under an external 30-second watchdog and treat timeout or missing final output as failure.

Required evidence:

- pure policy tests for all closed decision domains and boundary arithmetic;
- the new real worker/SAPI-site zero-byte successful-write regression;
- existing real named-pipe, two-span, blocked-write, terminal, cancellation, and rejected-write regressions;
- one complete Debug and Release `CoreEngine.Tests` suite;
- `git diff --check` and clean quiescence after test processes exit.
