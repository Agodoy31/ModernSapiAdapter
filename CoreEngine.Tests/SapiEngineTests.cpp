#include "pch.h"
#include "MockSapiInterfaces.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"
#include "../CoreEngine/PcmFrameAssembler.h"
#include <sddl.h>

namespace
{
std::vector<uint8_t> FeedPcmFragments(PcmFrameAssembler& assembler,
    const std::vector<std::vector<uint8_t>>& fragments,
    std::vector<size_t>& emittedSizes)
{
    std::vector<uint8_t> output;
    for (const auto& fragment : fragments)
    {
        const auto spans = assembler.Process(fragment.data(), fragment.size());
        for (const auto& span : spans)
        {
            emittedSizes.push_back(span.size);
            output.insert(output.end(), span.data, span.data + span.size);
        }
    }
    return output;
}

class CoreEngineDll
{
public:
    using DllGetClassObjectFunction = HRESULT(STDAPICALLTYPE*)(REFCLSID, REFIID, LPVOID*);
    using DllCanUnloadNowFunction = HRESULT(STDAPICALLTYPE*)();

    CoreEngineDll()
    {
        wchar_t executablePath[MAX_PATH] = {};
        const DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, ARRAYSIZE(executablePath));
        if (pathLength == 0 || pathLength == ARRAYSIZE(executablePath))
        {
            m_loadError = pathLength == 0 ? GetLastError() : ERROR_INSUFFICIENT_BUFFER;
            return;
        }

#if defined(_M_ARM64)
        constexpr auto platformDirectory = L"ARM64";
#else
        constexpr auto platformDirectory = L"x64";
#endif

        const std::filesystem::path dllPath = std::filesystem::path(executablePath)
            .parent_path()
            .parent_path()
            .parent_path()
            .parent_path() /
            L"CoreEngine" / platformDirectory / L"Debug" / L"CoreEngine.dll";

        m_module.reset(LoadLibraryW(dllPath.c_str()));
        if (!m_module)
        {
            m_loadError = GetLastError();
            return;
        }

        m_dllGetClassObject = reinterpret_cast<DllGetClassObjectFunction>(
            GetProcAddress(m_module.get(), "DllGetClassObject"));
        m_dllCanUnloadNow = reinterpret_cast<DllCanUnloadNowFunction>(
            GetProcAddress(m_module.get(), "DllCanUnloadNow"));
        if (!m_dllGetClassObject || !m_dllCanUnloadNow)
        {
            m_loadError = GetLastError();
        }
    }

    ~CoreEngineDll()
    {
        if (m_module && m_dllCanUnloadNow && m_dllCanUnloadNow() != S_OK)
        {
            m_module.release();
        }
    }

    bool IsLoaded() const noexcept
    {
        return m_module && m_dllGetClassObject && m_dllCanUnloadNow;
    }

    DWORD LoadError() const noexcept { return m_loadError; }

    HRESULT GetClassFactory(IClassFactory** factory) const
    {
        static constexpr CLSID sapiEngineClsid = {
            0x91cd243c, 0x63f7, 0x441f, { 0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d }
        };
        return GetClassObject(sapiEngineClsid, IID_IClassFactory, reinterpret_cast<void**>(factory));
    }

    HRESULT GetClassObject(REFCLSID clsid, REFIID iid, void** object) const
    {
        return m_dllGetClassObject(clsid, iid, object);
    }

