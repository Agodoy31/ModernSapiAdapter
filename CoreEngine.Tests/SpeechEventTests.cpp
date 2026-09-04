#include "pch.h"
#include "TestFixtureBase.h"
#include "SpeechProtocolUtils.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, OnSpeechEventMapsAndDispatchesToSite)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    const auto makeEvent = [](auto audioOffset, auto textOffset, auto textLength)
    {
        return nlohmann::json{{"event", "word_boundary"},
                              {"speak_id", 42},
                              {"audio_offset_ms", audioOffset},
                              {"text_offset", textOffset},
                              {"text_length", textLength}};
    };

    const auto json1 = makeEvent(50u, 17u, 5u);
    const ProviderControlEvent event1 = SpeechProtocolUtils::ParseControlEvent(json1);
    engine->OnSpeechEvent(event1);

    const auto json2 = makeEvent(50, 17, 5);
    const ProviderControlEvent event2 = SpeechProtocolUtils::ParseControlEvent(json2);
    engine->OnSpeechEvent(event2);

    const auto json3 = makeEvent(50.0, 17.0, 5.0);
    const ProviderControlEvent event3 = SpeechProtocolUtils::ParseControlEvent(json3);
    engine->OnSpeechEvent(event3);

    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        ASSERT_EQ(mockSite->receivedEvents.size(), 3u);
        for (const SPEVENT &received : mockSite->receivedEvents)
        {
            EXPECT_EQ(received.eEventId, SPEI_WORD_BOUNDARY);
            EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
            EXPECT_EQ(received.ullAudioStreamOffset, 2400u);
            EXPECT_EQ(received.wParam, 5u);
            EXPECT_EQ(received.lParam, 17);
        }
    }

    const auto json4 = makeEvent(50u, -1, 5u);
    const ProviderControlEvent event4 = SpeechProtocolUtils::ParseControlEvent(json4);
    engine->OnSpeechEvent(event4);

    const auto json5 = nlohmann::json{{"event", "word_boundary"}, {"speak_id", 42}, {"audio_offset_ms", 50u}, {"text_offset", 17u}};
    const ProviderControlEvent event5 = SpeechProtocolUtils::ParseControlEvent(json5);
    engine->OnSpeechEvent(event5);

    const auto json6 = nlohmann::json{{"event", "word_boundary"}, {"speak_id", 42}, {"text_offset", 17u}, {"text_length", 5u}};
    const ProviderControlEvent event6 = SpeechProtocolUtils::ParseControlEvent(json6);
    engine->OnSpeechEvent(event6);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_EQ(mockSite->receivedEvents.size(), 3u);
}

TEST_F(SapiEngineTests, OnSpeechEventPreservesLongAudioOffsets)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    nlohmann::json eventJson;
    eventJson["event"] = "word_boundary";
    eventJson["speak_id"] = 42;
    eventJson["audio_offset_ms"] = 90000u;
    eventJson["text_offset"] = 0u;
    eventJson["text_length"] = 4u;

    const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(eventJson);
    engine->OnSpeechEvent(event);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    EXPECT_EQ(mockSite->receivedEvents.front().ullAudioStreamOffset, 4320000u);
}

TEST_F(SapiEngineTests, OnSpeechEventAlignsOffsetsToPcmFrames)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 2, 11099, 66594, 6, 24, 0};

    nlohmann::json eventJson;
    eventJson["event"] = "bookmark_reached";
    eventJson["speak_id"] = 42;
    eventJson["audio_offset_ms"] = 9u;
    eventJson["bookmark_name"] = "1";

    const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(eventJson);
    engine->OnSpeechEvent(event);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT &received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.ullAudioStreamOffset, 594u);
    EXPECT_EQ(received.ullAudioStreamOffset % engine->m_config.audioFormat.nBlockAlign, 0u);
    CoTaskMemFree(reinterpret_cast<void *>(received.lParam));
}

