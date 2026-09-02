#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, OutputSiteAbortCancelsTheActiveRequest)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    std::wstring firstText;
    for (int word = 0; word < 80; ++word)
    {
        firstText += L"pending ";
    }
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText.c_str();
    firstFragment.ulTextLen = static_cast<ULONG>(firstText.length());

    HRESULT speakResult = E_FAIL;
    std::atomic<bool> returned = false;
    std::thread speakThread(
        [&]
        {
            speakResult =
                fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
            returned = true;
        });
    ThreadJoinGuard speakJoin(speakThread);

    ASSERT_TRUE(fixture.mockSite->WaitForBytesWritten(9600));

    fixture.mockSite->actions = SPVES_ABORT;
    EXPECT_TRUE(speakJoin.Join(2000));
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(speakResult, S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    const bool forwardedLateSentenceBoundary =
        std::any_of(fixture.mockSite->receivedEvents.begin(), fixture.mockSite->receivedEvents.end(),
                    [](const SPEVENT &event)
                    {
                        return event.eEventId == SPEI_SENTENCE_BOUNDARY;
                    });
    EXPECT_FALSE(forwardedLateSentenceBoundary);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, AbortObservationRejectsPcmBeforeCancellationTransportStarts)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(44));
    fixture.worker->PauseNextAbortTransitionForTest();
    fixture.mockSite->actions = SPVES_ABORT;

    HRESULT waitResult = E_FAIL;
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
        });
    ThreadJoinGuard waitJoin(waitThread);
    auto releaseAbortGate = wil::scope_exit(
        [&]
        {
            fixture.worker->ReleaseAbortTransitionForTest();
        });
    ASSERT_TRUE(fixture.worker->WaitForAbortTransitionPauseForTest(1000));
    EXPECT_TRUE(fixture.worker->WasCancellingAtAbortUnlockForTest())
        << "Abort handling reached its first request-mutex unlock before publishing Cancelling.";

    ASSERT_TRUE(fixture.server.WriteAudio({0xA1, 0xA2}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 2;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 2u);
    const ULONG writesBeforeCancellationTransport = fixture.mockSite->writeCallCount.load();

    fixture.worker->ReleaseAbortTransitionForTest();
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":44,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_EQ(writesBeforeCancellationTransport, 0u)
        << "PCM reached SAPI after abort observation but before cancellation state publication.";
}

TEST_F(SapiEngineTests, CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    fixture.engine->m_speakIdCounter = 45;
    ASSERT_TRUE(fixture.Start(45));
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":45,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    auto releaseEventGate = wil::scope_exit(
        [&]
        {
            fixture.worker->ReleaseEventForwardForTest();
        });
    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread(
        [&]
        {
            cancellationResult = fixture.worker->CancelAndDrain();
        });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    auto releaseEventGateBeforeJoin = wil::scope_exit(
        [&]
        {
            fixture.worker->ReleaseEventForwardForTest();
        });
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":45,\"audio_bytes_written\":0}\n"));

    fixture.worker->ReleaseEventForwardForTest();
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_EQ(cancellationResult, S_OK);
    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    EXPECT_TRUE(fixture.mockSite->receivedEvents.empty())
        << "An event approved while speaking reached SAPI after cancellation began.";
}

TEST_F(SapiEngineTests, CancellationDiscardsCarriedPcmBeforeTheNextRequest)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(13));
    ASSERT_TRUE(fixture.server.WriteAudio({0xA1}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 1;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 1u);

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread(
        [&]
        {
            cancellationResult = fixture.worker->CancelAndDrain();
        });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteAudio({0xA2}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 2;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 2u);
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":13,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(fixture.worker->Start(14));
    ASSERT_TRUE(fixture.server.WriteAudio({0xB1, 0xB2}));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":14,\"total_audio_bytes\":2}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
    EXPECT_EQ(fixture.mockSite->requestedWriteSizes, (std::vector<ULONG>{2}));
    EXPECT_EQ(fixture.mockSite->acceptedAudio, (std::vector<uint8_t>{0xB1, 0xB2}));
}
#endif
