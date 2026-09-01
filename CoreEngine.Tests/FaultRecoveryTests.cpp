#include "pch.h"
#include "TestFixtureBase.h"
#include <sddl.h>

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, PipeClientFailsImmediatelyWhenControlPipeAccessIsDenied) {
    static std::atomic_uint64_t nextPipeId{0};
    const std::wstring pipeName = L"CoreEngineDenied_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(++nextPipeId);
    const std::wstring controlPipePath = PipeSecurityUtils::BuildPipePath(
        pipeName, PipeSecurityUtils::GetCurrentUserSidString(), L"control");

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(D;;GA;;;WD)", SDDL_REVISION_1, &securityDescriptor, nullptr));
    wil::unique_hlocal securityDescriptorHandle(securityDescriptor);

    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    wil::unique_handle deniedControlPipe(CreateNamedPipeW(
        controlPipePath.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        &securityAttributes));
    ASSERT_TRUE(deniedControlPipe);

    PipeClient client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.Connect(pipeName, L"definitely-not-a-provider.exe"), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
}

TEST_F(SapiEngineTests, TimedOutControlReadCancelsItsOverlappedOperationBeforeReturning) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    nlohmann::json response;
    const auto timeoutStart = std::chrono::steady_clock::now();
    EXPECT_EQ(client.ReadControlMessage(response, 100), HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_LT(std::chrono::steady_clock::now() - timeoutStart, std::chrono::seconds(1));

    ASSERT_TRUE(server.WriteControl(
        "{\"response\":\"info\",\"audio_format\":{\"sample_rate\":24000,\"bits_per_sample\":16,\"channels\":1}}\n"));
    ASSERT_EQ(client.ReadControlMessage(response, 1000), S_OK);
    ASSERT_FALSE(response.is_null());
    EXPECT_EQ(response["response"], "info");
}

TEST_F(SapiEngineTests, ControlRecordTimeoutCoversTheWholeFragmentedMessage) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::thread writer([&] {
        server.WriteControl("{\"response\":");
        Sleep(80);
        server.WriteControl("\"info\"");
        Sleep(80);
        server.WriteControl("}\n");
    });
    ThreadJoinGuard writerJoin(writer);

    nlohmann::json response;
    const auto timeoutStart = std::chrono::steady_clock::now();
    const HRESULT result = client.ReadControlMessage(response, 100);
    const auto elapsed = std::chrono::steady_clock::now() - timeoutStart;
    EXPECT_TRUE(writerJoin.Join(1000));

    EXPECT_EQ(result, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_GE(elapsed, std::chrono::milliseconds(80));
    EXPECT_LT(elapsed, std::chrono::milliseconds(150));
}

TEST_F(SapiEngineTests, PipeDisconnectFaultsActiveWorkerWithoutRetryingForever) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(31));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    std::thread waitThread([&] {
        waitResult = fixture.worker->WaitUntilFinished(nullptr);
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    fixture.server.Disconnect();
    EXPECT_TRUE(WaitForCondition([&] { return waitReturned.load(); }, 1000));

    const bool returnedBeforeCleanup = waitReturned.load();
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_TRUE(FAILED(waitResult));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, SilentActiveRequestTimesOutInsteadOfHoldingSapiForever) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(32));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread([&] {
        waitResult = fixture.worker->WaitUntilFinished(nullptr);
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    EXPECT_TRUE(WaitForCondition([&] { return waitReturned.load(); }, 2300));

    const bool returnedBeforeCleanup = waitReturned.load();
    const auto elapsedBeforeCleanup = std::chrono::steady_clock::now() - waitStart;
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_EQ(waitResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeCleanup, std::chrono::milliseconds(1300));
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(2300));
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, InvalidSpeechEventSpeakIdQuarantinesTheWorker) {
    const std::vector<std::string> invalidEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":\"40\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"word_boundary\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"
    };

    for (const auto& invalidEvent : invalidEvents)
    {
        PipeServerWorkerFixture fixture;
        ASSERT_TRUE(fixture.Initialize());
        ASSERT_TRUE(fixture.Start(40));

        ASSERT_TRUE(fixture.server.WriteControl(invalidEvent));

        EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
        EXPECT_TRUE(fixture.worker->IsFaulted());
    }
}

TEST_F(SapiEngineTests, MalformedRequiredSpeechEventNumbersQuarantineTheWorker) {
    const std::vector<std::string> malformedEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":41,\"text_offset\":\"0\",\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"sentence_boundary\",\"speak_id\":41,\"text_offset\":0,\"text_length\":1.5,\"audio_offset_ms\":0}\n",
        "{\"event\":\"bookmark_reached\",\"speak_id\":41,\"bookmark_name\":\"mark\"}\n"
    };

    for (const auto& malformedEvent : malformedEvents)
    {
        PipeServerWorkerFixture fixture;
        ASSERT_TRUE(fixture.Initialize());
        ASSERT_TRUE(fixture.Start(41));

        ASSERT_TRUE(fixture.server.WriteControl(malformedEvent));

        EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
        EXPECT_TRUE(fixture.worker->IsFaulted());
    }
}
#endif

TEST_F(SapiEngineTests, StaleSpeechEventWithValidSpeakIdDoesNotQuarantineTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(43));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":42,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":43,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, InvalidCancellationBoundaryFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(7));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] {
        cancellationResult = fixture.worker->CancelAndDrain();
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_NE(cancellationRequest.find("\"command\":\"cancel\""), std::string::npos);
    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":7}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, MisalignedSynthesisCompleteTotalFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(11));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":11,\"total_audio_bytes\":1}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, MissingSynthesisCompleteTotalFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(21));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":21}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, NonIntegerSynthesisCompleteTotalFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(22));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22,\"total_audio_bytes\":2.5}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}