TEST_F(SapiEngineTests, OnSpeechEventMapsSentenceBoundaryToSite)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    nlohmann::json eventJson;
    eventJson["event"] = "sentence_boundary";
    eventJson["speak_id"] = 42;
    eventJson["audio_offset_ms"] = 75u;
    eventJson["text_offset"] = 22u;
    eventJson["text_length"] = 9u;

    const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(eventJson);
    engine->OnSpeechEvent(event);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT &received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_SENTENCE_BOUNDARY);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
    EXPECT_EQ(received.ullAudioStreamOffset, 3600u);
    EXPECT_EQ(received.wParam, 9u);
    EXPECT_EQ(received.lParam, 22);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsBookmarkStringEventToSite)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    nlohmann::json eventJson;
    eventJson["event"] = "bookmark_reached";
    eventJson["speak_id"] = 42;
    eventJson["audio_offset_ms"] = 100u;
    eventJson["bookmark_name"] = "42";

    const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(eventJson);
    engine->OnSpeechEvent(event);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT &received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(received.ullAudioStreamOffset, 4800u);
    EXPECT_EQ(received.wParam, 42u);
    ASSERT_NE(received.lParam, 0);
    EXPECT_STREQ(reinterpret_cast<const wchar_t *>(received.lParam), L"42");
    CoTaskMemFree(reinterpret_cast<void *>(received.lParam));
}

TEST_F(SapiEngineTests, OnSpeechEventMapsBookmarkStringEventWithNonAsciiNameToSite)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    nlohmann::json eventJson;
    eventJson["event"] = "bookmark_reached";
    eventJson["speak_id"] = 42;
    eventJson["audio_offset_ms"] = 100u;
    eventJson["bookmark_name"] = "42_\xE3\x83\x96\xE3\x83\x83\xE3\x82\xAF\xE3\x83\x9E\xE3\x83\xBC\xE3\x82\xAF_\xF0\x9F\x98\x80";

    const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(eventJson);
    engine->OnSpeechEvent(event);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT &received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(received.ullAudioStreamOffset, 4800u);
    EXPECT_EQ(received.wParam, 42u);
    ASSERT_NE(received.lParam, 0);
    EXPECT_STREQ(reinterpret_cast<const wchar_t *>(received.lParam), L"42_\u30D6\u30C3\u30AF\u30DE\u30FC\u30AF_\xD83D\xDE00");
    CoTaskMemFree(reinterpret_cast<void *>(received.lParam));
}

TEST_F(SapiEngineTests, OnSpeechEventRejectsMissingZeroOrMismatchedSpeakId)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    // 1. Missing speak_id (defaults to 0 in ParseControlEvent) -> rejected
    {
        nlohmann::json missingIdJson = {
            {"event", "word_boundary"},
            {"audio_offset_ms", 50u},
            {"text_offset", 0u},
            {"text_length", 4u}
        };
        const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(missingIdJson);
        engine->OnSpeechEvent(event);
    }

    // 2. Explicit zero speak_id -> rejected
    {
        nlohmann::json zeroIdJson = {
            {"event", "word_boundary"},
            {"speak_id", 0},
            {"audio_offset_ms", 50u},
            {"text_offset", 0u},
            {"text_length", 4u}
        };
        const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(zeroIdJson);
        engine->OnSpeechEvent(event);
    }

    // 3. Mismatched speak_id (99 != 42) -> rejected
    {
        nlohmann::json mismatchedIdJson = {
            {"event", "word_boundary"},
            {"speak_id", 99},
            {"audio_offset_ms", 50u},
            {"text_offset", 0u},
            {"text_length", 4u}
        };
        const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(mismatchedIdJson);
        engine->OnSpeechEvent(event);
    }

    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        EXPECT_TRUE(mockSite->receivedEvents.empty());
    }

    // 4. Matching speak_id (42 == 42) -> accepted
    {
        nlohmann::json matchingIdJson = {
            {"event", "word_boundary"},
            {"speak_id", 42},
            {"audio_offset_ms", 50u},
            {"text_offset", 0u},
            {"text_length", 4u}
        };
        const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(matchingIdJson);
        engine->OnSpeechEvent(event);
    }

    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        EXPECT_EQ(mockSite->receivedEvents.size(), 1u);
    }
}

