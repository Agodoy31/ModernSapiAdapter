# Task 1 Implementation Report: Characterize Ignored Punctuation Through the Real Control Pipe

## Status: DONE

## Overview
Task 1 characterized how unrecognized provider control events (specifically `punctuation_boundary`, `viseme`, and `viseme_reached`) are parsed and handled by the CoreEngine control event pipeline.

## Changes Made
1. `CoreEngine.Tests/ProtocolParsingTests.cpp`
   - Extended `SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings` to assert that:
     - `ParseProviderEventType("punctuation_boundary") == ProviderEventType::Unknown`
     - `ParseProviderEventType("viseme") == ProviderEventType::Unknown`
     - `ParseProviderEventType("viseme_reached") == ProviderEventType::Unknown`
   - Verified that no new test case was added to the fixture.

2. `CoreEngine.Tests/WorkerFaultTests.cpp`
   - Added `TEST_F(SapiEngineTests, PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault)` outside `#if defined(_DEBUG)` blocks to ensure execution in both Debug and Release builds.
   - Guarded the debug hooks (`ClearTestLogs()`, `PauseNextEventForwardForTest()`, `WaitForEventForwardPauseForTest(1000)`, log inspection for `L"Unknown event received"`, and `ReleaseEventForwardForTest()`) with `#if defined(_DEBUG)`.
   - Verified that sending `punctuation_boundary` for active speak_id 75 is ignored without faulting the worker, produces no SAPI events to `mockSite->receivedEvents`, and the worker completes cleanly upon `synthesis_complete`.
   - Enforced strict Allman bracing across all code blocks and lambdas.

## Verification Evidence
1. **Targeted Build**:
   - Command: `& "C:\Program Files\Microsoft Visual Studio\18\Professional\MSBuild\Current\Bin\MSBuild.exe" CoreEngine.Tests\CoreEngine.Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /v:m`
   - Result: Exit code `0`, clean build with 0 errors and 0 warnings.

2. **Test Execution**:
   - Command: `CoreEngine.Tests.exe --gtest_filter=SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault --gtest_color=no`
   - Result: Exit code `0`, PASSED (5 ms).
   - Command: `CoreEngine.Tests.exe --gtest_filter=SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings --gtest_color=no`
   - Result: Exit code `0`, PASSED (0 ms).

3. **MockProvider Quiescence**:
   - Command: `Get-Process -Name MockProvider -ErrorAction SilentlyContinue`
   - Result: Exit code `1` (no active processes found), confirming process quiescence.

4. **Scope Control**:
   - `git status --short` verified only `CoreEngine.Tests/ProtocolParsingTests.cpp` and `CoreEngine.Tests/WorkerFaultTests.cpp` modified.
