#include "pch.h"
#include "MockSapiInterfaces.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include <sddl.h>

namespace
{
std::wstring GetCurrentUserSidForTest()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return L"DefaultUser";

    wil::unique_handle tokenHandle(token);
    DWORD bytesRequired = 0;
    GetTokenInformation(tokenHandle.get(), TokenUser, nullptr, 0, &bytesRequired);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER) return L"DefaultUser";

    std::vector<BYTE> tokenBuffer(bytesRequired);
    if (!GetTokenInformation(tokenHandle.get(), TokenUser, tokenBuffer.data(), bytesRequired, &bytesRequired)) return L"DefaultUser";

    auto tokenUser = reinterpret_cast<TOKEN_USER*>(tokenBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText)) return L"DefaultUser";

    wil::unique_hlocal_string sid(sidText);
    return sid.get();
}

class ControlPipeTestServer
{
public:
    ControlPipeTestServer()
    {
        static std::atomic_uint64_t nextPipeId{0};
        m_pipeName = L"CoreEngineTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++nextPipeId);

        const std::wstring basePath = L"\\\\.\\pipe\\" + m_pipeName + L"\\" + GetCurrentUserSidForTest();
        m_controlPipe.reset(CreateNamedPipeW(
            (basePath + L"\\control").c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));
        m_audioPipe.reset(CreateNamedPipeW(
            (basePath + L"\\audio").c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));

        if (!m_controlPipe || !m_audioPipe)
        {
            m_createError = GetLastError();
            return;
        }

        m_connectThread = std::thread([this] {
            ConnectPipe(m_controlPipe.get());
            ConnectPipe(m_audioPipe.get());
        });
    }

    ~ControlPipeTestServer()
    {
        if (m_controlPipe) CancelIoEx(m_controlPipe.get(), nullptr);
        if (m_audioPipe) CancelIoEx(m_audioPipe.get(), nullptr);
        m_controlPipe.reset();
        m_audioPipe.reset();
        if (m_connectThread.joinable()) m_connectThread.join();
    }

    const std::wstring& PipeName() const { return m_pipeName; }
    DWORD CreateError() const { return m_createError; }

    bool WriteControl(const std::string& text)
    {
        if (m_connectThread.joinable()) m_connectThread.join();
        if (m_connectError != ERROR_SUCCESS) return false;

        DWORD bytesWritten = 0;
        return WriteFile(m_controlPipe.get(), text.data(), static_cast<DWORD>(text.size()), &bytesWritten, nullptr) && bytesWritten == text.size();
    }

private:
    void ConnectPipe(HANDLE pipe)
    {
        if (m_connectError != ERROR_SUCCESS) return;

        if (!ConnectNamedPipe(pipe, nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED) m_connectError = error;
        }
    }

    std::wstring m_pipeName;
    wil::unique_handle m_controlPipe;
    wil::unique_handle m_audioPipe;
    std::thread m_connectThread;
    std::atomic<DWORD> m_createError{ERROR_SUCCESS};
    std::atomic<DWORD> m_connectError{ERROR_SUCCESS};
};
}

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

TEST_F(SapiEngineTests, ReadControlMessageRetainsSecondJsonLineFromOnePipeRead) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("{\"event\":\"first\"}\n{\"event\":\"second\"}\n"));

    winrt::Windows::Data::Json::JsonObject first = nullptr;
    winrt::Windows::Data::Json::JsonObject second = nullptr;
    ASSERT_EQ(client.ReadControlMessage(first), S_OK);
    ASSERT_EQ(client.ReadControlMessage(second), S_OK);

    EXPECT_EQ(first.GetNamedString(L"event"), L"first");
    EXPECT_EQ(second.GetNamedString(L"event"), L"second");
}

TEST_F(SapiEngineTests, ReadControlMessageReassemblesFragmentedJsonLine) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::atomic_bool writesSucceeded{true};
    std::thread writer([&server, &writesSucceeded] {
        Sleep(20);
        writesSucceeded = server.WriteControl("{\"event\":\"");
        Sleep(20);
        writesSucceeded = server.WriteControl("fragmented\"}\n") && writesSucceeded.load();
    });

    winrt::Windows::Data::Json::JsonObject message = nullptr;
    EXPECT_EQ(client.ReadControlMessage(message), S_OK);
    writer.join();

    ASSERT_TRUE(writesSucceeded);
    EXPECT_EQ(message.GetNamedString(L"event"), L"fragmented");
}

