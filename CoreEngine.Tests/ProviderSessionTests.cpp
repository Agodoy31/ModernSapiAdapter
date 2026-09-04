#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpObjectToken.h"
#include "MockSpTTSEngineSite.h"
#include "ControlPipeTestServer.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/PipeClient.h"

using namespace TestInfrastructure;

TEST_F(SapiEngineTests, SetObjectTokenConnectsToPipeAndQueriesInfo)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    const auto start = std::chrono::steady_clock::now();
    HRESULT hr = engine->SetObjectToken(mockToken.get());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(hr, S_OK);
    EXPECT_LT(elapsed, std::chrono::seconds(1));

    GUID formatId = {};
    WAVEFORMATEX *pWaveFormat = nullptr;
    hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_EQ(hr, S_OK);
    EXPECT_EQ(formatId, SPDFID_WaveFormatEx);
    EXPECT_NE(pWaveFormat, nullptr);

    EXPECT_EQ(pWaveFormat->wFormatTag, WAVE_FORMAT_PCM);
    EXPECT_EQ(pWaveFormat->nSamplesPerSec, 24000u);
    EXPECT_EQ(pWaveFormat->nChannels, 1);
    EXPECT_EQ(pWaveFormat->wBitsPerSample, 16);

    CoTaskMemFree(pWaveFormat);
}

TEST_F(SapiEngineTests, GetObjectTokenDoesNotWaitForActiveSpeakSerialization)
{
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    engine->m_cpToken.copy_from(mockToken.get());

    std::unique_lock<std::mutex> tokenLock(engine->m_speakMutex);
    std::atomic_bool getterStarted{false};
    std::atomic_bool getterCompleted{false};
    HRESULT getResult = E_FAIL;
    ISpObjectToken *returnedToken = nullptr;
    std::thread getter(
        [&]
        {
            getterStarted.store(true, std::memory_order_release);
            getResult = engine->GetObjectToken(&returnedToken);
            getterCompleted.store(true, std::memory_order_release);
        });
    ThreadJoinGuard getterJoin(getter);

    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return getterStarted.load(std::memory_order_acquire);
        },
        1000, 1));
    EXPECT_TRUE(WaitForCondition(
        [&]
        {
            return getterCompleted.load(std::memory_order_acquire);
        },
        1000, 1));

    tokenLock.unlock();
    ASSERT_TRUE(getterJoin.Join());
    EXPECT_EQ(getResult, S_OK);
    EXPECT_EQ(returnedToken, mockToken.get());
    if (returnedToken)
    {
        returnedToken->Release();
    }
}

TEST_F(SapiEngineTests, GetOutputFormatFailsWhenNoProviderLoaded)
{
    auto engine = winrt::make_self<CSapiEngine>();

    GUID formatId = GUID_NULL;
    WAVEFORMATEX *pWaveFormat = reinterpret_cast<WAVEFORMATEX *>(static_cast<uintptr_t>(0xDEADBEEF));

    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_EQ(hr, SPERR_UNINITIALIZED);
    EXPECT_EQ(pWaveFormat, nullptr);
}

TEST_F(SapiEngineTests, CreateProviderSessionDoesNotPublishAnInvalidInfoResponse)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    auto engine = winrt::make_self<CSapiEngine>();
    engine->m_config.executablePath = L"ignored.exe";
    engine->m_config.pipeName = server.PipeName();

    std::atomic_bool infoResponseSent{false};
    std::thread infoResponder(
        [&server, &infoResponseSent]
        {
            std::string request;
            infoResponseSent = server.ReadControl(request) && request == "{\"command\":\"info\"}\n" &&
                               server.WriteControl("{\"response\":\"not_info\",\"audio_format\":{\"sample_rate\":24000,"
                                                   "\"bits_per_sample\":16,\"channels\":1}}\n");
        });

    EXPECT_EQ(engine->CreateProviderSessionLocked(), E_FAIL);
    infoResponder.join();
    EXPECT_TRUE(infoResponseSent.load());
    EXPECT_EQ(engine->m_pClient, nullptr);
    EXPECT_EQ(engine->m_pWorker, nullptr);
}

