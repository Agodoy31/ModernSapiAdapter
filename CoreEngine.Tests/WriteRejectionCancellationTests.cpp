#include "pch.h"
#include "TestFixtureBase.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, RejectedAudioWriteDrainsCancellationBeforeNextSpeak)
{
    EngineInitializedFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    wchar_t firstText[] = L"[delay-cancelled-event] rejected write remains pending";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));

    fixture.mockSite->rejectNextWrite = true;
    HRESULT firstSpeakResult = E_FAIL;
    std::atomic_bool firstSpeakReturned = false;
    std::thread firstSpeakThread([&]
    {
        firstSpeakResult = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    EXPECT_TRUE(WaitForCondition([&]
    {
        return fixture.mockSite->writeCallCount.load() > 0;
    }, 1000, 5));
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
TEST_F(SapiEngineTests, RejectedAudioWriteWithFailedCancellationQuarantinesWorker)
{
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
    std::thread firstSpeakThread([&]
    {
        firstSpeakResult = fixture.engine->Speak(0, fixture.formatId, fixture.pWaveFormat, &firstFragment, fixture.mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    EXPECT_TRUE(WaitForCondition([&]
    {
        return fixture.mockSite->writeCallCount.load() > 0;
    }, 1000, 5));
    ASSERT_EQ(fixture.mockSite->writeCallCount.load(), 1u);

    EXPECT_TRUE(WaitForCondition([&]
    {
        return firstSpeakReturned.load();
    }, 1000, 5));
    ASSERT_TRUE(firstSpeakReturned.load());

    EXPECT_TRUE(firstSpeakJoin.Join(2000));
    EXPECT_EQ(firstSpeakResult, E_FAIL);
    EXPECT_LT(std::chrono::steady_clock::now() - firstSpeakStart, std::chrono::seconds(1));

    EXPECT_EQ(fixture.engine->m_pWorker.get(), faultedWorker);
    EXPECT_EQ(fixture.engine->m_pClient.get(), faultedClient);
    EXPECT_TRUE(faultedWorker->IsFaulted());

    {
        std::lock_guard<std::mutex> lock(fixture.mockSite->eventsMutex);
        fixture.mockSite->receivedEvents.clear();
    }

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
