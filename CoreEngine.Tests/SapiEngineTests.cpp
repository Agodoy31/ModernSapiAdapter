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

    bool ReadControl(std::string& text)
    {
        if (m_connectThread.joinable()) m_connectThread.join();
        if (m_connectError != ERROR_SUCCESS) return false;

        char buffer[512] = {};
        DWORD bytesRead = 0;
        if (!ReadFile(m_controlPipe.get(), buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0)
        {
            return false;
        }

        text.assign(buffer, bytesRead);
        return true;
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

class ThreadJoinGuard
{
public:
    explicit ThreadJoinGuard(std::thread& thread) : m_thread(thread) {}

    ~ThreadJoinGuard()
    {
        if (m_thread.joinable())
        {
            m_thread.join();
        }
    }

private:
    std::thread& m_thread;
};

class ScopedFaultEventLog
{
public:
    ScopedFaultEventLog()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);

        std::ofstream stream(m_path, std::ios::binary | std::ios::trunc);
        m_isActive = stream.good();
    }

    ~ScopedFaultEventLog()
    {
        std::error_code error;
        std::filesystem::remove(m_path, error);
    }

    bool IsActive() const noexcept { return m_isActive; }

    size_t SnapshotSize() const
    {
        std::error_code error;
        return std::filesystem::exists(m_path, error) ? static_cast<size_t>(std::filesystem::file_size(m_path, error)) : 0;
    }

    bool WaitForEntryAfter(size_t snapshotSize, const std::string& entry, DWORD timeoutMs) const
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        do
        {
            std::ifstream stream(m_path, std::ios::binary);
            std::string contents((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
            if (contents.size() >= snapshotSize && contents.find(entry, snapshotSize) != std::string::npos)
            {
                return true;
            }
            Sleep(10);
        } while (std::chrono::steady_clock::now() < deadline);

        return false;
    }

private:
    std::filesystem::path m_path{std::filesystem::temp_directory_path() / L"ModernSapiAdapterMockProviderFaultTrace.log"};
    bool m_isActive{false};
};

}

