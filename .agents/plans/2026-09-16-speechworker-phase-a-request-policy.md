# SpeechWorker Phase A Request-State Policy Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: use `subagent-driven-development` task by task. Each task receives an independent specification review and code-quality review before the next task starts.

**Goal:** Extract only request-lifecycle decisions into `SpeechStatePolicy` while preserving every current thread, lock, callback, IPC, timing, residual-state, and fault-publication behavior.

**Architecture:** `SpeechWorker` remains the state and synchronization owner. A pure Level-0 policy consumes immutable request state and already-classified facts, then returns typed decisions that `SpeechWorker` applies under one continuous `m_requestMutex` hold. Event classification remains Phase B work; PCM/frame and write-result classification remain Phase C work.

**Tech stack:** C++20, MSVC, Windows 11 SDK 10.0.26100.0, GoogleTest, Win32 named-pipe integration fixtures, SAPI 5, and PowerShell 7 (`pwsh`) for watchdog execution.

## Global Constraints

- Risk tier is Tier 3 because concurrent lifecycle and SAPI/IPC coordination are touched.
- Build only x64 locally. x86 is prohibited. The ARM64 exception record below applies to this phase.
- Add no thread, queue, callback, mutex, condition variable, sleep, fixed delay, polling interval, timeout, allocation, IPC operation, COM call, or logging call.
- Preserve the only nested production lock order: `m_eventForwardMutex` then `m_requestMutex`.
- Hold `m_requestMutex` continuously across fact capture, policy evaluation, current-token validation, and decision application.
- Preserve `ResetToIdleLocked()` as a partial lifecycle reset. Do not substitute `RequestContext::Reset()`.
- Preserve debug-hook placement relative to state mutation, unlock, callback, and fault publication.
- Keep Phase B event classification and Phase C PCM/write classification in `SpeechWorker`.
- New policy code is `noexcept`, allocation-free, stateless, nonblocking, bounded O(1), and free of I/O, callbacks, logging, locks, waits, clock reads, and notifications.
- C++ blocks use Allman braces. Headers are self-contained. `pch.h` remains first where the production PCH contract requires it.
- Use targeted CoreEngine and CoreEngine.Tests builds only.

## ARM64 Verification Exception

```text
Rule: BTW-01 and ModernSapiAdapter profile 10.1.1 (verify every supported architecture before integration).
Scope: SpeechWorker Phase A request-state policy extraction.
Risk tier: Tier 3.
Reason for exception: The available environment does not contain the ARM64 dependency libraries/toolchain required to link CoreEngine.Tests; the repository owner explicitly directed agents not to attempt ARM64 builds in this environment.
Risk introduced: A compile or linkage regression specific to ARM64 could remain undetected locally.
Compensating safeguards: The extracted policy uses fixed-width standard integer types, no architecture intrinsics, no packing change, no ABI export, and no new dependency. Debug and Release x64 builds and complete test suites are mandatory. No ARM64 artifact is shipped from this evidence.
Evidence reviewed: Existing project support matrix, unavailable ARM64 library state, x64 baseline, and the repository owner's prior explicit instruction.
Approver: Andres Godoy, repository owner.
Approval date: 2026-09-16.
Status: Active for this phase only.
Expiry or follow-up condition: Re-run the targeted CoreEngine and CoreEngine.Tests ARM64 build before producing or claiming an ARM64 release, or when the missing ARM64 dependencies become available.
```

## Files

- Create `CoreEngine/SpeechStatePolicy.h`.
- Create `CoreEngine/SpeechStatePolicy.cpp`.
- Create `CoreEngine.Tests/SpeechStatePolicyTests.cpp`.
- Modify `CoreEngine/SpeechWorker.cpp`.
- Modify `CoreEngine/SpeechWorker.h` only if obsolete private declarations can be removed after rewiring.
- Modify `CoreEngine/CoreEngine.vcxproj`.
- Modify `CoreEngine/CoreEngine.vcxproj.filters`.
- Modify `CoreEngine.Tests/CoreEngine.Tests.vcxproj`.
- Write evidence beneath `.agents/sdd/phase7a/`.

## Exact Verification Setup

Run every command from the implementation worktree root. Do not hard-code the planning worktree path.

Launch the verification shell with `pwsh -NoLogo -NoProfile`. Every PowerShell block in this plan requires PowerShell 7 because the watchdog uses `ProcessStartInfo.ArgumentList`; Windows PowerShell 5.1 is unsupported for these commands.