TEST_F(SapiEngineTests, IgnoredEventTypesDoNotCallAddEvents)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite.get());
        engine->m_activeSpeakId = 42;
    }

    const std::vector<nlohmann::json> ignoredEvents = {
        {{"event", "synthesis_complete"}, {"speak_id", 42}, {"total_audio_bytes", 0}},
        {{"event", "synthesis_cancelled"}, {"speak_id", 42}, {"audio_bytes_written", 0}},
        {{"event", "completed"}, {"speak_id", 42}},
        {{"event", "unknown_event_type"}, {"speak_id", 42}}
    };

    for (const auto& json : ignoredEvents)
    {
        const ProviderControlEvent event = SpeechProtocolUtils::ParseControlEvent(json);
        engine->OnSpeechEvent(event);
    }

    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        EXPECT_TRUE(mockSite->receivedEvents.empty());
    }

#if defined(_DEBUG)
    ClearTestLogs();
    nlohmann::json defaultLogJson = {{"event", "log"}, {"speak_id", 42}};
    const ProviderControlEvent defaultLogEvent = SpeechProtocolUtils::ParseControlEvent(defaultLogJson);
    engine->OnSpeechEvent(defaultLogEvent);

    bool foundDefaultLog = false;
    for (const auto& line : GetTestLogs())
    {
        if (line.find(L"Provider error (error): Unknown log") != std::wstring::npos)
        {
            foundDefaultLog = true;
            break;
        }
    }
    EXPECT_TRUE(foundDefaultLog);
#endif
}

TEST_F(SapiEngineTests, EventAfterReplacementSitePublishedIsDropped)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite1 = winrt::make_self<MockSpTTSEngineSite>();
    auto mockSite2 = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    // Publish site 1 with speak_id 1
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite1.get());
        engine->m_activeSpeakId = 1;
    }

    // Publish replacement site 2 with speak_id 2
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite2.get());
        engine->m_activeSpeakId = 2;
    }

    // Dispatch event with old speak_id 1 -> dropped!
    nlohmann::json eventJson1 = {
        {"event", "word_boundary"},
        {"speak_id", 1},
        {"audio_offset_ms", 50u},
        {"text_offset", 0u},
        {"text_length", 4u}
    };
    const ProviderControlEvent event1 = SpeechProtocolUtils::ParseControlEvent(eventJson1);
    engine->OnSpeechEvent(event1);

    {
        std::lock_guard<std::mutex> lock1(mockSite1->eventsMutex);
        EXPECT_TRUE(mockSite1->receivedEvents.empty());
    }
    {
        std::lock_guard<std::mutex> lock2(mockSite2->eventsMutex);
        EXPECT_TRUE(mockSite2->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, EventWithCapturedOldSiteCompletesOnlyAgainstOldSite)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite1 = winrt::make_self<MockSpTTSEngineSite>();
    auto mockSite2 = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_config.audioFormat = {WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0};

    // Publish site 1 with speak_id 1
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite1.get());
        engine->m_activeSpeakId = 1;
    }

    // When site 1 receives AddEvents, publish replacement site 2 with speak_id 2
    mockSite1->onAddEvents = [&](const SPEVENT*, ULONG)
    {
        std::lock_guard<std::mutex> lock(engine->m_siteMutex);
        engine->m_cpSite.copy_from(mockSite2.get());
        engine->m_activeSpeakId = 2;
    };

    nlohmann::json eventJson1 = {
        {"event", "word_boundary"},
        {"speak_id", 1},
        {"audio_offset_ms", 50u},
        {"text_offset", 0u},
        {"text_length", 4u}
    };
    const ProviderControlEvent event1 = SpeechProtocolUtils::ParseControlEvent(eventJson1);
    engine->OnSpeechEvent(event1);

    {
        std::lock_guard<std::mutex> lock1(mockSite1->eventsMutex);
        EXPECT_EQ(mockSite1->receivedEvents.size(), 1u);
    }
    {
        std::lock_guard<std::mutex> lock2(mockSite2->eventsMutex);
        EXPECT_TRUE(mockSite2->receivedEvents.empty());
    }
}

