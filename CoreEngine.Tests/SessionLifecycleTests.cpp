#include "pch.h"
#include "TestFixtureBase.h"
#include "MockSpObjectToken.h"
#include "MockSpTTSEngineSite.h"
#include "ControlPipeTestServer.h"
#include "CoreEngineDllFixture.h"
#include "../CoreEngine/PipeClient.h"
#include "../CoreEngine/SapiEngine.h"

using namespace TestInfrastructure;

namespace
{

class DllUnloadAdmissionBarrier final
{
public:
    DllUnloadAdmissionBarrier() :
        m_entryPaused(CreateEventW(nullptr, TRUE, FALSE, EventName(L"DllGetClassObjectPaused").c_str())),
        m_closingStarted(CreateEventW(nullptr, TRUE, FALSE, EventName(L"DllEntryClosing").c_str())),
        m_releaseEntry(CreateEventW(nullptr, TRUE, FALSE, EventName(L"DllGetClassObjectRelease").c_str()))
    {
    }

    [[nodiscard]] bool IsValid() const noexcept
    {
        return m_entryPaused && m_closingStarted && m_releaseEntry;
    }

    [[nodiscard]] bool WaitForEntryPaused(const DWORD timeoutMs) const noexcept
    {
        return WaitForSingleObject(m_entryPaused.get(), timeoutMs) == WAIT_OBJECT_0;
    }

    [[nodiscard]] bool WaitForClosingStarted(const DWORD timeoutMs) const noexcept
    {
        return WaitForSingleObject(m_closingStarted.get(), timeoutMs) == WAIT_OBJECT_0;
    }

    void ReleaseEntry() const noexcept
    {
        SetEvent(m_releaseEntry.get());
    }

private:
    [[nodiscard]] static std::wstring EventName(const wchar_t* const phase)
    {
        return L"Local\\ModernSapiAdapter.CoreEngine." + std::wstring(phase) + L"." +
            std::to_wstring(GetCurrentProcessId());
    }

    wil::unique_event m_entryPaused;
    wil::unique_event m_closingStarted;
    wil::unique_event m_releaseEntry;
};

} // namespace

TEST_F(SapiEngineTests, DllCanUnloadNowTracksFactoryLifetime) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

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

TEST_F(SapiEngineTests, DllCanUnloadNowDrainsLoggerAndAllowsRestartOnNextLoad) {
    {
        CoreEngineDll module;
        ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

        winrt::com_ptr<IClassFactory> factory;
        ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

        winrt::com_ptr<ISpTTSEngine> engine;
        ASSERT_EQ(factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), engine.put_void()), S_OK);
        factory = nullptr;
        engine = nullptr;

        EXPECT_EQ(module.CanUnloadNow(), S_OK);
    }

    {
        CoreEngineDll module;
        ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

        winrt::com_ptr<IClassFactory> factory;
        ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

        winrt::com_ptr<ISpTTSEngine> engine;
        ASSERT_EQ(factory->CreateInstance(nullptr, __uuidof(ISpTTSEngine), engine.put_void()), S_OK);
        factory = nullptr;
        engine = nullptr;

        EXPECT_EQ(module.CanUnloadNow(), S_OK);
    }
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

