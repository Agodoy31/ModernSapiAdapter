# Phase 7C PCM Delivery Policy Implementation Plan

> Implement in the existing Phase 7C worktree with test-driven development. Keep evidence concise: commands, pass counts, elapsed time, and material findings.

**Goal:** Extract pure PCM-delivery decisions from `SpeechWorker` while preserving the established concurrency, byte, cancellation, and SAPI-write contracts.

**Architecture:** Add a stateless `PcmDeliveryPolicy` leaf over existing `SpeechWorkerTypes`. `SpeechWorker` remains the sole mutable supervisor and applies decisions under its existing lock. No callback, pipe, buffer, assembler, clock, or lifecycle ownership moves.

**Baseline:** `d0e42d3dce82b35c398726f77c7775f712888943`

## Task 1: Characterize `S_OK` With Zero Bytes Written

1. Extend the mock SAPI site with a precise mode that returns `S_OK` while setting `*pcbWritten = 0`. Do not change the production callback contract.
2. Add the all-configuration integration test `SapiEngineTests.SuccessfulZeroByteWriteDrainsCancellationBeforeNextSpeak`, proving:
   - Core treats the zero-byte write as rejected;
   - cancellation is sent and drains through the acknowledged terminal boundary;
   - rejected bytes are not credited as delivered;
   - a subsequent request remains usable.
3. Run the single new test in Debug and Release under a 30-second external watchdog. Confirm it passes against the current production implementation; this is characterization, not a deliberately red behavior test.
4. Commit the characterization separately.

## Task 2: Add the Pure Policy With Direct Tests

1. Declare the approved API in `PcmDeliveryPolicy.h`, then add `PcmDeliveryPolicyTests.cpp` and its test-project metadata before implementing any functions.
2. Write all twelve direct tests. Build Debug x64 and require unresolved `PcmDeliveryPolicy` symbols as the red state; syntax, compilation, project-metadata, or test-discovery failures are invalid red states.
3. Implement `PcmDeliveryPolicy.cpp`. Add the header/source to `CoreEngine.vcxproj` and `CoreEngine.vcxproj.filters`. Add `..\CoreEngine\PcmDeliveryPolicy.cpp` to `CoreEngine.Tests.vcxproj` with `PrecompiledHeader=NotUsing` and `ProgramDataBaseFileName=$(IntDir)PcmDeliveryPolicy.pdb`, matching the established direct-policy test pattern.
4. Use compact tables where they improve coverage without hiding failures. The suite must expose these twelve named behavioral groups:
   - speaking chunk frames the full chunk and requests progress/raw accounting;
   - known speaking terminal clips framing but counts the whole raw chunk;
   - speaking at/past terminal frames zero bytes without underflow;
   - cancelling chunk drains with progress/raw accounting;
   - idle and faulted chunk actions remain distinct and effect-free;
   - matching speaking token admits a span;
   - stale token, wrong generation, inactive state, and pending fault reject admission;
   - accepted speaking batch credits its full accepted prefix and checks completion;
   - rejected speaking batch credits only its accepted prefix and begins cancellation;
   - stale, `Idle`, `Faulted`, `Cancelling`, and `faultPending` post-callback outcomes preserve their distinct current behavior;
   - terminal-boundary facts cover exact speaking overrun, exact speaking completion with/without carry, and cancellation raw-byte completion semantics;
   - alignment covers zero alignment, exact frames, and remainders.
5. Keep all policy functions `[[nodiscard]]`, `noexcept`, stateless, bounded, allocation-free, and side-effect free.
6. Run only `PcmDeliveryPolicyTests.*` in Debug and Release under the watchdog, then commit.

## Task 3: Rewire Chunk Ingest and Terminal Arithmetic

1. Replace the decision nesting inside `IngestAudioChunkLocked` with `EvaluateChunkIngest`.
2. Apply the returned action immediately under `m_requestMutex`; leave assembler calls, byte mutations, progress timestamps, logging, fault publication, and boundary checks in `SpeechWorker`.
3. Replace the calculations in `HasSpeakingAudioOverrunLocked`, `IsSpeakingTerminalReachedLocked`, and `IsCancellingTerminalReachedLocked` with the policy facts, then remove those worker helpers if they become unused. Replace the terminal frame-alignment expression in `HandleTerminalEventLocked` with `IsTerminalByteCountFrameAligned`. Preserve the exact fact formulas in the design table and keep lifecycle outcome/application in `SpeechStatePolicy` and `SpeechWorker`.
4. Preserve full raw-chunk accounting and clipped framing exactly.
5. Run the focused partial-frame and terminal tests in both configurations under the watchdog, then commit.