TEST_F(SapiEngineTests, AddEventsBlockingDoesNotDelayWorkerFaultPublication)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(42));

    wil::unique_event addEventsEntered(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    wil::unique_event releaseAddEvents(CreateEventW(nullptr, TRUE, FALSE, nullptr));

    fixture.mockSite->onAddEvents = [&](const SPEVENT*, ULONG)
    {
        SetEvent(addEventsEntered.get());
        WaitForSingleObject(releaseAddEvents.get(), 5000);
    };

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":42,\"text_offset\":0,\"text_length\":1,\"audio_offset_ms\":0}\n"));

    ASSERT_EQ(WaitForSingleObject(addEventsEntered.get(), 2000), WAIT_OBJECT_0);

    // EnterFaultedState must complete and publish m_faultVisible without waiting for AddEvents
    fixture.worker->EnterFaultedState();

    EXPECT_TRUE(fixture.worker->m_faultVisible.load(std::memory_order_acquire));

    SetEvent(releaseAddEvents.get());
    fixture.server.Disconnect();
}

TEST_F(SapiEngineTests, RealControlPipeBoundaryAndBookmarkEndToEnd)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(50));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"word_boundary\",\"speak_id\":50,\"text_offset\":10,\"text_length\":5,\"audio_offset_ms\":100}\n"));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"bookmark_reached\",\"speak_id\":50,\"audio_offset_ms\":200,\"bookmark_name\":\"42_\xE3\x83\x96\xE3\x83\x83\xE3\x82\xAF\xE3\x83\x9E\xE3\x83\xBC\xE3\x82\xAF_\xF0\x9F\x98\x80\"}\n"));

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":50,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    ASSERT_EQ(fixture.mockSite->receivedEvents.size(), 2u);

    const SPEVENT& ev1 = fixture.mockSite->receivedEvents[0];
    EXPECT_EQ(ev1.eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(ev1.elParamType, SPET_LPARAM_IS_UNDEFINED);
    EXPECT_EQ(ev1.ullAudioStreamOffset, fixture.engine->AudioOffsetMsToBytes(100));
    EXPECT_EQ(ev1.wParam, 5u);
    EXPECT_EQ(ev1.lParam, 10);

    const SPEVENT& ev2 = fixture.mockSite->receivedEvents[1];
    EXPECT_EQ(ev2.eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(ev2.elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(ev2.ullAudioStreamOffset, fixture.engine->AudioOffsetMsToBytes(200));
    EXPECT_EQ(ev2.wParam, 42u);
    ASSERT_NE(ev2.lParam, 0);
    EXPECT_STREQ(reinterpret_cast<const wchar_t*>(ev2.lParam), L"42_\u30D6\u30C3\u30AF\u30DE\u30FC\u30AF_\xD83D\xDE00");
    CoTaskMemFree(reinterpret_cast<void*>(ev2.lParam));
}

TEST_F(SapiEngineTests, RealControlPipeLogWithFriendlyTextEndToEnd)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(51));

    ClearTestLogs();

    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"log\",\"speak_id\":51,\"severity\":\"warning\",\"message\":\"Something failed\",\"friendly_text\":\"Check network connection\"}\n"));
    ASSERT_TRUE(fixture.server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":51,\"total_audio_bytes\":0}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    bool foundLog = false;
    for (const auto& line : GetTestLogs())
    {
        if (line.find(L"Provider error (warning): Something failed Check network connection") != std::wstring::npos)
        {
            foundLog = true;
            break;
        }
    }
    EXPECT_TRUE(foundLog);
}