    HRESULT CanUnloadNow() const
    {
        return m_dllCanUnloadNow();
    }

private:
    wil::unique_hmodule m_module;
    DllGetClassObjectFunction m_dllGetClassObject = nullptr;
    DllCanUnloadNowFunction m_dllCanUnloadNow = nullptr;
    DWORD m_loadError = ERROR_SUCCESS;
};

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

    bool WriteAudio(const std::vector<uint8_t>& bytes)
    {
        if (m_connectThread.joinable()) m_connectThread.join();
        if (m_connectError != ERROR_SUCCESS) return false;

        DWORD bytesWritten = 0;
        return WriteFile(m_audioPipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, nullptr) &&
            bytesWritten == bytes.size();
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

TEST_F(SapiEngineTests, DllCanUnloadNowTracksFactoryLifetime) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    EXPECT_EQ(module.CanUnloadNow(), S_OK);

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);
    EXPECT_EQ(module.CanUnloadNow(), S_FALSE);

    factory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, DllCanUnloadNowTracksEngineLifetimeAfterFactoryRelease) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

    winrt::com_ptr<ISpTTSEngine> engine;
    ASSERT_EQ(factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), engine.put_void()), S_OK);
    factory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_FALSE);

    engine = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, LockServerKeepsDllResidentUntilBalancedUnlock) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> lockingFactory;
    ASSERT_EQ(module.GetClassFactory(lockingFactory.put()), S_OK);
    ASSERT_EQ(lockingFactory->LockServer(TRUE), S_OK);
    lockingFactory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_FALSE);

    winrt::com_ptr<IClassFactory> unlockingFactory;
    ASSERT_EQ(module.GetClassFactory(unlockingFactory.put()), S_OK);
    ASSERT_EQ(unlockingFactory->LockServer(FALSE), S_OK);
    unlockingFactory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, CoreEngineDllDoesNotUnloadWhileServerLockIsActive) {
    HMODULE lockedModule = nullptr;
    {
        CoreEngineDll module;
        ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

        winrt::com_ptr<IClassFactory> factory;
        ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);
        ASSERT_EQ(factory->LockServer(TRUE), S_OK);
        factory = nullptr;
        ASSERT_EQ(module.CanUnloadNow(), S_FALSE);

        lockedModule = GetModuleHandleW(L"CoreEngine.dll");
        ASSERT_NE(lockedModule, nullptr);
    }

    ASSERT_EQ(GetModuleHandleW(L"CoreEngine.dll"), lockedModule);
    auto dllGetClassObject = reinterpret_cast<CoreEngineDll::DllGetClassObjectFunction>(
        GetProcAddress(lockedModule, "DllGetClassObject"));
    auto dllCanUnloadNow = reinterpret_cast<CoreEngineDll::DllCanUnloadNowFunction>(
        GetProcAddress(lockedModule, "DllCanUnloadNow"));
    ASSERT_NE(dllGetClassObject, nullptr);
    ASSERT_NE(dllCanUnloadNow, nullptr);

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, { 0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d }
    };
    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(dllGetClassObject(sapiEngineClsid, IID_IClassFactory, factory.put_void()), S_OK);
    ASSERT_EQ(factory->LockServer(FALSE), S_OK);
    factory = nullptr;
    ASSERT_EQ(dllCanUnloadNow(), S_OK);
    ASSERT_TRUE(FreeLibrary(lockedModule));
}

TEST_F(SapiEngineTests, DllGetClassObjectClearsOutputStorageBeforeRejectingUnknownClass) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    static constexpr CLSID unknownClsid = {
        0x6e9bf9d2, 0x65ee, 0x45c5, { 0xa3, 0xbc, 0x6d, 0xdc, 0xc8, 0xf1, 0x21, 0x71 }
    };
    void* object = reinterpret_cast<void*>(static_cast<uintptr_t>(1));

    EXPECT_EQ(module.GetClassObject(unknownClsid, IID_IClassFactory, &object), CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(object, nullptr);
}

TEST_F(SapiEngineTests, DllGetClassObjectRejectsNullOutputStorage) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, { 0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d }
    };

    EXPECT_EQ(module.GetClassObject(sapiEngineClsid, IID_IClassFactory, nullptr), E_POINTER);
}

TEST_F(SapiEngineTests, CreateInstanceClearsOutputStorageBeforeRejectingAggregation) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

    void* object = reinterpret_cast<void*>(static_cast<uintptr_t>(1));
    EXPECT_EQ(factory->CreateInstance(factory.get(), IID_IUnknown, &object), CLASS_E_NOAGGREGATION);
    EXPECT_EQ(object, nullptr);
}

TEST_F(SapiEngineTests, CreateInstanceRejectsNullOutputStorage) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

    EXPECT_EQ(factory->CreateInstance(nullptr, IID_IUnknown, nullptr), E_POINTER);
}

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
    EXPECT_EQ(received.wParam, 5u);
    EXPECT_EQ(received.lParam, 17);
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
    EXPECT_EQ(received.wParam, 9u);
    EXPECT_EQ(received.lParam, 22);
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
    SpeechWorker worker(engine.get(), &client, 2);
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

