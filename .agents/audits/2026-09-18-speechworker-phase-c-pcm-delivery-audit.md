# SpeechWorker Phase 7C PCM Delivery Audit

Date: 2026-09-18

Baseline: `d0e42d3dce82b35c398726f77c7775f712888943`

Risk: Tier 3 because the code coordinates concurrent named-pipe input with the SAPI COM output boundary.

## Current Ownership

`SpeechWorker` currently owns every mutable or effectful part of audio delivery:

- the audio thread and named-pipe reads;
- `PcmFrameAssembler` and its carry buffer;
- `m_requestMutex`, `RequestContext`, request tokens, and byte counters;
- progress timestamps, cancellation, fault publication, and waiter notification;
- calls to `CSapiEngine::OnAudioData`.

`CSapiEngine::OnAudioData` snapshots the current `ISpTTSEngineSite`, calls `ISpTTSEngineSite::Write`, and accepts the write only when the HRESULT succeeded and `pcbWritten` equals the requested byte count. `S_OK` with zero or a short count is therefore a rejected write.

## Existing Flow

1. `AudioThreadProc` reads a raw byte chunk from the audio pipe.
2. Under `m_requestMutex`, `IngestAudioChunkLocked` records the current token and classifies the chunk from the downstream state.
3. Speaking requests refresh progress and count the entire raw chunk. If a terminal byte count is already known, only the remaining declared prefix is sent to `PcmFrameAssembler`.
4. Cancelling requests refresh progress, count and discard the raw chunk, then check the cancellation drain boundary.
5. Audio received while idle is a protocol fault. Audio for a faulted session is drained without delivery.
6. A speaking batch may contain two spans: a completed carry frame followed by an aligned zero-copy span.
7. Immediately before every individual SAPI write, the worker reacquires `m_requestMutex` and verifies the request token, generation, `Speaking` state, and absence of `faultPending`.
8. The lock is released across `OnAudioData` and `ISpTTSEngineSite::Write`.
9. After the batch, the worker reacquires the lock, revalidates fresh state, credits only fully accepted spans, and either checks the terminal boundary or begins cancellation after a rejected write.

## Frozen Invariants

- Count the full pipe chunk as raw audio even when a known terminal clips delivery to a prefix.
- Never pass a partial PCM frame to SAPI.
- Validate token and generation before every span write; a two-span batch receives two validations.
- Never hold `m_requestMutex` across `OnAudioData` or `ISpTTSEngineSite::Write`.
- Classify the post-write result from freshly reacquired state, not the state captured before the callback.
- Credit only spans that SAPI accepted completely. A rejected second span preserves credit for an accepted first span.
- Treat successful HRESULT plus zero or short `pcbWritten` as rejection.
- Defer a terminal-boundary check until after delivery when a batch contains spans. Check immediately when assembly produces no spans.
- A stale or replaced batch must not mutate the new request.
- A batch observed in `Cancelling` after the callback may check its drain boundary but must not issue cancellation again.
- Preserve all existing polling intervals, timeouts, callback order, logging, and fault behavior.

## Existing Characterization Coverage

The current suite covers frame carry/reset, awkward 24-bit stereo fragmentation, two-span stale replacement, terminal clipping and overrun, blocked SAPI writes, cancellation drain, rejected writes, failed cancellation quarantine, and cross-request byte isolation. The focused baseline selects 15 Debug tests and 9 Release tests.

The missing characterization is a site that returns `S_OK` while reporting zero bytes written. Add one configuration-independent integration regression proving that this is rejected, cancellation drains, and a following request remains usable.

## Safe Extraction Boundary

Extract a stateless `PcmDeliveryPolicy` that returns typed, owned decisions for:

1. raw-chunk ingest and terminal clipping;
2. per-span admission;
3. post-batch accounting/cancellation classification from fresh state;
4. terminal-position arithmetic and frame-alignment validation.

The policy must not own or receive the assembler, audio pointers/spans, threads, pipes, locks, clocks, callbacks, COM interfaces, logger, notifications, or mutable request state.

## Deferred Findings

These possible defects are outside this behavior-preserving phase and must not be silently fixed here:

1. The carry-derived first span points into `PcmFrameAssembler::m_completedFrame`. A concurrent reset while a SAPI write is blocked may formally invalidate that pointer even if current capacity usually preserves the address.
2. Admission and use remain separate: cancellation or request replacement can occur after the per-span gate and before `OnAudioData` snapshots the SAPI site. The callback carries no request token, so stale PCM may theoretically target a newly installed site.

Record both for later concurrency hardening. Stop Phase 7C if the extraction cannot remain safe without changing either behavior.
