# Phase 7C Codex Whole-Branch Review

Date: 2026-09-18

Range: `2862d85850ea8e766e0824f36bd6341b69d8d209..e1b964101c7568fe13eb2c4389a5174f3f6b1364`

Verdict: Approved for integration with one documented pre-existing verification issue deferred to Phase 8.

## Production Review

No production-code findings were identified.

- `PcmDeliveryPolicy` implements the approved pure API and owns no state, locks, buffers, callbacks, clocks, I/O, COM objects, or lifecycle effects.
- Full raw chunks remain accounted while only the declared prefix reaches frame assembly after an upstream terminal.
- Request identity, generation, `Speaking`, and `faultPending` are revalidated immediately before every individual SAPI write.
- `CSapiEngine::OnAudioData` and `ISpTTSEngineSite::Write` remain outside `m_requestMutex`.
- Post-callback batch classification uses freshly reacquired state.
- Accepted-prefix credit, rejected-write cancellation, cancellation drain, terminal arithmetic, and frame-alignment validation preserve the prior behavior.
- `SendCancellation` remains outside `m_requestMutex`.
- Project wiring and the zero-byte-successful-write integration test are correct.

Two independent final reviewers reported no production-code or contract blockers. Both also confirmed that the previously documented assembler-span lifetime and SAPI-site check/use window remain pre-existing deferred risks and were not widened by Phase 7C.

## Verification

- Targeted x64 Debug and Release CoreEngine.Tests builds succeeded independently.
- AntiGravity reported focused Debug 28/28, focused Release 22/22, complete Debug 262/262, and complete Release 216/216 under the 30-second watchdog.
- Independent focused reviewer runs passed Debug 28/28 and Release 22/22.
- Independent Codex Release execution passed 216/216 in 11.9 seconds under the watchdog.
- Independent Codex Debug execution produced one clean 262/262 run; the Phase 7C audio-boundary test also passed 10/10 in isolation.
- `git diff --check` is clean and the tracked worktree is clean.

## Deferred Verification Issue

Two independent complete Debug attempts each produced one intermittent initialization failure in different, unrelated tests. One involved `SpeakWaitsForSynthesisCompleteByteBoundary`; another failed `FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback` at `fixture.Initialize()` after approximately 995 ms. The failing behaviors did not execute, and immediate isolated or complete reruns passed.

Source inspection identified a pre-existing paired-pipe startup race: `PipeClient::TryConnectPipes` discards a successful temporary control connection if audio is not yet available, while MockProvider constructs its control server before its audio server. The failure is therefore not evidence of a Phase 7C PCM-policy regression.

The issue is recorded in the local bug tracker and `.agents/known-issues/paired-pipe-startup-race.md` for Phase 8. Subsequent AntiGravity handoffs must carry the canonical footnote until the correction is merged and the known-issue record is formally closed.

## Scope Decision

Do not mix the PipeClient/MockProvider correction into Phase 7C. Phase 7C is a behavior-preserving SpeechWorker readability refactor whose focused and complete passing evidence is sufficient when read alongside the explicit pre-existing-race record. Fix the connection lifecycle independently in Phase 8 with a deterministic real-Win32-pipe regression.
