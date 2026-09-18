# Task 1 Brief: Characterize Ignored Punctuation Through the Real Control Pipe

## Objectives
1. In `CoreEngine.Tests/ProtocolParsingTests.cpp`:
   Extend `SpeechProtocolUtilsTests.ParseProviderEventTypeMapsAllKnownStrings`, without adding a new test case, to assert that representative punctuation and viseme names remain `Unknown`:
   - `EXPECT_EQ(ParseProviderEventType("punctuation_boundary"), ProviderEventType::Unknown);`
   - `EXPECT_EQ(ParseProviderEventType("viseme"), ProviderEventType::Unknown);`
   - `EXPECT_EQ(ParseProviderEventType("viseme_reached"), ProviderEventType::Unknown);`
2. In `CoreEngine.Tests/WorkerFaultTests.cpp`:
   Add `TEST_F(SapiEngineTests, PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault)` outside any `#if defined(_DEBUG)` block so that the test compiles and runs in both Debug and Release.
   Inside the test:
   - Use `PipeServerWorkerFixture fixture; ASSERT_TRUE(fixture.Initialize()); ASSERT_TRUE(fixture.Start(75));`
   - In Debug (`#if defined(_DEBUG)`): clear test logs and pause event forward (`fixture.worker->PauseNextEventForwardForTest()`).
   - Write control JSON: `{"event":"punctuation_boundary","speak_id":75,"audio_offset_ms":100,"text_offset":5,"text_length":1}\n`
   - In Debug: wait for pause hook (1000ms), check that test logs contain `L"Unknown event received"`, release event forward hook.
   - Write control JSON: `{"event":"synthesis_complete","speak_id":75,"total_audio_bytes":0}\n`
   - Assert `fixture.worker->WaitUntilFinished(nullptr) == S_OK`.
   - Assert `!fixture.worker->IsFaulted()`.
   - Assert `fixture.mockSite->receivedEvents` is empty under `fixture.mockSite->eventsMutex`.
3. Verify:
   - Build Debug x64: `CoreEngine.Tests.vcxproj`.
   - Run the new test under 30s watchdog: `SapiEngineTests.PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault`.
   - Ensure MockProvider quiesces.
