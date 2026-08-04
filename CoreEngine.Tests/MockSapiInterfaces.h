#pragma once

struct MockSpTTSEngineSite : winrt::implements<MockSpTTSEngineSite, ISpTTSEngineSite, ISpEventSink>
{
    std::atomic<ULONG> totalBytesWritten = 0;
    std::atomic<ULONG> writeCallCount = 0;
    std::mutex eventsMutex;
    std::vector<SPEVENT> receivedEvents;

    IFACEMETHODIMP AddEvents(const SPEVENT* pEventArray, ULONG ulCount) noexcept override {
        std::lock_guard<std::mutex> lock(eventsMutex);
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
        if (wcscmp(pszValueName, L"ProviderExecutablePath") == 0) {
            const wchar_t* path = L"d:\\Projects\\ModernSapiAdapter\\bin\\MockProvider\\MockProvider.exe";
            size_t size = (wcslen(path) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), path);
            return S_OK;
        }
        if (wcscmp(pszValueName, L"ProviderPipeName") == 0) {
            const wchar_t* name = L"msa_mock_provider";
            size_t size = (wcslen(name) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), name);
            return S_OK;
        }
        if (wcscmp(pszValueName, L"VoiceId") == 0) {
            const wchar_t* id = L"mock_voice_1";
            size_t size = (wcslen(id) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), id);
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

struct MockSpObjectToken : winrt::implements<MockSpObjectToken, ISpObjectToken>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override {
        if (wcscmp(pszValueName, L"ProviderExecutablePath") == 0) {
            const wchar_t* path = L"d:\\Projects\\ModernSapiAdapter\\bin\\MockProvider\\MockProvider.exe";
            size_t size = (wcslen(path) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), path);
            return S_OK;
        }
        if (wcscmp(pszValueName, L"ProviderPipeName") == 0) {
            const wchar_t* name = L"msa_mock_provider";
            size_t size = (wcslen(name) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), name);
            return S_OK;
        }
        if (wcscmp(pszValueName, L"VoiceId") == 0) {
            const wchar_t* id = L"mock_voice_1";
            size_t size = (wcslen(id) + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), id);
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
