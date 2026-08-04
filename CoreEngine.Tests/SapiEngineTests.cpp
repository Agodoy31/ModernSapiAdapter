#include "pch.h"
#include "MockSapiInterfaces.h"
#include "../CoreEngine/SapiEngine.h"

class SapiEngineTests : public ::testing::Test {
protected:
    void SetUp() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
    }

    void TearDown() override {
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
};

TEST_F(SapiEngineTests, GetOutputFormatFailsWhenNoProviderLoaded) {
    auto engine = winrt::make_self<CSapiEngine>();
    
    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    
    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_TRUE(hr == SPERR_UNINITIALIZED);
    EXPECT_TRUE(pWaveFormat == nullptr);
}

TEST_F(SapiEngineTests, SetObjectTokenConnectsToPipeAndQueriesInfo) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    HRESULT hr = engine->SetObjectToken(mockToken.get());
    EXPECT_EQ(hr, S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
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

TEST_F(SapiEngineTests, SpeakStreamsAudioAndEventsFromMockProvider) {
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

    // Give worker time to complete streaming
    std::this_thread::sleep_for(std::chrono::milliseconds(300));

    EXPECT_GT(mockSite->writeCallCount.load(), 0u);

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
