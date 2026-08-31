#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpTTSEngineSite.h"
#include "MockSpObjectToken.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/SpeechWorker.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, OutputSiteAbortCancelsTheActiveRequest) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

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
        speakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        returned = true;
    });
    ThreadJoinGuard speakJoin(speakThread);

    ASSERT_TRUE(mockSite->WaitForBytesWritten(9600));

    mockSite->actions = SPVES_ABORT;
    EXPECT_TRUE(speakJoin.Join(2000));
    EXPECT_TRUE(returned.load());
    CoTaskMemFree(pWaveFormat);
    EXPECT_EQ(speakResult, S_OK);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    const bool forwardedLateSentenceBoundary = std::any_of(
        mockSite->receivedEvents.begin(),
        mockSite->receivedEvents.end(),
        [](const SPEVENT& event) { return event.eEventId == SPEI_SENTENCE_BOUNDARY; });
    EXPECT_FALSE(forwardedLateSentenceBoundary);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, AbortObservationRejectsPcmBeforeCancellationTransportStarts) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(44));
    worker.PauseNextAbortTransitionForTest();
    mockSite->actions = SPVES_ABORT;

    HRESULT waitResult = E_FAIL;
    std::thread waitThread([&] { waitResult = worker.WaitUntilFinished(mockSite.get()); });
    ThreadJoinGuard waitJoin(waitThread);
    auto releaseAbortGate = wil::scope_exit([&] { worker.ReleaseAbortTransitionForTest(); });
    ASSERT_TRUE(worker.WaitForAbortTransitionPauseForTest(1000));
    EXPECT_TRUE(worker.WasCancellingAtAbortUnlockForTest())
        << "Abort handling reached its first request-mutex unlock before publishing Cancelling.";

    ASSERT_TRUE(server.WriteAudio({ 0xA1, 0xA2 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 2; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 2u);
    const ULONG writesBeforeCancellationTransport = mockSite->writeCallCount.load();

    worker.ReleaseAbortTransitionForTest();
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":44,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(waitJoin.Join(2000));

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_EQ(writesBeforeCancellationTransport, 0u)
        << "PCM reached SAPI after abort observation but before cancellation state publication.";
}

TEST_F(SapiEngineTests, CancellationRejectsAnInitiallyApprovedEventAtTheSapiBoundary) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_speakIdCounter = 45;
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(45));
    worker.PauseNextEventForwardForTest();

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":45,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    auto releaseEventGate = wil::scope_exit([&] { worker.ReleaseEventForwardForTest(); });
    ASSERT_TRUE(worker.WaitForEventForwardPauseForTest(1000));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    auto releaseEventGateBeforeJoin = wil::scope_exit([&] { worker.ReleaseEventForwardForTest(); });
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":45,\"audio_bytes_written\":0}\n"));

    worker.ReleaseEventForwardForTest();
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_EQ(cancellationResult, S_OK);
    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_TRUE(mockSite->receivedEvents.empty())
        << "An event approved while speaking reached SAPI after cancellation began.";
}
#endif

#if defined(_DEBUG)
TEST_F(SapiEngineTests, CancellationDiscardsCarriedPcmBeforeTheNextRequest) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(13));
    ASSERT_TRUE(server.WriteAudio({ 0xA1 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 1; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 1u);

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteAudio({ 0xA2 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 2; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 2u);
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":13,\"audio_bytes_written\":2}\n"));
    EXPECT_TRUE(cancellationJoin.Join(2000));
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(worker.Start(14));
    ASSERT_TRUE(server.WriteAudio({ 0xB1, 0xB2 }));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":14,\"total_audio_bytes\":2}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(mockSite->writesMutex);
    EXPECT_EQ(mockSite->requestedWriteSizes, (std::vector<ULONG>{ 2 }));
    EXPECT_EQ(mockSite->acceptedAudio, (std::vector<uint8_t>{ 0xB1, 0xB2 }));
}
#endif

