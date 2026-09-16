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
#endif
