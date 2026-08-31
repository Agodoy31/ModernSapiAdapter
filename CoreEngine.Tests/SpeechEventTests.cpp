#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpTTSEngineSite.h"
#include "MockSpObjectToken.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/SpeechWorker.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, OnSpeechEventMapsAndDispatchesToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    const auto makeEvent = [](auto audioOffset, auto textOffset, auto textLength) {
        return nlohmann::json{
            {"event", "word_boundary"},
            {"audio_offset_ms", audioOffset},
            {"text_offset", textOffset},
            {"text_length", textLength}
        };
    };

    engine->OnSpeechEvent(makeEvent(50u, 17u, 5u));
    engine->OnSpeechEvent(makeEvent(50, 17, 5));
    engine->OnSpeechEvent(makeEvent(50.0, 17.0, 5.0));

    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        ASSERT_EQ(mockSite->receivedEvents.size(), 3u);
        for (const SPEVENT& received : mockSite->receivedEvents)
        {
            EXPECT_EQ(received.eEventId, SPEI_WORD_BOUNDARY);
            EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
            EXPECT_EQ(received.ullAudioStreamOffset, 2400u);
            EXPECT_EQ(received.wParam, 5u);
            EXPECT_EQ(received.lParam, 17);
        }
    }

    engine->OnSpeechEvent(makeEvent(50u, -1, 5u));
    engine->OnSpeechEvent(nlohmann::json{
        {"event", "word_boundary"},
        {"audio_offset_ms", 50u},
        {"text_offset", 17u}
    });
    engine->OnSpeechEvent(nlohmann::json{
        {"event", "word_boundary"},
        {"text_offset", 17u},
        {"text_length", 5u}
    });

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_EQ(mockSite->receivedEvents.size(), 3u);
}

TEST_F(SapiEngineTests, OnSpeechEventPreservesLongAudioOffsets) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    nlohmann::json eventJson;
    eventJson["event"] = "word_boundary";
    eventJson["audio_offset_ms"] = 90000u;
    eventJson["text_offset"] = 0u;
    eventJson["text_length"] = 4u;

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    EXPECT_EQ(mockSite->receivedEvents.front().ullAudioStreamOffset, 4320000u);
}

TEST_F(SapiEngineTests, OnSpeechEventAlignsOffsetsToPcmFrames) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 2, 11099, 66594, 6, 24, 0 };

    nlohmann::json eventJson;
    eventJson["event"] = "bookmark_reached";
    eventJson["audio_offset_ms"] = 9u;
    eventJson["bookmark_name"] = "1";

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT& received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.ullAudioStreamOffset, 594u);
    EXPECT_EQ(received.ullAudioStreamOffset % engine->m_audioFormat.nBlockAlign, 0u);
    CoTaskMemFree(reinterpret_cast<void*>(received.lParam));
}

TEST_F(SapiEngineTests, OnSpeechEventMapsSentenceBoundaryToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    nlohmann::json eventJson;
    eventJson["event"] = "sentence_boundary";
    eventJson["audio_offset_ms"] = 75u;
    eventJson["text_offset"] = 22u;
    eventJson["text_length"] = 9u;

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT& received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_SENTENCE_BOUNDARY);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
    EXPECT_EQ(received.ullAudioStreamOffset, 3600u);
    EXPECT_EQ(received.wParam, 9u);
    EXPECT_EQ(received.lParam, 22);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsBookmarkStringEventToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    nlohmann::json eventJson;
    eventJson["event"] = "bookmark_reached";
    eventJson["audio_offset_ms"] = 100u;
    eventJson["bookmark_name"] = "42";

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT& received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(received.ullAudioStreamOffset, 4800u);
    EXPECT_EQ(received.wParam, 42u);
    ASSERT_NE(received.lParam, 0);
    EXPECT_STREQ(reinterpret_cast<const wchar_t*>(received.lParam), L"42");
    CoTaskMemFree(reinterpret_cast<void*>(received.lParam));
}

TEST_F(SapiEngineTests, MatchingProviderEventsKeepLongRequestAlive) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(34));

    HRESULT waitResult = E_FAIL;
    std::thread waitThread([&] {
        waitResult = worker.WaitUntilFinished(nullptr);
    });
    ThreadJoinGuard waitJoin(waitThread);

    for (int eventIndex = 0; eventIndex < 3; ++eventIndex)
    {
        ASSERT_TRUE(server.WriteControl(
            "{\"event\":\"word_boundary\",\"speak_id\":34,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));
    }
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":34,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(waitJoin.Join(2000));
    EXPECT_EQ(waitResult, S_OK);
    EXPECT_FALSE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, WarningLogDoesNotFaultSession) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(43));

    std::thread serverThread([&] {
        server.WriteControl("{\"event\":\"log\",\"speak_id\":43,\"severity\":\"warning\",\"message\":\"Cancel ignored: speak_id 43 already completed\"}\n");
        server.WriteAudio({ 1, 2, 3, 4 });
        server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":43,\"total_audio_bytes\":4}\n");
    });

    HRESULT hr = worker.WaitUntilFinished(mockSite.get());

    if (serverThread.joinable()) {
        serverThread.join();
    }

    EXPECT_EQ(hr, S_OK);
    EXPECT_FALSE(worker.IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    SpeechWorker* worker = engine->m_pWorker.get();
    ASSERT_NE(worker, nullptr);
    worker->PauseNextEventForwardForTest();

    wchar_t text[] = L"event is paused before its SAPI callback";
    SPVTEXTFRAG fragment = {};
    fragment.pTextStart = text;
    fragment.ulTextLen = static_cast<ULONG>(wcslen(text));

    mockSite->rejectNextWrite = true;
    engine->FailNextCancellationControlSendForTest();
    HRESULT speakResult = S_OK;
    std::thread speakThread([&] {
        speakResult = engine->Speak(0, formatId, pWaveFormat, &fragment, mockSite.get());
    });
    ThreadJoinGuard speakJoin(speakThread);

    const bool pausedBeforeSapiCallback = worker->WaitForEventForwardPauseForTest(1000);
    std::unique_lock<std::mutex> sessionLock(engine->m_sessionMutex);
    for (int attempt = 0; attempt < 100 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    const ULONG writeCallCount = mockSite->writeCallCount.load();
    const bool faulted = worker->WaitForFaultForTest(1000);

    worker->ReleaseEventForwardForTest();
    sessionLock.unlock();
    EXPECT_TRUE(speakJoin.Join(2000));
    CoTaskMemFree(pWaveFormat);

    ASSERT_TRUE(pausedBeforeSapiCallback);
    ASSERT_EQ(writeCallCount, 1u);
    ASSERT_TRUE(faulted);
    EXPECT_EQ(speakResult, E_FAIL);
    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_TRUE(mockSite->receivedEvents.empty())
        << "An event authorized before fault reached SAPI after fault became visible.";
}
#endif