TEST_F(SapiEngineTests, MatchingProviderEventsKeepLongRequestAlive)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(33));

    HRESULT waitResult = E_FAIL;
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
        });
    ThreadJoinGuard waitJoin(waitThread);

    for (int eventIndex = 0; eventIndex < 3; ++eventIndex)
    {
        Sleep(500);
        ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"word_boundary\",\"speak_id\":33,\"text_offset\":0,\"text_"
                                                "length\":1,\"audio_offset_ms\":0}\n"));
    }
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":33,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(waitJoin.Join(2000));
    EXPECT_EQ(waitResult, S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, MultipleWordBoundariesWithinRequestAreProcessed)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(34));

    HRESULT waitResult = E_FAIL;
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
        });
    ThreadJoinGuard waitJoin(waitThread);

    for (int eventIndex = 0; eventIndex < 3; ++eventIndex)
    {
        ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"word_boundary\",\"speak_id\":34,\"text_offset\":0,\"text_"
                                                "length\":1,\"audio_offset_ms\":0}\n"));
    }
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":34,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(waitJoin.Join(2000));
    EXPECT_EQ(waitResult, S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

TEST_F(SapiEngineTests, WarningLogDoesNotFaultSession)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(43));

    std::thread serverThread(
        [&]
        {
            fixture.server.WriteControl("{\"event\":\"log\",\"speak_id\":43,\"severity\":\"warning\",\"message\":"
                                        "\"Cancel ignored: speak_id 43 already completed\"}\n");
            fixture.server.WriteAudio({1, 2, 3, 4});
            fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":43,\"total_audio_bytes\":4}\n");
        });

    HRESULT hr = fixture.worker->WaitUntilFinished(fixture.mockSite.get());

    if (serverThread.joinable())
    {
        serverThread.join();
    }

    EXPECT_EQ(hr, S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    SpeechWorker *worker = fixture.engine->m_pWorker.get();
    ASSERT_NE(worker, nullptr);
    worker->PauseNextEventForwardForTest();

    wchar_t text[] = L"event is paused before its SAPI callback";
    SPVTEXTFRAG fragment = {};
    fragment.pTextStart = text;
    fragment.ulTextLen = static_cast<ULONG>(wcslen(text));

    fixture.mockSite->rejectNextWrite = true;
    fixture.engine->FailNextCancellationControlSendForTest();
    HRESULT speakResult = S_OK;
    std::thread speakThread(
        [&]
        {
            speakResult =
                fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &fragment, fixture.mockSite.get());
        });
    ThreadJoinGuard speakJoin(speakThread);

    const bool pausedBeforeSapiCallback = worker->WaitForEventForwardPauseForTest(1000);
    std::unique_lock<std::mutex> sessionLock(fixture.engine->m_sessionMutex);
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.mockSite->writeCallCount.load() > 0;
        },
        1000, 10));
    const ULONG writeCallCount = fixture.mockSite->writeCallCount.load();
    const bool faulted = worker->WaitForFaultForTest(1000);

    worker->ReleaseEventForwardForTest();
    sessionLock.unlock();
    EXPECT_TRUE(speakJoin.Join(2000));

    ASSERT_TRUE(pausedBeforeSapiCallback);
    ASSERT_EQ(writeCallCount, 1u);
    ASSERT_TRUE(faulted);
    EXPECT_EQ(speakResult, E_FAIL);
    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    EXPECT_TRUE(fixture.mockSite->receivedEvents.empty())
        << "An event authorized before fault reached SAPI after fault became visible.";
}
#endif