class SapiEngineTests : public ::testing::Test {
protected:
    void SetUp() override {
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
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

    const auto start = std::chrono::steady_clock::now();
    HRESULT hr = engine->SetObjectToken(mockToken.get());
    const auto elapsed = std::chrono::steady_clock::now() - start;

    EXPECT_EQ(hr, S_OK);
    EXPECT_LT(elapsed, std::chrono::seconds(1));

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

TEST_F(SapiEngineTests, PipeClientFailsImmediatelyWhenControlPipeAccessIsDenied) {
    static std::atomic_uint64_t nextPipeId{0};
    const std::wstring pipeName = L"CoreEngineDenied_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
        std::to_wstring(++nextPipeId);
    const std::wstring controlPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + GetCurrentUserSidForTest() + L"\\control";

    PSECURITY_DESCRIPTOR securityDescriptor = nullptr;
    ASSERT_TRUE(ConvertStringSecurityDescriptorToSecurityDescriptorW(
        L"D:(D;;GA;;;WD)", SDDL_REVISION_1, &securityDescriptor, nullptr));
    wil::unique_hlocal securityDescriptorHandle(securityDescriptor);

    SECURITY_ATTRIBUTES securityAttributes = {};
    securityAttributes.nLength = sizeof(securityAttributes);
    securityAttributes.lpSecurityDescriptor = securityDescriptor;

    wil::unique_handle deniedControlPipe(CreateNamedPipeW(
        controlPipePath.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        4096,
        4096,
        0,
        &securityAttributes));
    ASSERT_TRUE(deniedControlPipe);

    PipeClient client;
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(client.Connect(pipeName, L"definitely-not-a-provider.exe"), HRESULT_FROM_WIN32(ERROR_ACCESS_DENIED));
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::milliseconds(100));
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

TEST_F(SapiEngineTests, CreateProviderSessionDoesNotPublishAnInvalidInfoResponse) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    auto engine = winrt::make_self<CSapiEngine>();
    engine->m_providerExecutablePath = L"ignored.exe";
    engine->m_providerPipeName = server.PipeName();

    std::atomic_bool infoResponseSent{false};
    std::thread infoResponder([&server, &infoResponseSent] {
        std::string request;
        infoResponseSent = server.ReadControl(request) &&
            request == "{\"command\":\"info\"}\n" &&
            server.WriteControl("{\"response\":\"not_info\",\"audio_format\":{\"sample_rate\":24000,\"bits_per_sample\":16,\"channels\":1}}\n");
    });

    EXPECT_EQ(engine->CreateProviderSessionLocked(), E_FAIL);
    infoResponder.join();
    EXPECT_TRUE(infoResponseSent.load());
    EXPECT_EQ(engine->m_pClient, nullptr);
    EXPECT_EQ(engine->m_pWorker, nullptr);
}

TEST_F(SapiEngineTests, InvalidCancellationBoundaryFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client);
    ASSERT_TRUE(worker.Start(nullptr, 7));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] {
        cancellationResult = worker.CancelAndDrain();
    });
    ThreadJoinGuard cancellationJoin(cancellationThread);

    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_NE(cancellationRequest.find("\"command\":\"cancel\""), std::string::npos);
    ASSERT_TRUE(server.WriteControl("{\"event\":\"synthesis_cancelled\",\"speak_id\":7}\n"));

    cancellationThread.join();
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, RejectedAudioWriteDrainsCancellationBeforeNextSpeak) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    wchar_t firstText[] = L"[delay-cancelled-event] rejected write remains pending";
    SPVTEXTFRAG firstFragment = {};
    firstFragment.pTextStart = firstText;
    firstFragment.ulTextLen = static_cast<ULONG>(wcslen(firstText));

    mockSite->rejectNextWrite = true;
    HRESULT firstSpeakResult = E_FAIL;
    std::atomic_bool firstSpeakReturned = false;
    std::thread firstSpeakThread([&] {
        firstSpeakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    for (int attempt = 0; attempt < 50 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    ASSERT_EQ(mockSite->writeCallCount.load(), 1u);

    Sleep(10);
    EXPECT_FALSE(firstSpeakReturned.load());

    firstSpeakThread.join();
    EXPECT_EQ(firstSpeakResult, S_OK);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), 0u);

    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &secondFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    EXPECT_EQ(mockSite->totalBytesWritten.load(), 9600u);
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, FaultedSessionDoesNotForwardAnEventPausedBeforeItsSapiCallback) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    SpeechWorker* worker = engine->m_pWorker.get();
    ASSERT_NE(worker, nullptr);
    worker->PauseNextEventForwardForTest();

    wchar_t text[] = L"event is paused before its SAPI callback";
    SPVTEXTFRAG fragment = {};
    fragment.pTextStart = text;
    fragment.ulTextLen = static_cast<ULONG>(wcslen(text));

    mockSite->rejectNextWrite = true;
    engine->FailNextCancellationControlSendForTest();
    HRESULT speakResult = S_OK;
    std::thread speakThread([&] {
        speakResult = engine->Speak(0, formatId, pWaveFormat, &fragment, mockSite.get());
    });
    ThreadJoinGuard speakJoin(speakThread);

    const bool pausedBeforeSapiCallback = worker->WaitForEventForwardPauseForTest(1000);
    std::unique_lock<std::mutex> sessionLock(engine->m_sessionMutex);
    for (int attempt = 0; attempt < 100 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    const ULONG writeCallCount = mockSite->writeCallCount.load();
    const bool faulted = worker->WaitForFaultForTest(1000);

    worker->ReleaseEventForwardForTest();
    sessionLock.unlock();
    speakThread.join();
    CoTaskMemFree(pWaveFormat);

    ASSERT_TRUE(pausedBeforeSapiCallback);
    ASSERT_EQ(writeCallCount, 1u);
    ASSERT_TRUE(faulted);
    EXPECT_EQ(speakResult, E_FAIL);
    std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
    EXPECT_TRUE(mockSite->receivedEvents.empty())
        << "An event authorized before fault reached SAPI after fault became visible.";
}