TEST_F(SapiEngineTests, MisalignedSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 11));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":11,\"total_audio_bytes\":1}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, MissingSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 21));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":21}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, NonIntegerSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 22));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":22,\"total_audio_bytes\":2.5}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, DuplicateSynthesisCompleteTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 23));

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":23,\"total_audio_bytes\":0}\n"));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, StaleSynthesisCompleteForDifferentSpeakIdDoesNotFaultIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 32));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":32,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    worker.PauseNextEventForwardForTest();

    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":31,\"total_audio_bytes\":0}\n"));
    const bool eventPaused = worker.WaitForEventForwardPauseForTest(1000);
    worker.ReleaseEventForwardForTest();

    EXPECT_TRUE(eventPaused);
    EXPECT_FALSE(worker.IsFaulted());
    EXPECT_TRUE(worker.Start(nullptr, 33));
    worker.Stop();
}

TEST_F(SapiEngineTests, TerminalBeforeOverrunAudioForwardsOnlyDeclaredFrames) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 26));
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
    ASSERT_TRUE(worker.Start(nullptr, 27));

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

TEST_F(SapiEngineTests, AudioAfterNormalCompletionFaultsIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 24));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":24,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);

    ASSERT_TRUE(server.WriteAudio({ 0xA1, 0xA2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, FaultPendingRejectsStartBeforeFaultPublicationCompletes) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 28));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":28,\"total_audio_bytes\":0}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    worker.PauseNextFaultPublicationForTest();

    ASSERT_TRUE(server.WriteAudio({ 0xE1, 0xE2 }));
    const bool publicationPaused = worker.WaitForFaultPublicationPauseForTest(1000);
    EXPECT_TRUE(publicationPaused);
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_FALSE(worker.Start(nullptr, 29));
    worker.ReleaseFaultPublicationForTest();

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, FrameAssemblyFailureFaultsWorkerWithoutEscapingThread) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 30));
    worker.FailNextFrameAssemblyForTest();

    ASSERT_TRUE(server.WriteAudio({ 0xF1, 0xF2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
    EXPECT_EQ(mockSite->writeCallCount.load(), 0u);
}

TEST_F(SapiEngineTests, AudioAfterCancellationCompletionFaultsIdleWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 25));

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":25,\"audio_bytes_written\":0}\n"));
    cancellationThread.join();
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(server.WriteAudio({ 0xB1, 0xB2 }));

    EXPECT_TRUE(worker.WaitForFaultForTest(1000));
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, MisalignedCancellationTotalFaultsTheWorker) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 12));

    HRESULT cancellationResult = S_OK;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":12,\"audio_bytes_written\":1}\n"));

    cancellationThread.join();
    EXPECT_EQ(cancellationResult, E_FAIL);
    EXPECT_TRUE(worker.IsFaulted());
}

TEST_F(SapiEngineTests, CancellationDiscardsCarriedPcmBeforeTheNextRequest) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    engine->m_cpSite.copy_from(mockSite.get());
    SpeechWorker worker(engine.get(), &client, 2);
    ASSERT_TRUE(worker.Start(nullptr, 13));
    ASSERT_TRUE(server.WriteAudio({ 0xA1 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 1; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 1u);

    HRESULT cancellationResult = E_FAIL;
    std::thread cancellationThread([&] { cancellationResult = worker.CancelAndDrain(); });
    ThreadJoinGuard cancellationJoin(cancellationThread);
    std::string cancellationRequest;
    ASSERT_TRUE(server.ReadControl(cancellationRequest));
    ASSERT_TRUE(server.WriteAudio({ 0xA2 }));
    for (int attempt = 0; attempt < 100 && worker.RawAudioBytesForTest() != 2; ++attempt) Sleep(1);
    ASSERT_EQ(worker.RawAudioBytesForTest(), 2u);
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_cancelled\",\"speak_id\":13,\"audio_bytes_written\":2}\n"));
    cancellationThread.join();
    ASSERT_EQ(cancellationResult, S_OK);

    ASSERT_TRUE(worker.Start(nullptr, 14));
    ASSERT_TRUE(server.WriteAudio({ 0xB1, 0xB2 }));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":14,\"total_audio_bytes\":2}\n"));
    ASSERT_EQ(worker.WaitUntilFinished(nullptr), S_OK);

    std::lock_guard<std::mutex> lock(mockSite->writesMutex);
    EXPECT_EQ(mockSite->requestedWriteSizes, (std::vector<ULONG>{ 2 }));
    EXPECT_EQ(mockSite->acceptedAudio, (std::vector<uint8_t>{ 0xB1, 0xB2 }));
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
