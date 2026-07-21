#pragma once

struct MockSpTTSEngineSite : winrt::implements<MockSpTTSEngineSite, ISpTTSEngineSite, ISpEventSink>
{
    ULONG totalBytesWritten = 0;
    ULONG writeCallCount = 0;
    std::vector<SPEVENT> receivedEvents;

    IFACEMETHODIMP AddEvents(const SPEVENT* pEventArray, ULONG ulCount) noexcept override {
        if (pEventArray && ulCount > 0) {
            for (ULONG i = 0; i < ulCount; ++i) {
                receivedEvents.push_back(pEventArray[i]);
            }
        }
        return S_OK;
    }
    IFACEMETHODIMP GetEventInterest(ULONGLONG*) noexcept override { return S_OK; }

    IFACEMETHODIMP_(DWORD) GetActions() noexcept override { return 0; }
    IFACEMETHODIMP Write(const void*, ULONG cb, ULONG* pcbWritten) noexcept override {
        totalBytesWritten += cb;
        writeCallCount++;
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    IFACEMETHODIMP GetRate(long*) noexcept override { return S_OK; }
    IFACEMETHODIMP GetVolume(USHORT*) noexcept override { return S_OK; }
    IFACEMETHODIMP GetSkipInfo(SPVSKIPTYPE*, long*) noexcept override { return S_OK; }
    IFACEMETHODIMP CompleteSkip(long) noexcept override { return S_OK; }
};

struct MockSpDataKey : winrt::implements<MockSpDataKey, ISpDataKey>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override {
        if (wcscmp(pszValueName, L"ProviderDLL") == 0) {
            const wchar_t* dllPath = L"MockProvider.dll";
            size_t size = (wcslen(dllPath) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), dllPath);
            return S_OK;
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

struct MockSpObjectToken : winrt::implements<MockSpObjectToken, ISpObjectToken, ISpDataKey>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override {
        if (wcscmp(pszValueName, L"ProviderDLL") == 0) {
            const wchar_t* dllPath = L"MockProvider.dll";
            size_t size = (wcslen(dllPath) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), dllPath);
            return S_OK;
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