```powershell
if ($PSVersionTable.PSVersion.Major -lt 7) { throw 'Phase A verification requires PowerShell 7 (pwsh).' }
$repoRoot = (Resolve-Path '.').Path
$repoPrefix = [System.IO.Path]::GetFullPath($repoRoot).TrimEnd([char[]] @('\', '/')) + [System.IO.Path]::DirectorySeparatorChar
$msbuild = 'C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe'
$debugExe = Join-Path $repoRoot 'bin\CoreEngine.Tests\x64\Debug\CoreEngine.Tests.exe'
$releaseExe = Join-Path $repoRoot 'bin\CoreEngine.Tests\x64\Release\CoreEngine.Tests.exe'
$evidence = Join-Path $repoRoot '.agents\sdd\phase7a'

$focusedFilter = 'SpeechStatePolicyTests.*:SpeechProtocolUtilsTests.RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum:SapiEngineTests.SpeakWaitsForSynthesisCompleteByteBoundary:SapiEngineTests.TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames:SapiEngineTests.SynthesisCompleteWaitsForFinalSapiWriteToFinish:SapiEngineTests.OutputSiteAbortCancelsTheActiveRequest:SapiEngineTests.ValidSynthesisCancelledWhileSpeakingCompletesWithoutFault:SapiEngineTests.RejectedAudioWriteDrainsCancellationBeforeNextSpeak:SapiEngineTests.SynthesisCompleteWhileCancellingCompletesPromptly:SapiEngineTests.IgnoredCancellationTimesOutTheEntireTransaction:SapiEngineTests.AddEventsBlockingDoesNotDelayWorkerFaultPublication:SapiEngineTests.SilentActiveRequestTimesOutInsteadOfHoldingSapiForever:SapiEngineTests.StalledTerminalAudioDrainTimesOut:SapiEngineTests.InvalidCancellationBoundaryFaultsTheWorker:SapiEngineTests.MisalignedSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.DuplicateSynthesisCompleteTotalFaultsTheWorker:SapiEngineTests.TerminalEventDeclaringFewerBytesThanAlreadyReadFaultsTheWorker:SapiEngineTests.RequestErrorFailsUtteranceWithoutKillingProvider:SapiEngineTests.FaultPendingRejectsStartBeforeFaultPublicationCompletes:SapiEngineTests.AudioAfterNormalCompletionFaultsIdleWorker'
```

Create `.agents/sdd/phase7a/` and record returned command output with the available native file tools. Do not use shell file-writing commands to create or populate evidence.

Build commands:

```powershell
& $msbuild (Join-Path $repoRoot 'CoreEngine.Tests\CoreEngine.Tests.vcxproj') /t:Build /p:Configuration=Debug /p:Platform=x64 /m
if ($LASTEXITCODE -ne 0) { throw "Debug x64 CoreEngine.Tests build failed: $LASTEXITCODE" }

& $msbuild (Join-Path $repoRoot 'CoreEngine.Tests\CoreEngine.Tests.vcxproj') /t:Build /p:Configuration=Release /p:Platform=x64 /m
if ($LASTEXITCODE -ne 0) { throw "Release x64 CoreEngine.Tests build failed: $LASTEXITCODE" }
```

Fail-closed watchdog and quiescence functions. The wrapper captures both streams in memory, emits a structured result for evidence, and executes provider quiescence in `finally` before propagating the test or cleanup failure:

