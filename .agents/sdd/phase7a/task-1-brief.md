# Task 1 Brief: Characterize Residual Lifecycle State Before Extraction

**Worktree:** `D:\Projects\ModernSapiAdapter\.worktrees\phase7a-speech-state-policy`
**Plan Document:** `.agents/plans/2026-09-16-speechworker-phase-a-request-policy.md` (Task 1)
**Design Document:** `.agents/specs/2026-09-16-speechworker-phase-a-request-policy-design.md`

## Task Objectives
1. Modify `CoreEngine.Tests/RequestContextTests.cpp`:
   - Add test `SpeechProtocolUtilsTests.RequestContext_TransitionToCancellingPreservesCompletedUpstreamEnum`:
     - Initialize `RequestContext ctx{}`:
       - `ctx.token.speakId = 42;`
       - `ctx.token.generation = 7;`
       - `ctx.upstreamState = UpstreamState::Completed;`
       - `ctx.downstreamState = DownstreamState::Speaking;`
       - `ctx.rawAudioBytesRead = 1000;`
       - `ctx.deliveredAudioBytes = 500;`
       - `ctx.upstreamTerminalBytes = 2000;`
       - `ctx.upstreamFinished = true;`
       - `ctx.faultPending = false;`
       - `ctx.cancellationDeadlineTick = 0;`
       - `ctx.completionHr = S_OK;`
     - Call `ctx.TransitionToCancelling(5000ULL);`
     - Assert:
       - `EXPECT_EQ(ctx.upstreamState, UpstreamState::Completed);` (proves `TransitionToCancelling` intentionally retains `Completed` enum!)
       - `EXPECT_EQ(ctx.downstreamState, DownstreamState::Cancelling);`
       - `EXPECT_FALSE(ctx.upstreamFinished);` (proves `upstreamFinished` is cleared)
       - `EXPECT_EQ(ctx.upstreamTerminalBytes, 0ULL);` (proves terminal bytes cleared)
       - `EXPECT_EQ(ctx.cancellationDeadlineTick, 5000ULL);`
       - `EXPECT_EQ(ctx.token.speakId, 42ULL);` (proves token preserved)
       - `EXPECT_EQ(ctx.token.generation, 7ULL);` (proves generation preserved)
       - `EXPECT_EQ(ctx.rawAudioBytesRead, 1000ULL);` (proves raw audio bytes counter preserved)
       - `EXPECT_EQ(ctx.deliveredAudioBytes, 500ULL);` (proves delivered audio bytes counter preserved)
       - `EXPECT_EQ(ctx.completionHr, S_OK);` (proves completion result preserved)
       - `EXPECT_TRUE(ctx.IsDrainingCancellation());`
2. Preserve strict Allman bracing on all blocks.
3. Build `CoreEngine.Tests\CoreEngine.Tests.vcxproj` in Debug|x64 using MSBuild:
   `& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" "CoreEngine.Tests\CoreEngine.Tests.vcxproj" /t:Build /p:Configuration=Debug /p:Platform=x64 /v:m`
4. Run the characterization tests using `Invoke-WatchdogTest` in pwsh and capture output in `.agents/sdd/phase7a/task1-characterization.txt`.
5. Write the implementation report to `.agents/sdd/phase7a/task-1-report.md`.
