#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, OutputSiteAbortCancelsTheActiveRequest) {
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
    std::thread speakThread([&] {
        speakResult = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
        returned = true;
    });
    ThreadJoinGuard speakJoin(speakThread);

    ASSERT_TRUE(fixture.mockSite->WaitForBytesWritten(9600));

    fixture.mockSite->actions = SPVES_ABORT;
    EXPECT_TRUE(speakJoin.Join(2000));
    EXPECT_TRUE(returned.load());
    EXPECT_EQ(speakResult, S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    const bool forwardedLateSentenceBoundary = std::any_of(
        fixture.mockSite->receivedEvents.begin(),
        fixture.mockSite->receivedEvents.end(),
        [](const SPEVENT& event) { return event.eEventId == SPEI_SENTENCE_BOUNDARY; });
    EXPECT_FALSE(forwardedLateSentenceBoundary);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, AbortObservationRejectsPcmBeforeCancellationTransportStarts) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(44));
    fixture.worker->PauseNextAbortTransitionForTest();
    fixture.mockSite->actions = SPVES_ABORT;

    HRESULT waitResult = E_FAIL;
    std::thread waitThread([&] { waitResult = fixture.worker->WaitUntilFinished(fixture.mockSite.get()); });
    ThreadJoinGuard waitJoin(waitThread);
    auto releaseAbortGate = wil::scope_exit([&] { fixture.worker->ReleaseAbortTransitionForTest(); });
    ASSERT_TRUE(fixture.worker->WaitForAbortTransitionPauseForTest(1000));
    EXPECT_TRUE(fixture.worker->WasCancellingAtAbortUnlockForTest())
        << "Abort handling reached its first request-mutex unlock before publishing Cancelling.";

    ASSERT_TRUE(fixture.server.WriteAudio({ 0xA1, 0xA2 }));
    EXPECT_TRUE(WaitForCondition([&] { return fixture.worker->RawAudioBytesForTest() == 2; }, 1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 2u);
    const ULONG writesBeforeCancellationTransport = fixture.mockSite->writeCallCount.load();

    fixture.worker->ReleaseAbortTransitionForTest();
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":44,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_EQ(writesBeforeCancellationTransport, 0u)
        << "PCM reached SAPI after abort observation but before cancellation state publication.";
}

TEST_F(SapiEngineTests, CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    fixture.engine->m_speakIdCounter = 45;
    ASSERT_TRUE(fixture.Start(45));
    fixture.worker->PauseNextEventForwardForTest();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":45,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    auto releaseEventGate = wil::scope_exit([&] { fixture.worker->ReleaseEventForwardForTest(); });
    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = fixture.worker->CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    auto releaseEventGateBeforeJoin = wil::scope_exit([&] { fixture.worker->ReleaseEventForwardForTest(); });
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":45,\"audio_bytes_written\":0}\n"));

    fixture.worker->ReleaseEventForwardForTest();
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_EQ(cancellationResult, S_OK);
    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    EXPECT_TRUE(fixture.mockSite->receivedEvents.empty())
        << "An event approved while speaking reached SAPI after cancellation began.";
}
#endif

#if defined(_DEBUG)
TEST_F(SapiEngineTests, CancellationDiscardsCarriedPcmBeforeTheNextRequest) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(13));
    ASSERT_TRUE(fixture.server.WriteAudio({ 0xA1 }));
    EXPECT_TRUE(WaitForCondition([&] { return fixture.worker->RawAudioBytesForTest() == 1; }, 1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 1u);

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = fixture.worker->CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));
    ASSERT_TRUE(fixture.server.WriteAudio({ 0xA2 }));
    EXPECT_TRUE(WaitForCondition([&] { return fixture.worker->RawAudioBytesForTest() == 2; }, 1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 2u);
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":13,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(fixture.worker->Start(14));
    ASSERT_TRUE(fixture.server.WriteAudio({ 0xB1, 0xB2 }));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":14,\"total_audio_bytes\":2}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
    EXPECT_EQ(fixture.mockSite->requestedWriteSizes, (std::vector<ULONG>{ 2 }));
    EXPECT_EQ(fixture.mockSite->acceptedAudio, (std::vector<uint8_t>{ 0xB1, 0xB2 }));
}
#endif