TEST_F(SapiEngineTests, GetOutputFormatFailsWhenNoProviderLoaded) {
    auto engine = winrt::make_self<CSapiEngine>();
    
    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    
    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_TRUE(hr == SPERR_UNINITIALIZED);
    EXPECT_TRUE(pWaveFormat == nullptr);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsAndDispatchesToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    using namespace winrt::Windows::Data::Json;
    JsonObject eventJson;
    eventJson.SetNamedValue(L"event", JsonValue::CreateStringValue(L"word_boundary"));
    eventJson.SetNamedValue(L"audio_offset_ms", JsonValue::CreateNumberValue(50));
    eventJson.SetNamedValue(L"text_offset", JsonValue::CreateNumberValue(17));
    eventJson.SetNamedValue(L"text_length", JsonValue::CreateNumberValue(5));

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT& received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
    EXPECT_EQ(received.ullAudioStreamOffset, 2400u);
    EXPECT_EQ(received.wParam, 17u);
    EXPECT_EQ(received.lParam, 5);
}

TEST_F(SapiEngineTests, OnSpeechEventPreservesLongAudioOffsets) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    using namespace winrt::Windows::Data::Json;
    JsonObject eventJson;
    eventJson.SetNamedValue(L"event", JsonValue::CreateStringValue(L"word_boundary"));
    eventJson.SetNamedValue(L"audio_offset_ms", JsonValue::CreateNumberValue(90000));
    eventJson.SetNamedValue(L"text_offset", JsonValue::CreateNumberValue(0));
    eventJson.SetNamedValue(L"text_length", JsonValue::CreateNumberValue(4));

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

    using namespace winrt::Windows::Data::Json;
    JsonObject eventJson;
    eventJson.SetNamedValue(L"event", JsonValue::CreateStringValue(L"bookmark_reached"));
    eventJson.SetNamedValue(L"audio_offset_ms", JsonValue::CreateNumberValue(9));
    eventJson.SetNamedValue(L"bookmark_name", JsonValue::CreateStringValue(L"1"));

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

    using namespace winrt::Windows::Data::Json;
    JsonObject eventJson;
    eventJson.SetNamedValue(L"event", JsonValue::CreateStringValue(L"sentence_boundary"));
    eventJson.SetNamedValue(L"audio_offset_ms", JsonValue::CreateNumberValue(75));
    eventJson.SetNamedValue(L"text_offset", JsonValue::CreateNumberValue(22));
    eventJson.SetNamedValue(L"text_length", JsonValue::CreateNumberValue(9));

    engine->OnSpeechEvent(eventJson);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    ASSERT_EQ(mockSite->receivedEvents.size(), 1u);
    const SPEVENT& received = mockSite->receivedEvents.front();
    EXPECT_EQ(received.eEventId, SPEI_SENTENCE_BOUNDARY);
    EXPECT_EQ(received.elParamType, SPET_LPARAM_IS_UNDEFINED);
    EXPECT_EQ(received.ullAudioStreamOffset, 3600u);
    EXPECT_EQ(received.wParam, 22u);
    EXPECT_EQ(received.lParam, 9);
}

TEST_F(SapiEngineTests, OnSpeechEventMapsBookmarkStringEventToSite) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    engine->m_audioFormat = { WAVE_FORMAT_PCM, 1, 24000, 48000, 2, 16, 0 };

    using namespace winrt::Windows::Data::Json;
    JsonObject eventJson;
    eventJson.SetNamedValue(L"event", JsonValue::CreateStringValue(L"bookmark_reached"));
    eventJson.SetNamedValue(L"audio_offset_ms", JsonValue::CreateNumberValue(100));
    eventJson.SetNamedValue(L"bookmark_name", JsonValue::CreateStringValue(L"42"));

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
    EXPECT_EQ(mockSite->receivedEvents[0].wParam, 0u);
    EXPECT_EQ(mockSite->receivedEvents[1].eEventId, SPEI_WORD_BOUNDARY);
    EXPECT_EQ(mockSite->receivedEvents[1].wParam, 23u);
}

TEST_F(SapiEngineTests, OutputSiteAbortCancelsTheActiveRequest) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    std::wstring firstText;
    for (int word = 0; word < 80; ++word)
    {
        firstText += L"pending ";
    }
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText.c_str();
    firstFragment.ulTextLen = static_cast<ULONG>(firstText.length());

    HRESULT speakResult = E_FAIL;
    std::atomic<bool> returned = false;
    std::thread speakThread([&] {
        speakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        returned = true;
    });

    for (int attempt = 0; attempt < 50 && mockSite->totalBytesWritten.load() == 0; ++attempt)
    {
        Sleep(20);
    }
    ASSERT_EQ(mockSite->totalBytesWritten.load(), 9600u);

    mockSite->actions = SPVES_ABORT;
    for (int attempt = 0; attempt < 25 && !returned.load(); ++attempt)
    {
        Sleep(20);
    }

    EXPECT_TRUE(returned.load());
    speakThread.join();
    CoTaskMemFree(pWaveFormat);
    EXPECT_EQ(speakResult, S_OK);

    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    const bool forwardedLateSentenceBoundary = std::any_of(
        mockSite->receivedEvents.begin(),
        mockSite->receivedEvents.end(),
        [](const SPEVENT& event) { return event.eEventId == SPEI_SENTENCE_BOUNDARY; });
    EXPECT_FALSE(forwardedLateSentenceBoundary);
}
