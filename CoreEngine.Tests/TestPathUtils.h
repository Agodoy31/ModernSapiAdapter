/**
 * @file TestPathUtils.h
 * @brief Binary resolution and mock token memory helpers for CoreEngine test fixtures.
 */

#pragma once

#include <string>
#include <filesystem>
#include <windows.h>
#include <sapi.h>

inline std::wstring GetMockProviderPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath)) == 0)
    {
        return {};
    }
    const auto testOutputDirectory = std::filesystem::path(modulePath).parent_path();
    const auto binaryDirectory = testOutputDirectory.parent_path().parent_path().parent_path();
#if defined(_DEBUG)
    constexpr const wchar_t* configuration = L"Debug";
#else
    constexpr const wchar_t* configuration = L"Release";
#endif
#if defined(_M_ARM64)
    constexpr const wchar_t* platform = L"ARM64";
#else
    constexpr const wchar_t* platform = L"x64";
#endif
    const auto anyCpuPath = binaryDirectory / L"MockProvider" / L"AnyCPU" / configuration / L"MockProvider.exe";
    if (std::filesystem::exists(anyCpuPath))
    {
        return anyCpuPath.wstring();
    }
    const auto platformPath = binaryDirectory / L"MockProvider" / platform / configuration / L"MockProvider.exe";
    if (std::filesystem::exists(platformPath))
    {
        return platformPath.wstring();
    }
    return anyCpuPath.wstring();
}

inline HRESULT CopyMockTokenString(const std::wstring& value, LPWSTR* output)
{
    if (!output)
    {
        return E_POINTER;
    }

    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    auto copy = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!copy)
    {
        return E_OUTOFMEMORY;
    }

    wcscpy_s(copy, value.size() + 1, value.c_str());
    *output = copy;
    return S_OK;
}
