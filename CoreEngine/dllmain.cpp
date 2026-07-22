/**
 * @file dllmain.cpp
 * @brief Standard unmanaged COM DLL entry points and registration for CoreEngine.
 */

#include "pch.h"
#include "SapiEngine.h"

extern "C" IMAGE_DOS_HEADER __ImageBase;

// IMPORTANT: This CLSID is strictly coupled with SapiManager. 
// If this GUID is ever changed, you MUST also update the 'CoreEngineClsid' 
// constant in SapiManager/Services/RegistryManager.cs, otherwise SAPI 5 
// will fail to instantiate the engine for voice tokens.
const CLSID CLSID_SapiEngine = { 0xb7e2e0a6, 0xa067, 0x4286, { 0x9a, 0x38, 0x9f, 0xe7, 0xfa, 0x25, 0xc9, 0x8d } };
const wchar_t* SapiEngineClsidString = L"{B7E2E0A6-A067-4286-9A38-9FE7FA25C98D}";

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

    IFACEMETHODIMP LockServer(BOOL /*fLock*/) noexcept override
    {
        return S_OK;
    }
};

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID /*lpReserved*/)
{
    if (ul_reason_for_call == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(hModule);
        CoreLog(L"[CoreEngine] DLL_PROCESS_ATTACH");
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, LPVOID* ppv)
{
    CoreLog(L"[CoreEngine] DllGetClassObject called.");
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
    return S_FALSE; 
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


