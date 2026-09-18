#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

#if defined(_DEBUG)
TEST_F(SapiEngineTests, InvalidSpeechEventSpeakIdQuarantinesTheWorker)
{
    const std::vector<std::string> invalidEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":\"40\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"word_boundary\",\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"};

    for (const auto &invalidEvent : invalidEvents)
    {
        PipeServerWorkerFixture fixture;
        ASSERT_TRUE(fixture.Initialize());
        ASSERT_TRUE(fixture.Start(40));

        ASSERT_TRUE(fixture.server.WriteControl(invalidEvent));

        EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
        EXPECT_TRUE(fixture.worker->IsFaulted());
    }
}

TEST_F(SapiEngineTests, MalformedRequiredSpeechEventNumbersQuarantineTheWorker)
{
    const std::vector<std::string> malformedEvents{
        "{\"event\":\"word_boundary\",\"speak_id\":41,\"text_offset\":\"0\",\"text_length\":1,\"audio_offset_ms\":0}\n",
        "{\"event\":\"sentence_boundary\",\"speak_id\":41,\"text_offset\":0,\"text_length\":1.5,\"audio_offset_ms\":0}"
        "\n",
        "{\"event\":\"bookmark_reached\",\"speak_id\":41,\"bookmark_name\":\"mark\"}\n"};

    for (const auto &malformedEvent : malformedEvents)
    {
        PipeServerWorkerFixture fixture;
        ASSERT_TRUE(fixture.Initialize());
        ASSERT_TRUE(fixture.Start(41));

        ASSERT_TRUE(fixture.server.WriteControl(malformedEvent));

        EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
        EXPECT_TRUE(fixture.worker->IsFaulted());
    }
}

TEST_F(SapiEngineTests, UnknownNamedEventWithoutSpeakIdQuarantinesTheWorker)
{
    const std::vector<std::string> unknownEventsWithoutSpeakId{
        "{\"event\":\"custom_unknown_action\"}\n",
        "{\"event\":\"another_unrecognized_event\",\"speak_id\":0}\n",
        "{\"event\":\"third_unrecognized_event\",\"speak_id\":\"not_an_int\"}\n"};

    for (const auto &eventPayload : unknownEventsWithoutSpeakId)
    {
        PipeServerWorkerFixture fixture;
        ASSERT_TRUE(fixture.Initialize());
        ASSERT_TRUE(fixture.Start(56));

        ASSERT_TRUE(fixture.server.WriteControl(eventPayload));

        EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
        EXPECT_TRUE(fixture.worker->IsFaulted());
    }
}
#endif

TEST_F(SapiEngineTests, StaleSpeechEventWithValidSpeakIdDoesNotQuarantineTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(43));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":42,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":43,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, MissingEventNameIsSilentlyIgnored)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(55));

    ASSERT_TRUE(fixture.server.WriteControl("{\"speak_id\":55,\"audio_offset_ms\":0}\n"));
    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":123,\"speak_id\":55}\n"));
    ASSERT_TRUE(fixture.server.WriteControl("{}\n"));

    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":55,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}


TEST_F(SapiEngineTests, MalformedBoundaryWhileIdleFaultsTheWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(76));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":76,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":76,\"text_offset\":-1,\"text_length\":4,\"audio_offset_ms\":10}\n"));

    EXPECT_TRUE(fixture.WaitForFault(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, LegacyCompletedFollowedByRealTerminalCompletesSuccessfully)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(77));

    HRESULT waitResult = E_FAIL;
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
        });
    auto joinWait = wil::scope_exit(
        [&]
        {
            if (waitThread.joinable())
            {
                fixture.worker->Stop();
                waitThread.join();
            }
        });

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"completed\",\"speak_id\":77}\n"));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":77,\"total_audio_bytes\":0}\n"));

    waitThread.join();

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, PunctuationBoundaryWithActiveSpeakIdIsIgnoredWithoutFault)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(75));

#if defined(_DEBUG)
    ClearTestLogs();
    fixture.worker->PauseNextEventForwardForTest();
#endif

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"punctuation_boundary\",\"speak_id\":75,\"audio_offset_ms\":100,\"text_offset\":5,\"text_length\":1}\n"));

