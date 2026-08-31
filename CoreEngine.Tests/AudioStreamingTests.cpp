#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpTTSEngineSite.h"
#include "MockSpObjectToken.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/SpeechWorker.h"
#include "../CoreEngine/PcmFrameAssembler.h"

using namespace TestInfrastructure;

TEST(PcmFrameAssemblerTests, OneByteFragmentsDoNotProducePartial16BitMonoFrames) {
    PcmFrameAssembler assembler(2);
    std::vector<size_t> emittedSizes;

    const auto output = FeedPcmFragments(assembler,
        { { 0x10 }, { 0x11 }, { 0x20 }, { 0x21 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 2, 2 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{ 0x10, 0x11, 0x20, 0x21 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, Awkward24BitStereoBoundariesPreserveEveryProviderByte) {
    PcmFrameAssembler assembler(6);
    std::vector<size_t> emittedSizes;

    const auto output = FeedPcmFragments(assembler,
        { { 0x01 }, { 0x02, 0x03, 0x04, 0x05 }, { 0x06, 0x11, 0x12 },
          { 0x13, 0x14, 0x15, 0x16, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 6, 6, 6 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, ResetPreventsPartialFrameBytesCrossingRequestBoundaries) {
    PcmFrameAssembler assembler(6);
    EXPECT_TRUE(assembler.Process(std::vector<uint8_t>{ 0xA1, 0xA2 }.data(), 2).empty());

    assembler.Reset();
    std::vector<size_t> emittedSizes;
    const auto output = FeedPcmFragments(assembler,
        { { 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 } }, emittedSizes);

    EXPECT_EQ(emittedSizes, (std::vector<size_t>{ 6 }));
    EXPECT_EQ(output, (std::vector<uint8_t>{ 0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6 }));
    EXPECT_FALSE(assembler.HasCarry());
}

TEST(PcmFrameAssemblerTests, AlignedInputWithoutCarryUsesTheOriginalBuffer) {
    PcmFrameAssembler assembler(6);
    const std::vector<uint8_t> input{ 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                      0x11, 0x12, 0x13, 0x14, 0x15, 0x16 };

    const auto spans = assembler.Process(input.data(), input.size());

    ASSERT_EQ(spans.size(), 1u);
    EXPECT_EQ(spans[0].data, input.data());
    EXPECT_EQ(spans[0].size, 12u);
}

TEST_F(SapiEngineTests, SpeakWaitsForSynthesisCompleteByteBoundary) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    HRESULT hrFormat = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    ASSERT_EQ(hrFormat, S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    SPVTEXTFRAG frag = {};
    wchar_t text[] = L"Hello world from mock provider";
    frag.pTextStart = text;
    frag.ulTextLen = static_cast<ULONG>(wcslen(text));

    HRESULT hr = engine->Speak(0, formatId, pWaveFormat, &frag, mockSite.get());
    CoTaskMemFree(pWaveFormat);

    EXPECT_EQ(hr, S_OK);
    EXPECT_GT(mockSite->writeCallCount.load(), 0u);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 48000u);
    {
        std::lock_guard<std::mutex> lock(mockSite->writesMutex);
        ASSERT_FALSE(mockSite->requestedWriteSizes.empty());
        EXPECT_TRUE(std::all_of(mockSite->requestedWriteSizes.begin(), mockSite->requestedWriteSizes.end(),
            [](ULONG requestedSize) { return requestedSize % 2 == 0; }));
    }

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_GT(mockSite->receivedEvents.size(), 0u);

    bool foundWordBoundary = false;
    for (const auto& evt : mockSite->receivedEvents) {
        if (evt.eEventId == SPEI_WORD_BOUNDARY) {
            foundWordBoundary = true;
            break;
        }
    }
    EXPECT_TRUE(foundWordBoundary);
}

TEST_F(SapiEngineTests, SpeakPreservesNonContiguousSapiSourceOffsets) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

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

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 2u);
    EXPECT_EQ(mockSite->receivedEvents[0].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(mockSite->receivedEvents[0].wParam, 5u);
    EXPECT_EQ(mockSite->receivedEvents[0].lParam, 0);
    EXPECT_EQ(mockSite->receivedEvents[1].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(mockSite->receivedEvents[1].wParam, 6u);
    EXPECT_EQ(mockSite->receivedEvents[1].lParam, 23);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(26));
    worker.PauseNextEventForwardForTest();
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":26,\"total_audio_bytes\":2}\n"));
    ASSERT_TRUE(worker.WaitForEventForwardPauseForTest(1000));

    ASSERT_TRUE(server.WriteAudio({ 0xC1, 0xC2, 0xD1, 0xD2 }));
    worker.ReleaseEventForwardForTest();

    ASSERT_TRUE(worker.WaitForFaultForTest(1000));
    std::lock_guard<std::mutex> lock(mockSite->writesMutex);
    EXPECT_EQ(mockSite->requestedWriteSizes, (std::vector<ULONG>{ 2 }));
    EXPECT_EQ(mockSite->acceptedAudio, (std::vector<uint8_t>{ 0xC1, 0xC2 }));
}

TEST_F(SapiEngineTests, WorkerReassemblesAwkward24BitStereoPipeFragments) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 6);
    ASSERT_TRUE(worker.Start(27));

    ASSERT_TRUE(server.WriteAudio({ 0x01 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 1; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 1u);
    ASSERT_TRUE(server.WriteAudio({ 0x02, 0x03, 0x04, 0x05 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 5; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 5u);
    ASSERT_TRUE(server.WriteAudio({ 0x06, 0x11, 0x12 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 8; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 8u);
    ASSERT_TRUE(server.WriteAudio({ 0x13, 0x14, 0x15, 0x16, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 18; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 18u);
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":27,\"total_audio_bytes\":18}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(mockSite->writesMutex);
    ASSERT_FALSE(mockSite->requestedWriteSizes.empty());
    EXPECT_TRUE(std::all_of(mockSite->requestedWriteSizes.begin(), mockSite->requestedWriteSizes.end(),
        [](ULONG requestedSize) { return requestedSize % 6 == 0; }));
    EXPECT_EQ(mockSite->acceptedAudio, (std::vector<uint8_t>{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
        0x11, 0x12, 0x13, 0x14, 0x15, 0x16,
        0x21, 0x22, 0x23, 0x24, 0x25, 0x26 }));
}

TEST_F(SapiEngineTests, SynthesisCompleteWaitsForFinalSapiWriteToFinish) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());

    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(42));

    mockSite->PauseNextWrite();
    ASSERT_TRUE(server.WriteAudio({ 0x10, 0x20, 0x30, 0x40 }));
    auto releaseWrite = wil::scope_exit([&] { mockSite->ReleaseWrite(); });
    ASSERT_TRUE(mockSite->WaitForWritePause(1000));
    worker.PauseNextEventForwardForTest();
    auto releaseEventForward = wil::scope_exit([&] { worker.ReleaseEventForwardForTest(); });
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":42,\"total_audio_bytes\":4}\n"));
    ASSERT_TRUE(worker.WaitForEventForwardPauseForTest(1000));

    std::mutex completionMutex;
    std::condition_variable completionCondition;
    bool waitReturned = false;
    HRESULT waitResult = E_UNEXPECTED;
    std::thread waitThread([&] {
        waitResult = worker.WaitUntilFinished(nullptr);
        {
            std::lock_guard<std::mutex> lock(completionMutex);
            waitReturned = true;
        }
        completionCondition.notify_all();
    });
    auto releaseWriteBeforeJoin = wil::scope_exit([&] {
        mockSite->ReleaseWrite();
        if (waitThread.joinable()) {
            waitThread.join();
        }
    });

    {
        std::unique_lock<std::mutex> lock(completionMutex);
        EXPECT_FALSE(completionCondition.wait_for(lock, std::chrono::milliseconds(100), [&] {
            return waitReturned;
        }));
    }
    EXPECT_FALSE(worker.IsFaulted());

    worker.ReleaseEventForwardForTest();
    mockSite->ReleaseWrite();
    waitThread.join();

    EXPECT_EQ(waitResult, S_OK);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 4u);
    EXPECT_FALSE(worker.IsFaulted());
}
#endif

TEST_F(SapiEngineTests, SpeakDoesNotHoldTheSessionLockAcrossReentrantGetActions) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    std::atomic_bool reentrantGetActionsRan{false};
    std::atomic_bool reentrantGetOutputCompleted{false};
    std::atomic_bool reentrantGetOutputCompletedBeforeReturn{false};
    HRESULT reentrantGetOutputResult = E_FAIL;
    std::thread reentrantGetOutputThread;
    mockSite->getActionsCallback = [&] {
        bool expected = false;
        if (reentrantGetActionsRan.compare_exchange_strong(expected, true))
        {
            reentrantGetOutputThread = std::thread([&] {
                GUID nestedFormatId = {};
                WAVEFORMATEX* nestedFormat = nullptr;
                reentrantGetOutputResult = engine->GetOutputFormat(nullptr, nullptr, &nestedFormatId, &nestedFormat);
                CoTaskMemFree(nestedFormat);
                reentrantGetOutputCompleted = true;
            });

            for (int attempt = 0; attempt < 40 && !reentrantGetOutputCompleted.load(); ++attempt)
            {
                Sleep(5);
            }
            reentrantGetOutputCompletedBeforeReturn = reentrantGetOutputCompleted.load();
        }
        return SPVES_CONTINUE;
    };

    wchar_t text[] = L"reentrant get actions";
    SPVTEXTFRAG fragment = {};
    fragment.pTextStart = text;
    fragment.ulTextLen = static_cast<ULONG>(wcslen(text));

    const auto speakStart = std::chrono::steady_clock::now();
    const HRESULT speakResult = engine->Speak(0, formatId, pWaveFormat, &fragment, mockSite.get());
    CoTaskMemFree(pWaveFormat);
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