TEST_F(SapiEngineTests, RejectedAudioWriteDrainsCancellationBeforeNextSpeak) {
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    wchar_t firstText[] = L"[delay-cancelled-event] rejected write remains pending";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));

    fixture.mockSite->rejectNextWrite = true;
    HRESULT firstSpeakResult = E_FAIL;
    std::atomic_bool firstSpeakReturned = false;
    std::thread firstSpeakThread([&] {
        firstSpeakResult = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    EXPECT_TRUE(WaitForCondition([&] { return fixture.mockSite->writeCallCount.load() > 0; }, 1000, 5));
    ASSERT_EQ(fixture.mockSite->writeCallCount.load(), 1u);

    EXPECT_FALSE(firstSpeakReturned.load());

    EXPECT_TRUE(firstSpeakJoin.Join(2000));
    EXPECT_EQ(firstSpeakResult, S_OK);
    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), 0u);

    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &secondFragment, fixture.mockSite.get()), S_OK);

    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), 9600u);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, RejectedAudioWriteWithFailedCancellationQuarantinesWorker) {
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    SpeechWorker* faultedWorker = fixture.engine->m_pWorker.get();
    PipeClient* faultedClient = fixture.engine->m_pClient.get();
    ASSERT_NE(faultedWorker, nullptr);
    ASSERT_NE(faultedClient, nullptr);

    wchar_t firstText[] = L"[delay-cancelled-event] old request";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));

    wchar_t faultedBookmark[] = L"faulted bookmark";
    SPVTEXTFRAG bookmarkFragment = {};
    bookmarkFragment.State.eAction = SPVA_Bookmark;
    bookmarkFragment.pTextStart = faultedBookmark;
    bookmarkFragment.ulTextLen = static_cast<ULONG>(wcslen(faultedBookmark));
    firstFragment.pNext = &bookmarkFragment;

    fixture.mockSite->rejectNextWrite = true;
    fixture.engine->FailNextCancellationControlSendForTest();
    HRESULT firstSpeakResult = S_OK;
    std::atomic_bool firstSpeakReturned = false;
    const auto firstSpeakStart = std::chrono::steady_clock::now();
    std::thread firstSpeakThread([&] {
        firstSpeakResult = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    EXPECT_TRUE(WaitForCondition([&] { return fixture.mockSite->writeCallCount.load() > 0; }, 1000, 5));
    ASSERT_EQ(fixture.mockSite->writeCallCount.load(), 1u);

    EXPECT_TRUE(WaitForCondition([&] { return firstSpeakReturned.load(); }, 1000, 5));
    ASSERT_TRUE(firstSpeakReturned.load());

    EXPECT_TRUE(firstSpeakJoin.Join(2000));
    EXPECT_EQ(firstSpeakResult, E_FAIL);
    EXPECT_LT(std::chrono::steady_clock::now() - firstSpeakStart, std::chrono::seconds(1));

    // The failed utterance publishes Faulted but retains its discard-only worker/client until a later Speak owns teardown.
    EXPECT_EQ(fixture.engine->m_pWorker.get(), faultedWorker);
    EXPECT_EQ(fixture.engine->m_pClient.get(), faultedClient);
    EXPECT_TRUE(faultedWorker->IsFaulted());

    // Discard events accepted before the rejected write. Entering Faulted now cancels both
    // pipe reads, so the provider is not required (or expected) to complete later writes.
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        fixture.mockSite->receivedEvents.clear();
    }

    // No faulted-session PCM or events may cross the SAPI boundary.
    EXPECT_EQ(fixture.mockSite->BytesAcceptedAfterRejectedWrite(), 0u);
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        EXPECT_TRUE(fixture.mockSite->receivedEvents.empty())
            << "Faulted provider output reached the SAPI event sink.";
    }

    const ULONG writesBeforeNextSpeak = fixture.mockSite->writeCallCount.load();
    const ULONG bytesBeforeNextSpeak = fixture.mockSite->totalBytesWritten.load();
    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &secondFragment, fixture.mockSite.get()), S_OK);

    ASSERT_NE(fixture.engine->m_pWorker, nullptr);
    ASSERT_NE(fixture.engine->m_pClient, nullptr);
    EXPECT_FALSE(fixture.engine->m_pWorker->IsFaulted());
    EXPECT_GT(fixture.mockSite->writeCallCount.load(), writesBeforeNextSpeak);
    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), bytesBeforeNextSpeak + 9600u);
}
#endif

