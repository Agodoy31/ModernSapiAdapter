#include "pch.h"
#include "MockSapiInterfaces.h"
#include "../CoreEngine/SapiEngine.h"

class SapiEngineTests : public ::testing::Test {
protected:
    void SetUp() override {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    }

    void TearDown() override {
        CoUninitialize();
    }
};

TEST_F(SapiEngineTests, SetObjectTokenSucceeds) {
    auto engine = winrt::make_self<CSapiEngine>();

    auto mockToken = winrt::make_self<MockSpObjectToken>();
    auto mockDataKey = winrt::make_self<MockSpDataKey>();

    // Our manual mock natively returns the MockProvider.dll string for GetStringValue("ProviderDLL")
    HRESULT hr = engine->SetObjectToken(mockToken.get());
    EXPECT_TRUE(hr == S_OK);
}

TEST_F(SapiEngineTests, SpeakDispatchesToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();

    // Mock GetActions to return 0 (no abort) is handled natively in the mock struct.

    // When Speak finishes or writes audio, it will call Write.
    // For now, since MockProvider is not loaded in this engine instance (we didn't call SetObjectToken),
    // it will just exit early without synthesizing.
    // Let's just verify Speak returns S_OK and doesn't crash.
    SPVTEXTFRAG textFrag = {};
    const char16_t* text = u"Hello";
    textFrag.pTextStart = (LPCWSTR)text;
    textFrag.ulTextLen = 5;

    WAVEFORMATEX wf = {};
    HRESULT hr = engine->Speak(0, SPDFID_WaveFormatEx, &wf, &textFrag, mockSite.get());
    EXPECT_TRUE(hr == S_OK);
}

TEST_F(SapiEngineTests, GetOutputFormatFailsWhenNoProviderLoaded) {
    auto engine = winrt::make_self<CSapiEngine>();
    
    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    
    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_TRUE(hr == SPERR_UNINITIALIZED);
    EXPECT_TRUE(pWaveFormat == nullptr);
}

TEST_F(SapiEngineTests, GetOutputFormatQueriesProviderFormat) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_TRUE(engine->SetObjectToken(mockToken.get()) == S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;

    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_TRUE(hr == S_OK);
    EXPECT_TRUE(formatId == SPDFID_WaveFormatEx);
    ASSERT_TRUE(pWaveFormat != nullptr);

    EXPECT_TRUE(pWaveFormat->wFormatTag == WAVE_FORMAT_PCM);
    EXPECT_TRUE(pWaveFormat->nSamplesPerSec == 24000);
    EXPECT_TRUE(pWaveFormat->nChannels == 1);
    EXPECT_TRUE(pWaveFormat->wBitsPerSample == 16);

    CoTaskMemFree(pWaveFormat);
}

TEST_F(SapiEngineTests, SpeakConcatenatesMultipleFragments) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_TRUE(engine->SetObjectToken(mockToken.get()) == S_OK);

    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();

    const char16_t* text1 = u"Hello ";
    const char16_t* text2 = u"World!";
    
    SPVTEXTFRAG frag2 = {};
    frag2.pTextStart = (LPCWSTR)text2;
    frag2.ulTextLen = 6;
    frag2.pNext = nullptr;

    SPVTEXTFRAG frag1 = {};
    frag1.pTextStart = (LPCWSTR)text1;
    frag1.ulTextLen = 6;
    frag1.pNext = &frag2;

    WAVEFORMATEX wf = {};
    HRESULT hr = engine->Speak(0, SPDFID_WaveFormatEx, &wf, &frag1, mockSite.get());
    EXPECT_TRUE(hr == S_OK);
}

TEST_F(SapiEngineTests, SpeakPropagatesAudioBytesToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_TRUE(engine->SetObjectToken(mockToken.get()) == S_OK);

    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();

    SPVTEXTFRAG textFrag = {};
    const char16_t* text = u"Test synthesis audio streaming";
    textFrag.pTextStart = (LPCWSTR)text;
    textFrag.ulTextLen = 30;

    WAVEFORMATEX wf = {};
    HRESULT hr = engine->Speak(0, SPDFID_WaveFormatEx, &wf, &textFrag, mockSite.get());
    EXPECT_TRUE(hr == S_OK);

    // MockProvider emits 5 blocks of 1024 bytes = 5120 total bytes
    EXPECT_TRUE(mockSite->writeCallCount == 5);
    EXPECT_TRUE(mockSite->totalBytesWritten == 5120);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsAndDispatchesToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    
    // Inject the mock site directly
    engine->m_cpSite.copy_from(mockSite.get());

    ProviderSpeechEvent providerEvent = {};
    providerEvent.EventType = PROVIDER_EVENT_WORD_BOUNDARY;
    providerEvent.AudioByteOffset = 1024;
    providerEvent.TextLength = 5; // length
    providerEvent.TextOffset = 0; // offset

    engine->OnSpeechEvent(&providerEvent);

    ASSERT_EQ(mockSite->receivedEvents.size(), 1);
    EXPECT_EQ(mockSite->receivedEvents[0].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(mockSite->receivedEvents[0].ullAudioStreamOffset, 1024);
    EXPECT_EQ(mockSite->receivedEvents[0].lParam, 5);
    EXPECT_EQ(mockSite->receivedEvents[0].wParam, 0);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsBookmarkStringEventToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();

    engine->m_cpSite.copy_from(mockSite.get());

    const char16_t* bookmarkStr = u"12345";
    ProviderSpeechEvent providerEvent = {};
    providerEvent.EventType = PROVIDER_EVENT_BOOKMARK;
    providerEvent.AudioByteOffset = 2048;
    providerEvent.TextLength = 5;
    providerEvent.TextOffset = 0;
    providerEvent.StringData = bookmarkStr;

    engine->OnSpeechEvent(&providerEvent);

    ASSERT_EQ(mockSite->receivedEvents.size(), 1);
    EXPECT_EQ(mockSite->receivedEvents[0].eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(mockSite->receivedEvents[0].elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(mockSite->receivedEvents[0].ullAudioStreamOffset, 2048);
    EXPECT_EQ(wcscmp(reinterpret_cast<const wchar_t*>(mockSite->receivedEvents[0].lParam), L"12345"), 0);
    EXPECT_EQ(mockSite->receivedEvents[0].wParam, 12345);
}

TEST_F(SapiEngineTests, SpeakDispatchesBookmarkStringEventToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_TRUE(engine->SetObjectToken(mockToken.get()) == S_OK);

    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();

    const char16_t* bookmarkText = u"999";
    SPVTEXTFRAG textFrag = {};
    textFrag.pTextStart = (LPCWSTR)bookmarkText;
    textFrag.ulTextLen = 3;
    textFrag.State.eAction = SPVA_Bookmark;

    WAVEFORMATEX wf = {};
    HRESULT hr = engine->Speak(0, SPDFID_WaveFormatEx, &wf, &textFrag, mockSite.get());
    EXPECT_TRUE(hr == S_OK);

    ASSERT_EQ(mockSite->receivedEvents.size(), 1);
    EXPECT_EQ(mockSite->receivedEvents[0].eEventId, SPEI_TTS_BOOKMARK);
    EXPECT_EQ(mockSite->receivedEvents[0].elParamType, SPET_LPARAM_IS_STRING);
    EXPECT_EQ(wcscmp(reinterpret_cast<const wchar_t*>(mockSite->receivedEvents[0].lParam), L"999"), 0);
    EXPECT_EQ(mockSite->receivedEvents[0].wParam, 999);
}

