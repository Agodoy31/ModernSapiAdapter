/**
 * @file MockSpObjectToken.h
 * @brief High-fidelity test doubles implementing SAPI 5 ISpObjectToken and ISpDataKey.
 */

#pragma once

#include <windows.h>
#include <sapi.h>
#include <winrt/base.h>
#include <string>
#include "TestPathUtils.h"

struct MockSpDataKey : winrt::implements<MockSpDataKey, ISpDataKey>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override
    {
        if (wcscmp(pszValueName, L"ProviderExecutablePath") == 0)
        {
            return CopyMockTokenString(GetMockProviderPath(), ppszValue);
        }
        if (wcscmp(pszValueName, L"ProviderPipeName") == 0)
        {
            return CopyMockTokenString(L"msa_mock_provider", ppszValue);
        }
        if (wcscmp(pszValueName, L"VoiceId") == 0)
        {
            return CopyMockTokenString(L"mock_voice_1", ppszValue);
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetDWORD(LPCWSTR, DWORD) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDWORD(LPCWSTR, DWORD*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP OpenKey(LPCWSTR, ISpDataKey**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateKey(LPCWSTR, ISpDataKey**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteKey(LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteValue(LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumKeys(ULONG, LPWSTR*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumValues(ULONG, LPWSTR*) noexcept override { return E_NOTIMPL; }
};

struct MockSpObjectToken : winrt::implements<MockSpObjectToken, ISpObjectToken>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override
    {
        if (wcscmp(pszValueName, L"ProviderExecutablePath") == 0)
        {
            return CopyMockTokenString(GetMockProviderPath(), ppszValue);
        }
        if (wcscmp(pszValueName, L"ProviderPipeName") == 0)
        {
            return CopyMockTokenString(L"msa_mock_provider", ppszValue);
        }
        if (wcscmp(pszValueName, L"VoiceId") == 0)
        {
            return CopyMockTokenString(L"mock_voice_1", ppszValue);
        }
        return E_NOTIMPL;
    }
    IFACEMETHODIMP SetDWORD(LPCWSTR, DWORD) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetDWORD(LPCWSTR, DWORD*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP OpenKey(LPCWSTR, ISpDataKey**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateKey(LPCWSTR, ISpDataKey**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteKey(LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP DeleteValue(LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumKeys(ULONG, LPWSTR*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP EnumValues(ULONG, LPWSTR*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetId(LPCWSTR, LPCWSTR, BOOL) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetId(LPWSTR*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetCategory(ISpObjectTokenCategory**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP CreateInstance(IUnknown*, DWORD, REFIID, void**) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStorageFileName(REFCLSID, LPCWSTR, LPCWSTR, ULONG, LPWSTR*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP RemoveStorageFileName(REFCLSID, LPCWSTR, BOOL) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP Remove(const CLSID*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP IsUISupported(LPCWSTR, void*, ULONG, IUnknown*, BOOL*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP DisplayUI(HWND, LPCWSTR, LPCWSTR, void*, ULONG, IUnknown*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP MatchesAttributes(LPCWSTR, BOOL*) noexcept override { return E_NOTIMPL; }
};