#endif

TEST_F(SapiEngineTests, IntegralFloatSynthesisCompleteFieldsCompleteTheRequest) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(22));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22.0,\"total_audio_bytes\":0.0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, DuplicateSynthesisCompleteTotalFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(23));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(32));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":32,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":31,\"total_audio_bytes\":0}\n"));
    const bool eventPaused = fixture.worker->WaitForEventForwardPauseForTest(1000);
    fixture.worker->ReleaseEventForwardForTest();

    EXPECT_TRUE(eventPaused);
    EXPECT_FALSE(fixture.worker->IsFaulted());
    EXPECT_TRUE(fixture.worker->Start(33));
    fixture.worker->Stop();
}

TEST_F(SapiEngineTests, AudioAfterNormalCompletionFaultsIdleWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(24));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":24,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    ASSERT_TRUE(fixture.server.WriteAudio({ 0xA1, 0xA2 }));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FaultPendingRejectsStartBeforeFaultPublicationCompletes) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(28));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":28,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    fixture.worker->PauseNextFaultPublicationForTest();

    ASSERT_TRUE(fixture.server.WriteAudio({ 0xE1, 0xE2 }));
    const bool publicationPaused = fixture.worker->WaitForFaultPublicationPauseForTest(1000);
    EXPECT_TRUE(publicationPaused);
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_FALSE(fixture.worker->Start(29));
    fixture.worker->ReleaseFaultPublicationForTest();

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FrameAssemblyFailureFaultsWorkerWithoutEscapingThread) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(30));
    fixture.worker->FailNextFrameAssemblyForTest();

    ASSERT_TRUE(fixture.server.WriteAudio({ 0xF1, 0xF2 }));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_FALSE(fixture.worker->IsAudioApartmentActiveForTest());
    EXPECT_EQ(fixture.mockSite->writeCallCount.load(), 0u);
}

TEST_F(SapiEngineTests, ControlThreadCreationFailureRollsBackAudioThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker::FailNextControlThreadCreationForTest();

    EXPECT_THROW(SpeechWorker worker(engine.get(), &client, 2), std::system_error);
}

TEST_F(SapiEngineTests, ControlThreadEntryExceptionFaultsWorkerWithoutEscapingThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker::FailNextControlThreadEntryForTest();
    SpeechWorker worker(engine.get(), &client, 2);

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, AudioAfterCancellationCompletionFaultsIdleWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(25));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = fixture.worker->CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":25,\"audio_bytes_written\":0}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(fixture.server.WriteAudio({ 0xB1, 0xB2 }));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}
#endif

TEST_F(SapiEngineTests, MisalignedCancellationTotalFaultsTheWorker) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(12));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] { cancellationResult = fixture.worker->CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":12,\"audio_bytes_written\":1}\n"));

    EXPECT_TRUE(cancellationJoin.Join(2000));
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, RequestErrorFailsUtteranceWithoutKillingProvider) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(44));

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&] {
        fixture.server.WriteControl("{\"event\":\"log\",\"speak_id\":44,\"severity\":\"error\",\"message\":\"Synthesis failed: 400 Bad Request\"}\n");
    });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_TRUE(FAILED(hr));
    EXPECT_LT(durationMs, 200);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FatalErrorFaultsSessionAndTriggersRestart) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(45));

    std::thread serverThread([&] {
        fixture.server.WriteControl("{\"event\":\"log\",\"speak_id\":45,\"severity\":\"fatal\",\"message\":\"Fatal provider crash\"}\n");
    });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_EQ(hr, E_FAIL);
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, StalledTerminalAudioDrainTimesOut) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(60));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":60,\"total_audio_bytes\":10000}\n"));

    std::vector<uint8_t> partialAudio(2000, 0x1A);
    ASSERT_TRUE(fixture.server.WriteAudio(partialAudio));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread([&] {
        waitResult = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    EXPECT_TRUE(WaitForCondition([&] { return waitReturned.load(); }, 3000));

    const bool returnedBeforeCleanup = waitReturned.load();
    const auto elapsedBeforeCleanup = std::chrono::steady_clock::now() - waitStart;
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_EQ(waitResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeCleanup, std::chrono::milliseconds(1300));
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(3000));
}

TEST_F(SapiEngineTests, LogEventsDoNotExtendSynthesisInactivityTimeout) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(61));

    HRESULT waitResult = S_OK;
    std::atomic_bool waitReturned{false};
    const auto waitStart = std::chrono::steady_clock::now();
    std::thread waitThread([&] {
        waitResult = fixture.worker->WaitUntilFinished(nullptr);
        waitReturned = true;
    });
    ThreadJoinGuard waitJoin(waitThread);

    std::thread logThread([&] {
        auto loopStart = std::chrono::steady_clock::now();
        while (!waitReturned.load() && std::chrono::steady_clock::now() - loopStart < std::chrono::seconds(3)) {
            fixture.server.WriteControl("{\"event\":\"log\",\"speak_id\":61,\"severity\":\"info\",\"message\":\"progress\"}\n");
            Sleep(500);
        }
    });

    EXPECT_TRUE(WaitForCondition([&] { return waitReturned.load(); }, 2500));
    logThread.join();

    const bool returnedBeforeCleanup = waitReturned.load();
    const auto elapsedBeforeCleanup = std::chrono::steady_clock::now() - waitStart;
    if (!returnedBeforeCleanup)
    {
        fixture.worker->Stop();
    }
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeCleanup);
    EXPECT_EQ(waitResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeCleanup, std::chrono::milliseconds(1300));
    EXPECT_LT(elapsedBeforeCleanup, std::chrono::milliseconds(2500));
}
