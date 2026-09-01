/**
 * @file dllmain.cpp
 * @brief Standard unmanaged COM DLL entry points and registration for CoreEngine.
 */

#include "pch.h"
#include "DllUnloadPolicy.h"
#include "SapiEngine.h"
#ifdef _DEBUG
#include "AsyncLogger.h"
#endif

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace
{

#if defined(_DEBUG)
[[nodiscard]] bool ShutdownLoggerSafely() noexcept
{
    auto* const logger = AsyncLogger::GetInstance();
    if (logger == nullptr)
    {
        return true;
    }

    return logger->Shutdown();
}
#else
[[nodiscard]] constexpr bool ShutdownLoggerSafely() noexcept
{
    return true;
}
#endif

} // namespace

// IMPORTANT: This CLSID is strictly coupled with SapiManager. 
// If this GUID is ever changed, you MUST also update the 'CoreEngineClsid' 
// constant in SapiManager/Services/RegistryManager.cs, otherwise SAPI 5 
// will fail to instantiate the engine for voice tokens.
const CLSID CLSID_SapiEngine = { 0x91cd243c, 0x63f7, 0x441f, { 0xae, 0x2f, 0x45, 0x05, 0x70, 0x05, 0xcb, 0x6d } };
const wchar_t* SapiEngineClsidString = L"{91CD243C-63F7-441F-AE2F-45057005CB6D}";

/**
 * @class SapiEngineClassFactory
 * @brief Standard IClassFactory implementation leveraging C++/WinRT winrt::implements.
 */
class SapiEngineClassFactory : public winrt::implements<SapiEngineClassFactory, IClassFactory>
{
public:
    IFACEMETHODIMP CreateInstance(IUnknown* pUnkOuter, REFIID riid, void** ppvObject) noexcept override
    {
        CoreLog(L"[CoreEngine] CreateInstance called.");
        if (ppvObject == nullptr) return E_POINTER;
        *ppvObject = nullptr;
        if (pUnkOuter != nullptr) return CLASS_E_NOAGGREGATION;
        try
        {
            auto instance = winrt::make_self<CSapiEngine>();
            return instance.as(riid, ppvObject);
        }
        catch (const std::exception& e)
        {
            CoreLog(L"[CoreEngine] CreateInstance exception: %hs", e.what());
            return winrt::to_hresult();
        }
        catch (...)
        {
            CoreLog(L"[CoreEngine] CreateInstance unknown exception.");
            return winrt::to_hresult();
        }
    }

    IFACEMETHODIMP LockServer(BOOL fLock) noexcept override
    {
        if (fLock) { ++winrt::get_module_lock(); }
        else { --winrt::get_module_lock(); }
        return S_OK;
    }
};



STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    CoreLog(L"[CoreEngine] DllGetClassObject called.");
    if (ppv == nullptr) return E_POINTER;
    *ppv = nullptr;
    if (rclsid == CLSID_SapiEngine)
    {
        try
        {
            auto factory = winrt::make_self<SapiEngineClassFactory>();
            return factory.as(riid, ppv);
        }
        catch (...) { return winrt::to_hresult(); }
    }
    CoreLog(L"[CoreEngine] DllGetClassObject: CLSID mismatch!");
    return CLASS_E_CLASSNOTAVAILABLE;
}

STDAPI DllCanUnloadNow(void)
{
    return CoreEngine::EvaluateDllUnload(
        []() noexcept { return static_cast<std::uint32_t>(winrt::get_module_lock()); },
        ShutdownLoggerSafely);
}

STDAPI DllRegisterServer(void)
{
    wchar_t modulePath[MAX_PATH];
    GetModuleFileNameW((HINSTANCE)&__ImageBase, modulePath, ARRAYSIZE(modulePath));

    std::wstring subKey = L"CLSID\\";
    subKey += SapiEngineClsidString;

    HKEY hKey = nullptr;
    LSTATUS status = RegCreateKeyExW(HKEY_CLASSES_ROOT, subKey.c_str(), 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hKey, nullptr);
    if (status == ERROR_SUCCESS)
    {
        RegSetValueExW(hKey, nullptr, 0, REG_SZ, (const BYTE*)L"ModernSapiAdapter CoreEngine", sizeof(L"ModernSapiAdapter CoreEngine"));
        
        HKEY hInprocKey = nullptr;
        status = RegCreateKeyExW(hKey, L"InprocServer32", 0, nullptr, REG_OPTION_NON_VOLATILE, KEY_WRITE, nullptr, &hInprocKey, nullptr);
        if (status == ERROR_SUCCESS)
        {
            RegSetValueExW(hInprocKey, nullptr, 0, REG_SZ, (const BYTE*)modulePath, (DWORD)(wcslen(modulePath) + 1) * sizeof(wchar_t));
            RegSetValueExW(hInprocKey, L"ThreadingModel", 0, REG_SZ, (const BYTE*)L"Both", sizeof(L"Both"));
            RegCloseKey(hInprocKey);
        }
        RegCloseKey(hKey);
    }
    return (status == ERROR_SUCCESS) ? S_OK : HRESULT_FROM_WIN32(status);
}

STDAPI DllUnregisterServer(void)
{
    std::wstring subKey = L"CLSID\\";
    subKey += SapiEngineClsidString;
    LSTATUS status = RegDeleteTreeW(HKEY_CLASSES_ROOT, subKey.c_str());
    return (status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND) ? S_OK : HRESULT_FROM_WIN32(status);
}