TEST_F(SapiEngineTests, DllCanUnloadNowRefusesUnloadAfterAnAdmittedFactoryPublishesAModuleLock) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    DllUnloadAdmissionBarrier barrier;
    ASSERT_TRUE(barrier.IsValid());

    winrt::com_ptr<IClassFactory> factory;
    HRESULT factoryResult = E_FAIL;
    std::thread factoryThread([&] {
        factoryResult = module.GetClassFactory(factory.put());
    });
    ThreadJoinGuard factoryJoin(factoryThread);
    auto releaseEntry = wil::scope_exit([&barrier] {
        barrier.ReleaseEntry();
    });

    ASSERT_TRUE(barrier.WaitForEntryPaused(1000));

    HRESULT unloadResult = E_FAIL;
    std::thread unloadThread([&] {
        unloadResult = module.CanUnloadNow();
    });
    ThreadJoinGuard unloadJoin(unloadThread);

    ASSERT_TRUE(barrier.WaitForClosingStarted(1000));
    barrier.ReleaseEntry();

    ASSERT_TRUE(factoryJoin.Join());
    ASSERT_EQ(factoryResult, S_OK);
    ASSERT_TRUE(unloadJoin.Join());
    EXPECT_EQ(unloadResult, S_FALSE);

    factory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, DllGetClassObjectRejectsNewAdmissionAfterUnloadApproval) {
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    EXPECT_EQ(module.CanUnloadNow(), S_OK);

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, { 0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d }
    };
    void* object = reinterpret_cast<void*>(1);
    EXPECT_EQ(module.GetClassObject(sapiEngineClsid, IID_IClassFactory, &object), CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(object, nullptr);
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

TEST_F(SapiEngineTests, GetObjectTokenDoesNotWaitForActiveSpeakSerialization) {
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    engine->m_cpToken.copy_from(mockToken.get());

    std::unique_lock<std::mutex> tokenLock(engine->m_speakMutex);
    std::atomic_bool getterStarted{false};
    std::atomic_bool getterCompleted{false};
    HRESULT getResult = E_FAIL;
    ISpObjectToken* returnedToken = nullptr;
    std::thread getter([&] {
        getterStarted.store(true, std::memory_order_release);
        getResult = engine->GetObjectToken(&returnedToken);
        getterCompleted.store(true, std::memory_order_release);
    });
    ThreadJoinGuard getterJoin(getter);

    EXPECT_TRUE(WaitForCondition([&] { return getterStarted.load(std::memory_order_acquire); }, 1000, 1));
    EXPECT_TRUE(WaitForCondition([&] { return getterCompleted.load(std::memory_order_acquire); }, 1000, 1));

    tokenLock.unlock();
    ASSERT_TRUE(getterJoin.Join());
    EXPECT_EQ(getResult, S_OK);
    EXPECT_EQ(returnedToken, mockToken.get());
    if (returnedToken)
    {
        returnedToken->Release();
    }
}

TEST_F(SapiEngineTests, GetOutputFormatFailsWhenNoProviderLoaded) {
    auto engine = winrt::make_self<CSapiEngine>();
    
    GUID formatId = GUID_NULL;
    WAVEFORMATEX* pWaveFormat = reinterpret_cast<WAVEFORMATEX*>(static_cast<uintptr_t>(0xDEADBEEF));
    
    HRESULT hr = engine->GetOutputFormat(nullptr, nullptr, &formatId, &pWaveFormat);
    EXPECT_EQ(hr, SPERR_UNINITIALIZED);
    EXPECT_EQ(pWaveFormat, nullptr);
}

TEST_F(SapiEngineTests, CreateProviderSessionDoesNotPublishAnInvalidInfoResponse) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    auto engine = winrt::make_self<CSapiEngine>();
    engine->m_config.executablePath = L"ignored.exe";
    engine->m_config.pipeName = server.PipeName();

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

TEST_F(SapiEngineTests, CreateProviderSessionAcceptsIntegralFloatAudioFormatNumbers) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    auto engine = winrt::make_self<CSapiEngine>();
    engine->m_config.executablePath = L"ignored.exe";
    engine->m_config.pipeName = server.PipeName();

    std::atomic_bool infoResponseSent{false};
    std::thread infoResponder([&server, &infoResponseSent] {
        std::string request;
        infoResponseSent = server.ReadControl(request) &&
            request == "{\"command\":\"info\"}\n" &&
            server.WriteControl(
                "{\"response\":\"info\",\"audio_format\":{\"sample_rate\":24000.0,\"bits_per_sample\":16.0,\"channels\":1.0}}\n");
    });

    const HRESULT sessionResult = engine->CreateProviderSessionLocked();
    infoResponder.join();
    ASSERT_EQ(sessionResult, S_OK);
    EXPECT_TRUE(infoResponseSent.load());
    EXPECT_EQ(engine->m_config.audioFormat.nSamplesPerSec, 24000u);
    EXPECT_EQ(engine->m_config.audioFormat.wBitsPerSample, 16u);
    EXPECT_EQ(engine->m_config.audioFormat.nChannels, 1u);
}