```powershell
function Get-WorktreeMockProviders
{
    $ownedProviders = @()
    foreach ($candidate in @(Get-Process -Name 'MockProvider' -ErrorAction SilentlyContinue))
    {
        try { $candidatePath = $candidate.Path }
        catch { throw "Cannot inspect MockProvider PID $($candidate.Id): $($_.Exception.Message)" }
        if (-not $candidatePath)
        {
            if ($candidate.HasExited) { continue }
            throw "Cannot resolve executable path for MockProvider PID $($candidate.Id)."
        }
        if ([System.IO.Path]::GetFullPath($candidatePath).StartsWith($repoPrefix, [StringComparison]::OrdinalIgnoreCase))
        {
            $ownedProviders += $candidate
        }
    }
    return $ownedProviders
}

function Assert-WorktreeMockProviderQuiesced
{
    $deadline = [DateTime]::UtcNow.AddSeconds(5)
    do
    {
        $ownedProviders = @(Get-WorktreeMockProviders)
        if ($ownedProviders.Count -eq 0) { return }
        Start-Sleep -Milliseconds 50
    }
    while ([DateTime]::UtcNow -lt $deadline)

    $ownedProviders = @(Get-WorktreeMockProviders)
    if ($ownedProviders.Count -ne 0)
    {
        throw "Worktree-owned MockProvider processes failed to quiesce: $($ownedProviders.Id -join ', ')"
    }
}

function Invoke-WatchdogTest
{
    param(
        [Parameter(Mandatory)] [string] $Executable,
        [Parameter(Mandatory)] [string[]] $Arguments
    )

    $primaryFailure = $null
    $cleanupFailure = $null
    $timedOut = $false
    $exitCode = $null
    $stdOut = ''
    $stdErr = ''
    try
    {
        $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
        $startInfo.FileName = $Executable
        $startInfo.UseShellExecute = $false
        $startInfo.CreateNoWindow = $true
        $startInfo.RedirectStandardOutput = $true
        $startInfo.RedirectStandardError = $true
        foreach ($argument in $Arguments)
        {
            [void] $startInfo.ArgumentList.Add($argument)
        }

        $process = [System.Diagnostics.Process]::new()
        $process.StartInfo = $startInfo
        if (-not $process.Start()) { throw "Failed to start $Executable" }
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $timedOut = -not $process.WaitForExit(30000)
        if ($timedOut)
        {
            $process.Kill()
        }
        $process.WaitForExit()

        $exitCode = $process.ExitCode
        $stdOut = $stdoutTask.GetAwaiter().GetResult()
        $stdErr = $stderrTask.GetAwaiter().GetResult()

        if ($timedOut)
        {
            $primaryFailure = "TIMEOUT after 30000 ms: $Executable $($Arguments -join ' ')"
        }
        elseif ($process.ExitCode -ne 0)
        {
            $primaryFailure = "Test failure $($process.ExitCode): $Executable $($Arguments -join ' ')"
        }
    }
    catch
    {
        $primaryFailure = $_.Exception.Message
    }
    finally
    {
        try { Assert-WorktreeMockProviderQuiesced }
        catch { $cleanupFailure = $_.Exception.Message }
    }

    [pscustomobject]@{
        Executable = $Executable
        Arguments = $Arguments
        TimedOut = $timedOut
        ExitCode = $exitCode
        StdOut = $stdOut
        StdErr = $stdErr
        Quiesced = -not [bool] $cleanupFailure
        QuiescenceError = $cleanupFailure
    }

    if ($primaryFailure -and $cleanupFailure)
    {
        throw "$primaryFailure; quiescence failure: $cleanupFailure"
    }
    if ($primaryFailure) { throw $primaryFailure }
    if ($cleanupFailure) { throw $cleanupFailure }
}
```

Selected-test count gate after the new tests exist:

```powershell
$listOutput = & $debugExe "--gtest_filter=$focusedFilter" --gtest_list_tests
if ($LASTEXITCODE -ne 0) { throw 'GoogleTest listing failed.' }
$selectedCount = @($listOutput | Where-Object { $_ -match '^\s{2}\S' }).Count
if ($selectedCount -ne 53) { throw "Focused filter selected $selectedCount tests; expected 53." }
$listOutput
```

`Invoke-WatchdogTest` performs the quiescence gate on success, test failure, startup failure, and timeout. `Assert-WorktreeMockProviderQuiesced` may also be invoked directly before the first test run. Copy each returned structured result into the named evidence file using native file tools; do not add shell redirection or shell file-writing commands.

## Required New Unit Tests

Create these exact tests in suite `SpeechStatePolicyTests`:

