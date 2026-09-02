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

#if defined(_DEBUG)
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