TEST_F(SapiEngineTests, SpeakRebuildsProviderSessionAfterSynthesisTimeout) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
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
TEST_F(SapiEngineTests, FailedSpeakDispatchQuarantinesSessionAndNextSpeakRecovers) {
    auto mockSite = winrt::make_self<MockSpTTSEngineSite>();
    auto engine = winrt::make_self<CSapiEngine>();
    auto mockToken = winrt::make_self<MockSpObjectToken>();
    ASSERT_EQ(engine->SetObjectToken(mockToken.get()), S_OK);

    GUID formatId = {};
    WAVEFORMATEX* pWaveFormat = nullptr;
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

TEST_F(SapiEngineTests, IdleProviderRemainsUsableBeyondTheActiveRequestDeadline) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    auto engine = winrt::make_self<CSapiEngine>();
    SpeechWorker worker(engine.get(), &client, 2);

    Sleep(1700);

    ASSERT_TRUE(worker.Start(33));
    ASSERT_TRUE(server.WriteControl(
        "{\"event\":\"synthesis_complete\",\"speak_id\":33,\"total_audio_bytes\":0}\n"));
    EXPECT_EQ(worker.WaitUntilFinished(nullptr), S_OK);
    EXPECT_FALSE(worker.IsFaulted());
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, ControlWriteTimeoutIncludesMutexContention) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    client.PauseNextControlWriteAfterLockForTest();

    HRESULT firstWriteResult = E_FAIL;
    std::thread firstWriteThread([&] {
        firstWriteResult = client.SendControlMessage({ {"command", "first"} });
    });
    ThreadJoinGuard firstWriteJoin(firstWriteThread);
    auto releaseControlWrite = wil::scope_exit([&] { client.ReleaseControlWriteForTest(); });
    ASSERT_TRUE(client.WaitForControlWritePauseForTest(1000));

    wil::unique_event secondWriteFinished(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    ASSERT_TRUE(secondWriteFinished);
    HRESULT secondWriteResult = E_FAIL;
    std::thread secondWriteThread([&] {
        secondWriteResult = client.SendControlMessage({ {"command", "second"} }, 50);
        SetEvent(secondWriteFinished.get());
    });
    ThreadJoinGuard secondWriteJoin(secondWriteThread);
    auto releaseControlWriteBeforeJoins = wil::scope_exit([&] { client.ReleaseControlWriteForTest(); });

    const DWORD completionBeforeRelease = WaitForSingleObject(secondWriteFinished.get(), 500);
    client.ReleaseControlWriteForTest();
    EXPECT_TRUE(secondWriteJoin.Join(1000));
    EXPECT_TRUE(firstWriteJoin.Join(1000));

    EXPECT_EQ(completionBeforeRelease, WAIT_OBJECT_0)
        << "The finite control-write timeout did not bound mutex contention.";
    EXPECT_EQ(secondWriteResult, HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    EXPECT_EQ(firstWriteResult, S_OK);
}
#endif

TEST_F(SapiEngineTests, ReadControlMessageRetainsSecondJsonLineFromOnePipeRead) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("{\"event\":\"first\"}\n{\"event\":\"second\"}\n"));

    nlohmann::json first;
    nlohmann::json second;
    ASSERT_EQ(client.ReadControlMessage(first), S_OK);
    ASSERT_EQ(client.ReadControlMessage(second), S_OK);

    EXPECT_EQ(first["event"], "first");
    EXPECT_EQ(second["event"], "second");
}

TEST_F(SapiEngineTests, ReadControlMessageReassemblesFragmentedJsonLine) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::atomic_bool writesSucceeded{true};
    std::thread writer([&server, &writesSucceeded] {
        writesSucceeded = server.WriteControl("{\"event\":\"");
        writesSucceeded = server.WriteControl("fragmented\"}\n") && writesSucceeded.load();
    });
    ThreadJoinGuard writerJoin(writer);

    nlohmann::json message;
    EXPECT_EQ(client.ReadControlMessage(message), S_OK);
    EXPECT_TRUE(writerJoin.Join(1000));

    ASSERT_TRUE(writesSucceeded);
    EXPECT_EQ(message["event"], "fragmented");
}

TEST_F(SapiEngineTests, ReadControlMessageOffsetInfrastructureSupportsSequentialReads) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("{\"id\":1}\n{\"id\":2}\n{\"id\":3}\n"));

    nlohmann::json msg1;
    nlohmann::json msg2;
    nlohmann::json msg3;

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_EQ(client.ReadControlMessage(msg3), S_OK);

    EXPECT_EQ(msg1["id"], 1);
    EXPECT_EQ(msg2["id"], 2);
    EXPECT_EQ(msg3["id"], 3);
}