#if defined(_DEBUG)
    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    {
        const auto logs = GetTestLogs();
        const bool foundDiagnostic = std::any_of(logs.begin(), logs.end(), [](const std::wstring &line)
        {
            return line.find(L"Unknown event received") != std::wstring::npos;
        });
        EXPECT_TRUE(foundDiagnostic);
    }

    fixture.worker->ReleaseEventForwardForTest();
#endif

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":75,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, UnknownNamedEventWithActiveSpeakIdDoesNotFaultWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(72));

    ClearTestLogs();
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"custom_unknown_action\",\"speak_id\":72,\"payload\":\"telemetry\"}\n"));

    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    {
        const auto logs = GetTestLogs();
        const bool foundDiagnostic = std::any_of(logs.begin(), logs.end(), [](const std::wstring &line) {
            return line.find(L"Unknown event received") != std::wstring::npos;
        });
        EXPECT_TRUE(foundDiagnostic);
    }

    fixture.worker->ReleaseEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":72,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, UnknownNamedEventWithStaleSpeakIdIsRejectedByAdmissionWithoutFault)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(73));

    ClearTestLogs();
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"custom_unknown_action\",\"speak_id\":12,\"payload\":\"stale_telemetry\"}\n"));

    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    {
        const auto logs = GetTestLogs();
        const bool foundDiagnostic = std::any_of(logs.begin(), logs.end(), [](const std::wstring &line) {
            return line.find(L"Unknown event received") != std::wstring::npos;
        });
        EXPECT_FALSE(foundDiagnostic);
    }

    fixture.worker->ReleaseEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":73,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, ValidBoundaryArrivingWhileIdleIsSuppressedWithoutFault)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(74));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":74,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        fixture.mockSite->receivedEvents.clear();
    }

    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":74,\"text_offset\":0,\"text_length\":4,\"audio_offset_ms\":10}\n"));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"log\",\"speak_id\":74,\"severity\":\"info\",\"message\":\"fence\"}\n"));

    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    EXPECT_FALSE(fixture.worker->IsFaulted());
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }

    fixture.worker->ReleaseEventForwardForTest();

    ASSERT_TRUE(fixture.worker->Start(75));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":75,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    EXPECT_FALSE(fixture.worker->IsFaulted());
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(32));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":32,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":31,\"total_audio_bytes\":0}\n"));
    const bool eventPaused = fixture.worker->WaitForEventForwardPauseForTest(1000);
    fixture.worker->ReleaseEventForwardForTest();

    EXPECT_TRUE(eventPaused);
    EXPECT_FALSE(fixture.worker->IsFaulted());
    EXPECT_TRUE(fixture.worker->Start(33));
    fixture.worker->Stop();
}

TEST_F(SapiEngineTests, AudioAfterNormalCompletionFaultsIdleWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(24));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":24,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    ASSERT_TRUE(fixture.server.WriteAudio({0xA1, 0xA2}));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FaultPendingRejectsStartBeforeFaultPublicationCompletes)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(28));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":28,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    fixture.worker->PauseNextFaultPublicationForTest();

    ASSERT_TRUE(fixture.server.WriteAudio({0xE1, 0xE2}));
    const bool publicationPaused = fixture.worker->WaitForFaultPublicationPauseForTest(1000);
    EXPECT_TRUE(publicationPaused);
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_FALSE(fixture.worker->Start(29));
    fixture.worker->ReleaseFaultPublicationForTest();

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, FrameAssemblyFailureFaultsWorkerWithoutEscapingThread)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(30));
    fixture.worker->FailNextFrameAssemblyForTest();

    ASSERT_TRUE(fixture.server.WriteAudio({0xF1, 0xF2}));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_FALSE(fixture.worker->IsAudioApartmentActiveForTest());
    EXPECT_EQ(fixture.mockSite->writeCallCount.load(), 0u);
}

TEST_F(SapiEngineTests, ControlThreadEntryExceptionFaultsWorkerWithoutEscapingThread)
{
    SpeechWorker::FailNextControlThreadEntryForTest();
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, AudioAfterCancellationCompletionFaultsIdleWorker)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(25));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread(
        [&]
        {
            cancellationResult = fixture.worker->CancelAndDrain();
        });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":25,\"audio_bytes_written\":0}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(fixture.server.WriteAudio({0xB1, 0xB2}));

    EXPECT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    EXPECT_TRUE(fixture.worker->IsFaulted());
}
#endif
