#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

#if defined(_DEBUG)
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
