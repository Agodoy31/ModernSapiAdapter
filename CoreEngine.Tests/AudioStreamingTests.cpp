#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, SpeakWaitsForSynthesisCompleteByteBoundary)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    SPVTEXTFRAG frag = {};
    wchar_t text[] = L"Hello world from mock provider";
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));

    HRESULT hr = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &frag, fixture.mockSite.get());

    EXPECT_EQ(hr, S_OK);
    EXPECT_GT(fixture.mockSite->writeCallCount.load(), 0u);
    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), 48000u);
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
        ASSERT_FALSE(fixture.mockSite->requestedWriteSizes.empty());
        EXPECT_TRUE(std::all_of(fixture.mockSite->requestedWriteSizes.begin(),
                                fixture.mockSite->requestedWriteSizes.end(),
                                [](ULONG requestedSize)
                                {
                                    return requestedSize % 2 == 0;
                                }));
    }

    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    EXPECT_GT(fixture.mockSite->receivedEvents.size(), 0u);

    bool foundWordBoundary = false;
    for (const auto &evt : fixture.mockSite->receivedEvents)
    {
        if (evt.eEventId == SPEI_WORD_BOUNDARY)
        {
            foundWordBoundary = true;
            break;
        }
    }
    EXPECT_TRUE(foundWordBoundary);
}

TEST_F(SapiEngineTests, SpeakPreservesNonContiguousSapiSourceOffsets)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    wchar_t firstText[] = L"first";
    wchar_t secondText[] = L"second";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));
    firstFragment.ulTextSrcOffset = 0;

    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));
    secondFragment.ulTextSrcOffset = 23;
    firstFragment.pNext = &secondFragment;

    EXPECT_EQ(fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get()),
              S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
    ASSERT_EQ(fixture.mockSite->receivedEvents.size(), 2u);
    EXPECT_EQ(fixture.mockSite->receivedEvents[0].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(fixture.mockSite->receivedEvents[0].wParam, 5u);
    EXPECT_EQ(fixture.mockSite->receivedEvents[0].lParam, 0);
    EXPECT_EQ(fixture.mockSite->receivedEvents[1].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(fixture.mockSite->receivedEvents[1].wParam, 6u);
    EXPECT_EQ(fixture.mockSite->receivedEvents[1].lParam, 23);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(26));
    fixture.worker->PauseNextEventForwardForTest();
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":26,\"total_audio_bytes\":2}\n"));
    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    ASSERT_TRUE(fixture.server.WriteAudio({0xC1, 0xC2, 0xD1, 0xD2}));
    fixture.worker->ReleaseEventForwardForTest();

    ASSERT_TRUE(fixture.worker->WaitForFaultForTest(1000));
    std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
    EXPECT_EQ(fixture.mockSite->requestedWriteSizes, (std::vector<ULONG>{2}));
    EXPECT_EQ(fixture.mockSite->acceptedAudio, (std::vector<uint8_t>{0xC1, 0xC2}));
}

TEST_F(SapiEngineTests, WorkerReassemblesAwkward24BitStereoPipeFragments)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize(6));
    ASSERT_TRUE(fixture.Start(27));

    ASSERT_TRUE(fixture.server.WriteAudio({0x01}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 1;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 1u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x02, 0x03, 0x04, 0x05}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 5;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 5u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x06, 0x11, 0x12}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 8;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 8u);

    ASSERT_TRUE(fixture.server.WriteAudio({0x13, 0x14, 0x15, 0x16, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26}));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() == 18;
        },
        1000, 1));
    ASSERT_EQ(fixture.worker->RawAudioBytesForTest(), 18u);

    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":27,\"total_audio_bytes\":18}\n"));
    ASSERT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
    ASSERT_FALSE(fixture.mockSite->requestedWriteSizes.empty());
    EXPECT_TRUE(std::all_of(fixture.mockSite->requestedWriteSizes.begin(), fixture.mockSite->requestedWriteSizes.end(),
                            [](ULONG requestedSize)
                            {
                                return requestedSize % 6 == 0;
                            }));
    EXPECT_EQ(fixture.mockSite->acceptedAudio,
              (std::vector<uint8_t>{0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x21, 0x22,
                                    0x23, 0x24, 0x25, 0x26}));
}

TEST_F(SapiEngineTests, SynthesisCompleteWaitsForFinalSapiWriteToFinish)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(42));

    fixture.mockSite->PauseNextWrite();
    ASSERT_TRUE(fixture.server.WriteAudio({0x10, 0x20, 0x30, 0x40}));
    auto releaseWrite = wil::scope_exit(
        [&]
        {
            fixture.mockSite->ReleaseWrite();
        });
    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));
    fixture.worker->PauseNextEventForwardForTest();
    auto releaseEventForward = wil::scope_exit(
        [&]
        {
            fixture.worker->ReleaseEventForwardForTest();
        });
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n"));
    ASSERT_TRUE(fixture.worker->WaitForEventForwardPauseForTest(1000));

    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool waitReturned = false;
    HRESULT waitResult = E_UNEXPECTED;
    std::thread waitThread(
        [&]
        {
            waitResult = fixture.worker->WaitUntilFinished(nullptr);
            {
                std::lock_guard<std::mutex> lock(completionMutex);
                waitReturned = true;
            }
            completionCondition.notify_all();
        });
    auto releaseWriteBeforeJoin = wil::scope_exit(
        [&]
        {
            fixture.mockSite->ReleaseWrite();
            if (waitThread.joinable())
            {
                waitThread.join();
            }
        });

    {
        std::unique_lock<std::mutex> lock(completionMutex);
        EXPECT_FALSE(completionCondition.wait_for(lock, std::chrono::milliseconds(100),
                                                  [&]
                                                  {
                                                      return waitReturned;
                                                  }));
    }
    EXPECT_FALSE(fixture.worker->IsFaulted());

    fixture.worker->ReleaseEventForwardForTest();
    fixture.mockSite->ReleaseWrite();
    waitThread.join();

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), 4u);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}
#endif