## Task 4: Rewire Span Admission and Batch Outcome

1. Replace `IsAudioDeliveryEligibleLocked` with `EvaluateSpanAdmission` at the existing per-span gate.
2. Keep one evaluation immediately before each span write under `m_requestMutex`; do not cache one decision across a two-span batch.
3. Keep `OnAudioData` outside all state locks.
4. Replace post-delivery branching with `EvaluateBatchOutcome`, evaluated from fresh state after the callback and applied immediately while the same lock remains held.
5. Preserve batch-level accounting: a fully accepted first span remains creditable when a second span is rejected, but stale replacement credits nothing. `faultPending` continues to block span admission without independently suppressing accepted-prefix accounting after a callback.
6. For a rejected write, mutate state, reset the assembler, and capture the cancellation ID under `m_requestMutex`, then call `SendCancellation` only after releasing the lock.
7. Run the focused two-span, blocked-write, cancellation, stale-request, zero-byte, and rejected-write tests in both configurations under the watchdog, then commit.

## Task 5: Verification and Review

1. Use this exact focused filter:

   ```text
   PcmDeliveryPolicyTests.*:PcmFrameAssemblerTests.*:SapiEngineTests.SuccessfulZeroByteWriteDrainsCancellationBeforeNextSpeak:SapiEngineTests.WorkerReassemblesAwkward24BitStereoPipeFragments:SapiEngineTests.StaleSpansAreDroppedBetweenBatchWrites:SapiEngineTests.SpeakWaitsForSynthesisCompleteByteBoundary:SapiEngineTests.TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames:SapiEngineTests.TerminalEventDeclaringFewerBytesThanAlreadyReadFaultsTheWorker:SapiEngineTests.SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks:SapiEngineTests.SynthesisCompleteWaitsForFinalSapiWriteToFinish:SapiEngineTests.InFlightAudioFromCancelledRequestDoesNotIncrementNextRequestBytes:SapiEngineTests.RejectedAudioWriteDrainsCancellationBeforeNextSpeak:SapiEngineTests.RejectedAudioWriteWithFailedCancellationQuarantinesWorker:SapiEngineTests.CancellationDiscardsCarriedPcmBeforeTheNextRequest
   ```

2. Build only `CoreEngine.Tests` x64 Debug. Use `--gtest_list_tests` to require exactly 28 focused tests, then run all 28 under the watchdog.
3. Run the complete Debug suite and require exactly 262 passing tests.
4. After Debug and worktree-owned MockProvider processes are completely quiescent, build only `CoreEngine.Tests` x64 Release. Require exactly 22 focused tests and 216 complete passing tests. Never run Debug and Release builds concurrently.
5. Reuse the Phase 7A `Invoke-WatchdogTest` and MockProvider-quiescence helpers verbatim. Each run must finish inside 30 seconds, exit zero, and contain the final GoogleTest `PASSED` summary in captured stdout; an exit code without that summary is failure.
6. Run `git diff --check` and verify no test, MockProvider, or MSBuild processes remain.
7. Use one fresh Gemini 3.1 Pro subagent for a whole-implementation review after Task 4. Ask it specifically to audit token validation before every write, lock release across SAPI, fresh-state classification, byte clipping/accounting, exhaustive actions, and unintended behavior changes.
8. Correct verified findings and rerun only affected focused tests plus both complete suites when production code changes.
9. Return the branch to Codex for the required whole-branch review. Do not merge, squash, push, delete the worktree, or mark the roadmap complete.

## Required Handoff

Report:

- final head commit and exact files changed;
- focused Debug/Release selected and passed counts;
- full Debug/Release passed counts and elapsed times;
- watchdog implementation/result;
- `git diff --check` result and process quiescence;
- per-task Pro reviewer findings and resolutions;
- any deviation from this contract;
- confirmation that no timeout, protocol, callback, thread, lock ownership, or observable byte behavior changed.

Intermediate historical red/green transcripts are useful when naturally available but are not the product. Never fabricate them or repeat already-proven builds merely to manufacture evidence.