1. `StartAcceptsOnlyQuiescentNonFaultPendingState`
2. `StartDecisionOwnsAcceptedIdentityWithoutMutatingInput`
3. `UpstreamCompleteAppliesValidatedTerminalFact`
4. `UpstreamCancelledAppliesValidatedTerminalFact`
5. `DuplicateUpstreamTerminalRequestsTwoStageFaultPath`
6. `InvalidUpstreamTerminalBytesRequestFault`
7. `MisalignedUpstreamTerminalBytesRequestFault`
8. `DuplicateMalformedTerminalKeepsDuplicateFirstPrecedence`
9. `UtteranceFailureCapturesCurrentRawByteBoundary`
10. `FailedUpstreamTakesBoundaryPrecedence`
11. `UnfinishedUpstreamContinuesDespiteRetainedCompletedEnum`
12. `UnfinishedUpstreamContinuesDespiteRetainedCancelledEnum`
13. `SpeakingReachedResetsOnlySpeakingLifecycle`
14. `CancellationReachedResetsOnlyCancellingLifecycle`
15. `SpeakingOverrunRequestsFault`
16. `IrrelevantBoundaryFactsAreIgnoredForCurrentState`
17. `CancellationFromActiveSpeakingTransitions`
18. `CancellationFromCompletedSpeakingRetainsCompletedEnumContract`
19. `CancellationFromCancelledSpeakingRetainsCancelledEnumContract`
20. `CancellationAlreadyIdleMapsToAlreadyIdle`
21. `CancellationAlreadyDrainingMapsToAlreadyCancelling`
22. `CancellationFaultedMapsToFaulted`
23. `StopActiveRequestCapturesSpeakId`
24. `StopIdleDoesNothing`
25. `StopFaultedDoesNothing`
26. `CancellationTimeoutPrecedesInactivityTimeout`
27. `CancellationDeadlineRequiresNonzeroExpiredTick`
28. `ExpiredRetainedCancellationDeadlineWhileIdleDoesNotTimeout`
29. `ActiveSynthesisInactivityExpires`
30. `TerminalAudioInactivityExpires`
31. `ClockRegressionDoesNotExpireInactivity`
32. `NoEligibleTimeoutReturnsNone`
33. `WaitTerminalRecognizesIdleFaultAndExit`
34. `PolicyDecisionsDoNotMutateInputContext`

Tests compare every decision field and use copied `RequestContext` values to prove policy evaluation does not mutate caller state. They do not construct pipes, invoke COM, sleep, or allocate production test hooks.

## Task 1: Characterize Residual Lifecycle State Before Extraction

**Files:** modify `CoreEngine.Tests/RequestContextTests.cpp` for direct state characterization and only the cohesive existing SapiEngine test file if an integration assertion is missing. Do not add a production test hook or create a miscellaneous test bucket.

- [ ] Add `SpeechProtocolUtilsTests.RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum`: initialize Completed/Speaking with `upstreamFinished=true`, terminal bytes, token, counters, and completion result; call `TransitionToCancelling`; assert only the documented cancellation fields change and the Completed enum, token, counters, and completion result remain intact.
- [ ] Keep `SapiEngineTests.RequestErrorFailsUtteranceWithoutKillingProvider` as the end-to-end proof that a request-scoped error survives lifecycle reset long enough for the waiter to observe failure.
- [ ] Keep `SapiEngineTests.ValidSynthesisCancelledWhileSpeakingCompletesWithoutFault` unchanged as explicit characterization of the protocol-unexpected but currently supported state.
- [ ] Build Debug x64 with the exact command above.
- [ ] Run only the new characterization tests under `Invoke-WatchdogTest`; record output in `.agents/sdd/phase7a/task1-characterization`.
- [ ] Request an independent per-task review and commit the characterization tests separately.

## Task 2: Add the Pure Policy with TDD

**Files:** create the three policy/test files and update project metadata.

- [ ] Write `SpeechStatePolicy.h` exactly as specified in the approved design.
- [ ] Write all 34 tests listed above before implementation.
- [ ] Add `SpeechStatePolicyTests.cpp` to `CoreEngine.Tests.vcxproj`, build Debug x64, and capture the expected unresolved `SpeechStatePolicy` linker failures in `.agents/sdd/phase7a/task2-red.txt`. A compile failure caused by a malformed test is not an acceptable red state.
- [ ] Implement `SpeechStatePolicy.cpp` with `pch.h` first. Do not include `SpeechProtocolUtils.h`.
- [ ] Add `SpeechStatePolicy.cpp` and `.h` to `CoreEngine.vcxproj` and `.filters`.
- [ ] Add `..\CoreEngine\SpeechStatePolicy.cpp` to `CoreEngine.Tests.vcxproj` with `<PrecompiledHeader>NotUsing</PrecompiledHeader>` and the same `ProgramDataBaseFileName` convention as the other directly compiled production units.
- [ ] Build Debug x64 and execute `SpeechStatePolicyTests.*` under `Invoke-WatchdogTest`; capture `.agents/sdd/phase7a/task2-green`.
- [ ] Run the selected-test count gate and require exactly 53 tests.
- [ ] Request independent specification and code-quality reviews, fix every P0/P1 issue, and commit Task 2.

## Task 3: Rewire Request Admission, Stop, and Cancellation

**Files:** modify `SpeechWorker.cpp`; modify `SpeechWorker.h` only to remove obsolete declarations.

