#include "pch.h"
#include "TestFixtureBase.h"
#include "CoreEngineDllFixture.h"
#include "../CoreEngine/SapiEngine.h"

using namespace TestInfrastructure;

namespace
{

#if defined(_DEBUG)
class DllUnloadAdmissionBarrier final
{
  public:
    DllUnloadAdmissionBarrier()
        : m_entryPaused(CreateEventW(nullptr, TRUE, FALSE, EventName(L"DllGetClassObjectPaused").c_str())),
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
    [[nodiscard]] static std::wstring EventName(const wchar_t *const phase)
    {
        return L"Local\\ModernSapiAdapter.CoreEngine." + std::wstring(phase) + L"." +
               std::to_wstring(GetCurrentProcessId());
    }

    wil::unique_event m_entryPaused;
    wil::unique_event m_closingStarted;
    wil::unique_event m_releaseEntry;
};
#endif

} // namespace

TEST_F(SapiEngineTests, DllCanUnloadNowTracksFactoryLifetime)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);
    EXPECT_EQ(module.CanUnloadNow(), S_FALSE);

    factory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, DllCanUnloadNowTracksEngineLifetimeAfterFactoryRelease)
{
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

TEST_F(SapiEngineTests, DllCanUnloadNowDrainsLoggerAndAllowsRestartOnNextLoad)
{
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

TEST_F(SapiEngineTests, LockServerKeepsDllResidentUntilBalancedUnlock)
{
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

TEST_F(SapiEngineTests, CoreEngineDllDoesNotUnloadWhileServerLockIsActive)
{
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
    auto dllGetClassObject =
        reinterpret_cast<CoreEngineDll::DllGetClassObjectFunction>(GetProcAddress(lockedModule, "DllGetClassObject"));
    auto dllCanUnloadNow =
        reinterpret_cast<CoreEngineDll::DllCanUnloadNowFunction>(GetProcAddress(lockedModule, "DllCanUnloadNow"));
    ASSERT_NE(dllGetClassObject, nullptr);
    ASSERT_NE(dllCanUnloadNow, nullptr);

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, {0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d}};
    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(dllGetClassObject(sapiEngineClsid, IID_IClassFactory, factory.put_void()), S_OK);
    ASSERT_EQ(factory->LockServer(FALSE), S_OK);
    factory = nullptr;
    ASSERT_EQ(dllCanUnloadNow(), S_OK);
    ASSERT_TRUE(FreeLibrary(lockedModule));
}

#if defined(_DEBUG)
TEST_F(SapiEngineTests, DllCanUnloadNowRefusesUnloadAfterAnAdmittedFactoryPublishesAModuleLock)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    DllUnloadAdmissionBarrier barrier;
    ASSERT_TRUE(barrier.IsValid());

    winrt::com_ptr<IClassFactory> factory;
    HRESULT factoryResult = E_FAIL;
    std::thread factoryThread(
        [&]
        {
            factoryResult = module.GetClassFactory(factory.put());
        });
    ThreadJoinGuard factoryJoin(factoryThread);
    auto releaseEntry = wil::scope_exit(
        [&barrier]
        {
            barrier.ReleaseEntry();
        });

    ASSERT_TRUE(barrier.WaitForEntryPaused(1000));

    HRESULT unloadResult = E_FAIL;
    std::thread unloadThread(
        [&]
        {
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
#endif

TEST_F(SapiEngineTests, DllGetClassObjectSucceedsAfterUnloadApprovalIfReactivated)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    EXPECT_EQ(module.CanUnloadNow(), S_OK);

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, {0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d}};
    winrt::com_ptr<IClassFactory> factory;
    EXPECT_EQ(module.GetClassObject(sapiEngineClsid, IID_IClassFactory, factory.put_void()), S_OK);
    ASSERT_NE(factory, nullptr);

    winrt::com_ptr<IUnknown> engineInstance;
    EXPECT_EQ(factory->CreateInstance(nullptr, IID_IUnknown, engineInstance.put_void()), S_OK);
    EXPECT_NE(engineInstance, nullptr);

    EXPECT_EQ(module.CanUnloadNow(), S_FALSE);

    engineInstance = nullptr;
    factory = nullptr;
    EXPECT_EQ(module.CanUnloadNow(), S_OK);
}

TEST_F(SapiEngineTests, DllGetClassObjectClearsOutputStorageBeforeRejectingUnknownClass)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    static constexpr CLSID unknownClsid = {
        0x6e9bf9d2, 0x65ee, 0x45c5, {0xa3, 0xbc, 0x6d, 0xdc, 0xc8, 0xf1, 0x21, 0x71}};
    void *object = reinterpret_cast<void *>(static_cast<uintptr_t>(1));

    EXPECT_EQ(module.GetClassObject(unknownClsid, IID_IClassFactory, &object), CLASS_E_CLASSNOTAVAILABLE);
    EXPECT_EQ(object, nullptr);
}

TEST_F(SapiEngineTests, DllGetClassObjectRejectsNullOutputStorage)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    static constexpr CLSID sapiEngineClsid = {
        0x91cd243c, 0x63f7, 0x441f, {0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d}};

    EXPECT_EQ(module.GetClassObject(sapiEngineClsid, IID_IClassFactory, nullptr), E_POINTER);
}

TEST_F(SapiEngineTests, CreateInstanceClearsOutputStorageBeforeRejectingAggregation)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

    void *object = reinterpret_cast<void *>(static_cast<uintptr_t>(1));
    EXPECT_EQ(factory->CreateInstance(factory.get(), IID_IUnknown, &object), CLASS_E_NOAGGREGATION);
    EXPECT_EQ(object, nullptr);
}

TEST_F(SapiEngineTests, CreateInstanceRejectsNullOutputStorage)
{
    CoreEngineDll module;
    ASSERT_TRUE(module.IsLoaded()) << "Load error: " << module.LoadError();

    winrt::com_ptr<IClassFactory> factory;
    ASSERT_EQ(module.GetClassFactory(factory.put()), S_OK);

    EXPECT_EQ(factory->CreateInstance(nullptr, IID_IUnknown, nullptr), E_POINTER);
}