TEST_F(SapiEngineTests, SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    // Simulate SAPI waveOut hardware buffer backpressure by pausing Write() on first write
    fixture.mockSite->PauseNextWrite();
    auto releaseWriteGuard = wil::scope_exit([&] {
        fixture.mockSite->ReleaseWrite();
    });
    ASSERT_TRUE(fixture.Start(42));

    // Send an audio chunk to trigger the blocking Write()
    ASSERT_TRUE(fixture.server.WriteAudio({ 0x10, 0x20, 0x30, 0x40 }));

    // Deterministically wait for AudioThread to enter and pause in Write()
    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));

    // Now issue SPVES_ABORT from SAPI
    fixture.mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();
    
    // Concurrently wait for cancellation request to be read by server
    std::thread serverThread([&] {
        std::string request;
        if (fixture.server.ReadControl(request))
        {
            fixture.server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":42,\"audio_bytes_written\":4}\n");
        }
    });

    HRESULT cancelHr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto cancelDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    fixture.mockSite->ReleaseWrite();

    EXPECT_EQ(cancelHr, S_OK);
    // Cancellation MUST return promptly (< 100ms), NOT waiting for the unblocked Write()
    EXPECT_LT(cancelDuration, 100);
}

TEST_F(SapiEngineTests, SynthesisCompleteWhileCancellingCompletesPromptly) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(42));

    fixture.mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&] {
        std::string request;
        if (fixture.server.ReadControl(request))
        {
            fixture.server.WriteAudio({ 1, 2, 3, 4 });
            fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n");
        }
    });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    EXPECT_EQ(hr, S_OK);
    EXPECT_LT(durationMs, 200);
}

TEST_F(SapiEngineTests, IgnoredCancellationTimesOutTheEntireTransaction) {
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(35));

    HRESULT cancellationResult = S_OK;
    std::atomic_bool cancellationReturned{false};
    const auto cancellationStart = std::chrono::steady_clock::now();
    std::thread cancellationThread([&] {
        cancellationResult = fixture.worker->CancelAndDrain();
        cancellationReturned = true;
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(fixture.server.ReadControl(cancellationRequest));

    EXPECT_TRUE(WaitForCondition([&] { return cancellationReturned.load(); }, 1200));

    const bool returnedBeforeAcknowledgement = cancellationReturned.load();
    const auto elapsedBeforeAcknowledgement = std::chrono::steady_clock::now() - cancellationStart;
    if (!returnedBeforeAcknowledgement)
    {
        ASSERT_TRUE(fixture.server.WriteControl(
            "{\"event\":\"synthesis_cancelled\",\"speak_id\":35,\"audio_bytes_written\":0}\n"));
    }
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeAcknowledgement);
    EXPECT_EQ(cancellationResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(fixture.worker->IsFaulted());
    EXPECT_GE(elapsedBeforeAcknowledgement, std::chrono::milliseconds(400));
    EXPECT_LT(elapsedBeforeAcknowledgement, std::chrono::milliseconds(1200));
}