TEST_F(SapiEngineTests, SpeakDoesNotHoldTheSessionLockAcrossReentrantGetActions)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    std::atomic_bool reentrantGetActionsRan{false};
    std::atomic_bool reentrantGetOutputCompleted{false};
    std::atomic_bool reentrantGetOutputCompletedBeforeReturn{false};
    HRESULT reentrantGetOutputResult = E_FAIL;
    std::thread reentrantGetOutputThread;
    fixture.mockSite->getActionsCallback = [&]
    {
        bool expected = false;
        if (reentrantGetActionsRan.compare_exchange_strong(expected, true))
        {
            reentrantGetOutputThread = std::thread(
                [&]
                {
                    GUID nestedFormatId = {};
                    WAVEFORMATEX *nestedFormat = nullptr;
                    reentrantGetOutputResult =
                        fixture.engine->GetOutputFormat(nullptr, nullptr, &nestedFormatId, &nestedFormat);
                    CoTaskMemFree(nestedFormat);
                    reentrantGetOutputCompleted = true;
                });

            EXPECT_TRUE(WaitForCondition(
                [&]
                {
                    return reentrantGetOutputCompleted.load();
                },
                200, 5));
            reentrantGetOutputCompletedBeforeReturn = reentrantGetOutputCompleted.load();
        }
        return SPVES_CONTINUE;
    };

    wchar_t text[] = L"reentrant get actions";
    SPVTEXTFRAG fragment = {};
    fragment.pTextStart = text;
    fragment.ulTextLen = static_cast<ULONG>(wcslen(text));

    const auto speakStart = std::chrono::steady_clock::now();
    const HRESULT speakResult =
        fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &fragment, fixture.mockSite.get());
    if (reentrantGetOutputThread.joinable())
    {
        reentrantGetOutputThread.join();
    }

    EXPECT_TRUE(reentrantGetActionsRan.load());
    EXPECT_TRUE(reentrantGetOutputCompletedBeforeReturn.load())
        << "GetOutputFormat was blocked by Speak's session lock during GetActions.";
    EXPECT_TRUE(reentrantGetOutputCompleted.load());
    EXPECT_EQ(reentrantGetOutputResult, S_OK);
    EXPECT_EQ(speakResult, S_OK);
    EXPECT_LT(std::chrono::steady_clock::now() - speakStart, std::chrono::seconds(2));
}

TEST_F(SapiEngineTests, InFlightAudioFromCancelledRequestDoesNotIncrementNextRequestBytes)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(50));

    fixture.mockSite->PauseNextWrite();
    auto releaseWriteGuard = wil::scope_exit(
        [&]
        {
            fixture.mockSite->ReleaseWrite();
        });

    ASSERT_TRUE(fixture.server.WriteAudio({0x11, 0x22, 0x33, 0x44}));
    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));

    fixture.worker->Stop();
    ASSERT_TRUE(fixture.worker->Start(51));

    fixture.mockSite->ReleaseWrite();

    ASSERT_TRUE(fixture.server.WriteAudio({0x55, 0x66, 0x77, 0x88}));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":51,\"total_audio_bytes\":4}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
    EXPECT_EQ(fixture.mockSite->totalBytesWritten.load(), 8u);
    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
        EXPECT_EQ(fixture.mockSite->acceptedAudio,
                  (std::vector<uint8_t>{0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88}));
    }
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, StaleSpansAreDroppedBetweenBatchWrites)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());
    ASSERT_TRUE(fixture.Start(60));

    fixture.mockSite->PauseNextWrite();
    auto releaseWriteGuard = wil::scope_exit(
        [&]
        {
            fixture.mockSite->ReleaseWrite();
        });

    ASSERT_TRUE(fixture.server.WriteAudio({0x11}));

    // Wait for the worker to read the first byte and put it into carry
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return fixture.worker->RawAudioBytesForTest() >= 1;
        },
        1000, 1));

    ASSERT_TRUE(fixture.server.WriteAudio({0x22, 0x33, 0x44}));
    ASSERT_TRUE(fixture.mockSite->WaitForWritePause(1000));

    fixture.worker->Stop();
    ASSERT_TRUE(fixture.worker->Start(61));

    fixture.mockSite->ReleaseWrite();

    ASSERT_TRUE(fixture.server.WriteAudio({0xAA, 0xBB, 0xCC, 0xDD}));
    ASSERT_TRUE(
        fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":61,\"total_audio_bytes\":4}\n"));

    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->writesMutex);
        EXPECT_EQ(fixture.mockSite->acceptedAudio, (std::vector<uint8_t>{0x11, 0x22, 0xAA, 0xBB, 0xCC, 0xDD}));
    }
}
#endif