TEST_F(SapiEngineTests, RejectedAudioWriteWithFailedCancellationQuarantinesWorker) {
    ScopedFaultEventLog faultEventLog;
    ASSERT_TRUE(faultEventLog.IsActive());

    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();

    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
    ASSERT_EQ(engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat), S_OK);
    ASSERT_NE(pWaveFormat, nullptr);

    SpeechWorker* faultedWorker = engine->m_pWorker.get();
    PipeClient* faultedClient = engine->m_pClient.get();
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

    mockSite->rejectNextWrite = true;
    engine->FailNextCancellationControlSendForTest();
    HRESULT firstSpeakResult = S_OK;
    std::atomic_bool firstSpeakReturned = false;
    const auto firstSpeakStart = std::chrono::steady_clock::now();
    std::thread firstSpeakThread([&] {
        firstSpeakResult = engine->Speak(0, formatId, pWaveFormat, &firstFragment, mockSite.get());
        firstSpeakReturned = true;
    });
    ThreadJoinGuard firstSpeakJoin(firstSpeakThread);

    for (int attempt = 0; attempt < 50 && mockSite->writeCallCount.load() == 0; ++attempt)
    {
        Sleep(10);
    }
    ASSERT_EQ(mockSite->writeCallCount.load(), 1u);

    for (int attempt = 0; attempt < 100 && !firstSpeakReturned.load(); ++attempt)
    {
        Sleep(10);
    }
    ASSERT_TRUE(firstSpeakReturned.load());

    firstSpeakThread.join();
    EXPECT_EQ(firstSpeakResult, E_FAIL);
    EXPECT_LT(std::chrono::steady_clock::now() - firstSpeakStart, std::chrono::seconds(1));

    // The failed utterance publishes Faulted but retains its discard-only worker/client until a later Speak owns teardown.
    EXPECT_EQ(engine->m_pWorker.get(), faultedWorker);
    EXPECT_EQ(engine->m_pClient.get(), faultedClient);
    EXPECT_TRUE(faultedWorker->IsFaulted());

    // Discard events accepted before the rejected write. The trace offset makes the subsequent observations
    // prove post-fault provider activity rather than merely output that arrived before fault publication.
    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        mockSite->receivedEvents.clear();
    }
    const size_t postFaultTraceOffset = faultEventLog.SnapshotSize();

    ASSERT_TRUE(faultEventLog.WaitForEntryAfter(postFaultTraceOffset, "1:word_boundary", 1000))
        << "The faulted provider did not emit a late control event.";
    ASSERT_TRUE(faultEventLog.WaitForEntryAfter(postFaultTraceOffset, "1:pcm", 1000))
        << "The faulted provider did not emit late PCM.";

    // Faulted-session PCM and events must be consumed or discarded without crossing the SAPI boundary.
    EXPECT_EQ(mockSite->BytesAcceptedAfterRejectedWrite(), 0u);
    {
        std::lock_guard<std::mutex> lock(mockSite->eventsMutex);
        EXPECT_TRUE(mockSite->receivedEvents.empty())
            << "Faulted provider output reached the SAPI event sink.";
    }

    const ULONG writesBeforeNextSpeak = mockSite->writeCallCount.load();
    const ULONG bytesBeforeNextSpeak = mockSite->totalBytesWritten.load();
    wchar_t secondText[] = L"fresh";
    SPVTEXTFRAG secondFragment = {};
    secondFragment.pTextStart = secondText;
    secondFragment.ulTextLen = static_cast<ULONG>(wcslen(secondText));

    EXPECT_EQ(engine->Speak(0, formatId, pWaveFormat, &secondFragment, mockSite.get()), S_OK);
    CoTaskMemFree(pWaveFormat);

    ASSERT_NE(engine->m_pWorker, nullptr);
    ASSERT_NE(engine->m_pClient, nullptr);
    EXPECT_FALSE(engine->m_pWorker->IsFaulted());
    EXPECT_GT(mockSite->writeCallCount.load(), writesBeforeNextSpeak);
    EXPECT_EQ(mockSite->totalBytesWritten.load(), bytesBeforeNextSpeak + 9600u);
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