TEST_F(SapiEngineTests, ReadControlMessageHandlesCRLFAndEmptyLines) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);
    ASSERT_TRUE(server.WriteControl("\r\n{\"msg\":\"hello\"}\r\n\n{\"msg\":\"world\"}\n"));

    nlohmann::json msg1;
    nlohmann::json msg2;

    EXPECT_EQ(client.ReadControlMessage(msg1), S_FALSE);
    EXPECT_TRUE(msg1.is_null());

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_FALSE(msg1.is_null());
    EXPECT_EQ(msg1["msg"], "hello");

    EXPECT_EQ(client.ReadControlMessage(msg2), S_FALSE);

    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_FALSE(msg2.is_null());
    EXPECT_EQ(msg2["msg"], "world");
}

TEST_F(SapiEngineTests, ReadControlMessageCompactsBufferAndPreservesMessagesAcrossCompactionThreshold) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string stream;
    constexpr int totalMsgs = 500;
    for (int i = 0; i < totalMsgs; ++i) {
        char buf[64];
        snprintf(buf, sizeof(buf), "{\"seq\":%d}\n", i);
        stream += buf;
    }

    std::thread writer([&server, stream]() {
        server.WriteControl(stream.c_str());
    });

    for (int i = 0; i < totalMsgs; ++i) {
        nlohmann::json msg;
        ASSERT_EQ(client.ReadControlMessage(msg), S_OK) << "Failed at index " << i;
        ASSERT_FALSE(msg.is_null()) << "Null json at index " << i;
        EXPECT_EQ(static_cast<int>(msg["seq"]), i);
    }
    writer.join();
}

TEST_F(SapiEngineTests, ReadControlMessageHandlesLargePayloadAcrossCompactionThreshold) {
    ControlPipeTestServer server;
    ASSERT_EQ(server.CreateError(), ERROR_SUCCESS);

    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string largeVal(4200, 'x');
    std::string msgStr1 = "{\"data\":\"" + largeVal + "\"}\n";
    std::string msgStr2 = "{\"data\":\"small\"}\n";

    std::thread writer([&server, msgStr1, msgStr2]() {
        server.WriteControl((msgStr1 + msgStr2).c_str());
    });

    nlohmann::json msg1;
    nlohmann::json msg2;

    ASSERT_EQ(client.ReadControlMessage(msg1), S_OK);
    ASSERT_FALSE(msg1.is_null());
    EXPECT_EQ(msg1["data"].get<std::string>().size(), 4200u);

    ASSERT_EQ(client.ReadControlMessage(msg2), S_OK);
    ASSERT_FALSE(msg2.is_null());
    EXPECT_EQ(msg2["data"], "small");

    writer.join();
}

TEST_F(SapiEngineTests, ReadControlMessage_InvalidUtf8_RecoversPipe) {
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    std::string invalidUtf8 = "{\"type\":\"event\"}\xff\n";
    server.WriteControl(invalidUtf8);

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_NE(hr, S_OK);

    std::string validUtf8 = "{\"type\":\"event\"}\n";
    server.WriteControl(validUtf8);

    hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_EQ(hr, S_OK) << "Should recover and read next message successfully";
}

TEST_F(SapiEngineTests, ReadControlMessage_O_N_LinearSearch) {
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    // Send a message in tiny chunks to test offset search tracking
    std::string chunk(100, ' ');
    for (int i = 0; i < 30; ++i) {
        server.WriteControl(chunk);
    }
    server.WriteControl("{\"type\":\"event\"}\n");

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 5000); 
    EXPECT_EQ(hr, S_OK);
    if (!outJson.is_null()) {
        EXPECT_EQ(outJson["type"], "event");
    }
}

TEST_F(SapiEngineTests, ReadControlMessage_MalformedJson) {
    ClearTestLogs();
    ControlPipeTestServer server;
    PipeClient client;
    ASSERT_EQ(client.Connect(server.PipeName(), L""), S_OK);

    // Write malformed JSON
    std::string badJson = "{ bad_json ]\n";
    server.WriteControl(badJson);

    nlohmann::json outJson;
    HRESULT hr = client.ReadControlMessage(outJson, 1000);
    EXPECT_TRUE(FAILED(hr));

    auto logs = GetTestLogs();
    bool foundParseError = false;
    for (const auto& log : logs) {
        if (log.find(L"JSON Parse Error:") != std::wstring::npos) {
            foundParseError = true;
            break;
        }
    }
    EXPECT_TRUE(foundParseError) << "Expected to find a JSON Parse Error log message.";
}