TEST_F(SapiEngineTests, CreateProviderSessionAcceptsIntegralFloatAudioFormatNumbers)
{
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    auto engine = winrt::make_self<CSapiEngine>();
    engine->m_config.executablePath = L"ignored.exe";
    engine->m_config.pipeName = server.PipeName();

    std::atomic_bool infoResponseSent{false};
    std::thread infoResponder(
        [&server, &infoResponseSent]
        {
            std::string request;
            infoResponseSent = server.ReadControl(request) && request == "{\"command\":\"info\"}\n" &&
                               server.WriteControl("{\"response\":\"info\",\"audio_format\":{\"sample_rate\":24000.0,"
                                                   "\"bits_per_sample\":16.0,\"channels\":1.0}}\n");
        });

    const HRESULT sessionResult = engine->CreateProviderSessionLocked();
    infoResponder.join();
    ASSERT_EQ(sessionResult, S_OK);
    EXPECT_TRUE(infoResponseSent.load());
    EXPECT_EQ(engine->m_config.audioFormat.nSamplesPerSec, 24000u);
    EXPECT_EQ(engine->m_config.audioFormat.wBitsPerSample, 16u);
    EXPECT_EQ(engine->m_config.audioFormat.nChannels, 1u);
}

TEST_F(SapiEngineTests, SpeakRebuildsProviderSessionAfterSynthesisTimeout)
{
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX *pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    wchar_t stalledText[] = L"[stall-synthesis] provider never reports progress";
    SPVTEXTFRAG stalledFragment = {};
    stalledFragment.pTextStart = stalledText;
    stalledFragment.ulTextLen = static_cast<ULONG>(wcslen(stalledText));

    const auto stalledStart = std::chrono::steady_clock::now();
    const HRESULT stalledResult = engine->Speak(0, formatId, pWaveFormat, &stalledFragment, mockSite.get());
    const auto stalledElapsed = std::chrono::steady_clock::now() - stalledStart;

    EXPECT_EQ(stalledResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_GE(stalledElapsed, std::chrono::milliseconds(1300));
    EXPECT_LT(stalledElapsed, std::chrono::milliseconds(2300));
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 0u);

    wchar_t freshText[] = L"fresh";
    SPVTEXTFRAG freshFragment = {};
    freshFragment.pTextStart = freshText;
    freshFragment.ulTextLen = static_cast<ULONG>(wcslen(freshText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &freshFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    EXPECT_EQ(mockSite->totalBytesWritten.load(), 9600u);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, FailedSpeakDispatchQuarantinesSessionAndNextSpeakRecovers)
{
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX *pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    wchar_t rejectedText[] = L"rejected dispatch";
    SPVTEXTFRAG rejectedFragment = {};
    rejectedFragment.pTextStart = rejectedText;
    rejectedFragment.ulTextLen = static_cast<ULONG>(wcslen(rejectedText));

    engine->FailNextSpeakControlSendForTest();
    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &rejectedFragment, mockSite.get()), E_FAIL);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 0u);

    wchar_t freshText[] = L"fresh";
    SPVTEXTFRAG freshFragment = {};
    freshFragment.pTextStart = freshText;
    freshFragment.ulTextLen = static_cast<ULONG>(wcslen(freshText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &freshFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    EXPECT_EQ(mockSite->totalBytesWritten.load(), 9600u);
}
#endif

TEST_F(SapiEngineTests, IdleProviderRemainsUsableBeyondTheActiveRequestDeadline)
{
    PipeServerWorkerFixture fixture;
    ASSERT_TRUE(fixture.Initialize());

    Sleep(1700);

    ASSERT_TRUE(fixture.Start(33));
    ASSERT_TRUE(fixture.server.WriteControl("{\"event\":\"synthesis_complete\",\"speak_id\":33,\"total_audio_bytes\":0}\n"));
    EXPECT_EQ(fixture.worker->WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(fixture.worker->IsFaulted());
}