TEST_F(SapiEngineTests, RejectedAudioWriteDrainsCancellationBeforeNextSpeak) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    wchar_t firstText[] = L"[delay-cancelled-event] rejected write remains pending";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));

    mockSite->rejectNextWrite = true;
    HRESULT firstSpeakResult = E_FAIL;
    std::atomic_bool firstSpeakReturned = false;
    std::thread firstSpeakThread([&] {
        firstSpeakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    for (int attempt = 0; attempt < 50 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    ASSERT_EQ(mockSite->writeCallCount.load(), 1u);

    Sleep(10);
    EXPECT_FALSE(firstSpeakReturned.load());

    EXPECT_TRUE(firstSpeakJoin.Join(2000));
    EXPECT_EQ(firstSpeakResult, S_OK);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 0u);

    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &secondFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    EXPECT_EQ(mockSite->totalBytesWritten.load(), 9600u);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, RejectedAudioWriteWithFailedCancellationQuarantinesWorker) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    SpeechWorker* faultedWorker = engine->m_pWorker.get();
    PipeClient* faultedClient = engine->m_pClient.get();
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

    mockSite->rejectNextWrite = true;
    engine->FailNextCancellationControlSendForTest();
    HRESULT firstSpeakResult = S_OK;
    std::atomic_bool firstSpeakReturned = false;
    const auto firstSpeakStart = std::chrono::steady_clock::now();
    std::thread firstSpeakThread([&] {
        firstSpeakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    for (int attempt = 0; attempt < 50 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    ASSERT_EQ(mockSite->writeCallCount.load(), 1u);

    for (int attempt = 0; attempt < 100 && !firstSpeakReturned.load(); ++attempt)
    {
        Sleep(10);
    }
    ASSERT_TRUE(firstSpeakReturned.load());

    EXPECT_TRUE(firstSpeakJoin.Join(2000));
    EXPECT_EQ(firstSpeakResult, E_FAIL);
    EXPECT_LT(std::chrono::steady_clock::now() - firstSpeakStart, std::chrono::seconds(1));

    // The failed utterance publishes Faulted but retains its discard-only worker/client until a later Speak owns teardown.
    EXPECT_EQ(engine->m_pWorker.get(), faultedWorker);
    EXPECT_EQ(engine->m_pClient.get(), faultedClient);
    EXPECT_TRUE(faultedWorker->IsFaulted());

    // Discard events accepted before the rejected write. Entering Faulted now cancels both
    // pipe reads, so the provider is not required (or expected) to complete later writes.
    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        mockSite->receivedEvents.clear();
    }

    // No faulted-session PCM or events may cross the SAPI boundary.
    EXPECT_EQ(mockSite->BytesAcceptedAfterRejectedWrite(), 0u);
    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        EXPECT_TRUE(mockSite->receivedEvents.empty())
            << "Faulted provider output reached the SAPI event sink.";
    }

    const ULONG writesBeforeNextSpeak = mockSite->writeCallCount.load();
    const ULONG bytesBeforeNextSpeak = mockSite->totalBytesWritten.load();
    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &secondFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    ASSERT_NE(engine->m_pWorker, nullptr);
    ASSERT_NE(engine->m_pClient, nullptr);
    EXPECT_FALSE(engine->m_pWorker->IsFaulted());
    EXPECT_GT(mockSite->writeCallCount.load(), writesBeforeNextSpeak);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), bytesBeforeNextSpeak + 9600u);
}
#endif

TEST_F(SapiEngineTests, SpeakCancelsPromptlyEvenWhenOutputSiteWriteBlocks) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    // Simulate SAPI waveOut hardware buffer backpressure by delaying Write() for 300ms on first write
    mockSite->writeDelayMs = 300;

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(42));

    // Send an audio chunk to trigger the blocking Write()
    ASSERT_TRUE(server.WriteAudio({ 0x10, 0x20, 0x30, 0x40 }));

    // Wait a brief moment to ensure AudioThread is actively blocked in Write()
    Sleep(30);

    // Now issue SPVES_ABORT from SAPI
    mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();
    
    // Concurrently wait for cancellation request to be read by server
    std::thread serverThread([&] {
        std::string request;
        if (server.ReadControl(request)) {
            server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":42,\"audio_bytes_written\":4}\n");
        }
    });

    HRESULT cancelHr = worker.WaitUntilFinished(mockSite.get());
    const auto cancelDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_EQ(cancelHr, S_OK);
    // Cancellation MUST return promptly (< 100ms), NOT waiting for the 300ms Write() sleep to finish
    EXPECT_LT(cancelDuration, 100);
}

TEST_F(SapiEngineTests, SynthesisCompleteWhileCancellingCompletesPromptly) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(42));

    mockSite->actions = SPVES_ABORT;

    const auto cancelStart = std::chrono::steady_clock::now();

    std::thread serverThread([&] {
        std::string request;
        if (server.ReadControl(request)) {
            server.WriteAudio({ 1, 2, 3, 4 });
            server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n");
        }
    });

    HRESULT hr = worker.WaitUntilFinished(mockSite.get());
    const auto durationMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - cancelStart).count();

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_EQ(hr, S_OK);
    EXPECT_LT(durationMs, 200);
}

TEST_F(SapiEngineTests, IgnoredCancellationTimesOutTheEntireTransaction) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(35));

    HRESULT cancellationResult = S_OK;
    std::atomic_bool cancellationReturned{false};
    const auto cancellationStart = std::chrono::steady_clock::now();
    std::thread cancellationThread([&] {
        cancellationResult = worker.CancelAndDrain();
        cancellationReturned = true;
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));

    const auto cleanupDeadline = cancellationStart + std::chrono::milliseconds(1200);
    while (!cancellationReturned.load() && std::chrono::steady_clock::now() < cleanupDeadline)
    {
        Sleep(10);
    }

    const bool returnedBeforeAcknowledgement = cancellationReturned.load();
    const auto elapsedBeforeAcknowledgement = std::chrono::steady_clock::now() - cancellationStart;
    if (!returnedBeforeAcknowledgement)
    {
        ASSERT_TRUE(server.WriteControl(
            "{\"event\":\"synthesis_cancelled\",\"speak_id\":35,\"audio_bytes_written\":0}\n"));
    }
    EXPECT_TRUE(cancellationJoin.Join(2000));

    EXPECT_TRUE(returnedBeforeAcknowledgement);
    EXPECT_EQ(cancellationResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_GE(elapsedBeforeAcknowledgement, std::chrono::milliseconds(400));
    EXPECT_LT(elapsedBeforeAcknowledgement, std::chrono::milliseconds(1200));
}