- [ ] Rewire `Start` through `EvaluateStart`, preserving the full reset, generation increment, assembler reset, progress clock store, and return value under one lock hold.
- [ ] Rewire `Stop` through `EvaluateStop`, preserving partial Idle reset, notification, unlock-before-IPC, one cancellation send, and ignored send result.
- [ ] Rewire `BeginCancellationLocked` through `EvaluateBeginCancellation`, preserving HRESULT mapping, retained upstream enum, assembler reset, debug trace placement, and caller-provided deadline.
- [ ] In `UpdateAfterAudioDeliveryLocked`, leave token validation, delivered-byte credit, and write-result classification in place; only call the cancellation lifecycle policy after existing code has classified a rejected write. After the audio loop unlocks, send exactly once with a fresh full `CancellationTimeoutMs`; do not call `FinishCancellation` or wait synchronously on the audio worker.
- [ ] Build Debug x64 and run the 53-test focused filter under the watchdog.
- [ ] Confirm the watchdog result includes successful quiescence, request independent concurrency and code-quality reviews, resolve findings, and commit Task 3.

## Task 4: Rewire Terminal, Failure, Boundary, and Timeout Lifecycle

**Files:** modify `SpeechWorker.cpp`; remove only helpers made genuinely obsolete.

- [ ] In `HandleTerminalEventLocked`, retain event category classification plus terminal-byte presence/alignment computation. Pass all facts to `EvaluateUpstreamTerminal`, which selects duplicate before invalid or misaligned. Emit the matching existing diagnostic and apply its lifecycle decision; do not reject malformed facts before the policy preserves duplicate-first precedence.
- [ ] Preserve the duplicate/invalid terminal two-stage fault transition and notification sequence. The direct `DuplicateMalformedTerminalKeepsDuplicateFirstPrecedence` policy regression and existing integration fault tests must remain green.
- [ ] In `HandleLogEventLocked`, retain severity classification and logging. Invoke `EvaluateUtteranceFailure` only after existing code classifies severity `error`.
- [ ] In `CheckTerminalBoundaryLocked`, compute `TerminalBoundaryFacts` using the current byte/carry helpers, then call `EvaluateTerminalBoundary`. Keep all arithmetic in `SpeechWorker` for Phase C.
- [ ] In `WaitUntilFinished`, load one `now` and one progress tick per iteration, call `EvaluateTimeouts`, preserve cancellation-before-inactivity precedence, unlock before logging/fault publication, and preserve `HRESULT_FROM_WIN32(ERROR_TIMEOUT)`.
- [ ] Delegate `IsWaitTerminalLocked` to `IsWaitTerminal`; leave the condition-variable wait and SAPI `GetActions` outside policy.
- [ ] Build Debug x64, assert the 53-test focused selection, and run the focused filter through the watchdog/quiescence wrapper.
- [ ] Request independent concurrency/state and specification/code-quality reviews, resolve findings, and commit Task 4.

## Task 5: Whole-Branch Verification and Evidence

- [ ] Run the 53-test focused filter 10 consecutive times in Debug x64. Each iteration uses `Invoke-WatchdogTest`, whose `finally` block enforces provider quiescence. Any retry after failure is evidence of a failure, not a pass.
- [ ] Build Debug x64 and run the complete suite:

```powershell
Invoke-WatchdogTest -Executable $debugExe -Arguments @('--gtest_color=no')
```

- [ ] Build Release x64 and run the complete suite:

```powershell
Invoke-WatchdogTest -Executable $releaseExe -Arguments @('--gtest_color=no')
```

- [ ] Confirm every watchdog result includes successful quiescence; invoke `Assert-WorktreeMockProviderQuiesced` directly before the first run and after any non-watchdog diagnostic process.
- [ ] Run `git diff --check` against the implementation branch base and require zero diagnostics.
- [ ] Record exact test counts, elapsed times, build configuration/toolchain, all review findings and resolutions, and the active ARM64 exception in `.agents/sdd/phase7a/report.md`.
- [ ] Request two fresh final reviewers: concurrency/state/lock ordering and contract/test execution. Do not reuse either original approval because those reviews are superseded.
- [ ] Return the implementation branch to Codex for mandatory whole-branch review. Do not merge, push, clean the worktree, or begin Phase B.

## Pre-Code Gate

Implementation begins only after fresh readers confirm:

1. Phase A contains lifecycle decisions only.
2. The observed state matrix matches retained fields and reachable cancellation states.
3. Callback-blocked destruction is documented as unbounded baseline debt.
4. The test project links `SpeechStatePolicy.cpp` directly.
5. The focused filter selects exactly the intended 53 tests.
6. Build, watchdog, timeout-failure, and quiescence commands are executable and fail closed.
7. The ARM64 exception record is accepted for this phase.
