/**
 * @file CoreEngineDllFixture.h
 * @brief Dynamic loader and class factory wrapper for CoreEngine.dll in COM lifetime tests.
 */

#pragma once

#include <windows.h>
#include <wil/resource.h>
#include <filesystem>
#include <unknwn.h>

namespace TestInfrastructure
{

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

#if defined(_DEBUG)
        constexpr auto configuration = L"Debug";
#else
        constexpr auto configuration = L"Release";
#endif

        const std::filesystem::path dllPath = std::filesystem::path(executablePath)
            .parent_path()
            .parent_path()
            .parent_path()
            .parent_path() /
            L"CoreEngine" / platformDirectory / configuration / L"CoreEngine.dll";

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

} // namespace TestInfrastructure
