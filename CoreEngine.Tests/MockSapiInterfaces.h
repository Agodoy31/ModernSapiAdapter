#pragma once

inline std::wstring GetMockProviderPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath)) == 0) return {};

    const auto testOutputDirectory = std::filesystem::path(modulePath).parent_path();
    const auto binaryDirectory = testOutputDirectory.parent_path().parent_path().parent_path();
#if defined(_M_ARM64)
    constexpr const wchar_t* platform = L"ARM64";
#else
    constexpr const wchar_t* platform = L"x64";
#endif
#if defined(_DEBUG)
    constexpr const wchar_t* configuration = L"Debug";
#else
    constexpr const wchar_t* configuration = L"Release";
#endif
    return (binaryDirectory / L"MockProvider" / platform / configuration / L"MockProvider.exe").wstring();
}

inline HRESULT CopyMockTokenString(const std::wstring& value, LPWSTR* output)
{
    if (!output) return E_POINTER;

    const size_t bytes = (value.size() + 1) * sizeof(wchar_t);
    auto copy = static_cast<LPWSTR>(CoTaskMemAlloc(bytes));
    if (!copy) return E_OUTOFMEMORY;

    wcscpy_s(copy, value.size() + 1, value.c_str());
    *output = copy;
    return S_OK;
}

struct MockSpTTSEngineSite : winrt::implements<MockSpTTSEngineSite, ISpTTSEngineSite, ISpEventSink>
{
    std::atomic<ULONG> totalBytesWritten = 0;
    std::atomic<ULONG> writeCallCount = 0;
    std::atomic<ULONG> bytesAcceptedAfterRejectedWrite = 0;
    std::atomic_bool rejectNextWrite = false;
    std::atomic<DWORD> actions = SPVES_CONTINUE;
    std::function<DWORD()> getActionsCallback;
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

    IFACEMETHODIMP_(DWORD) GetActions() noexcept override {
        return getActionsCallback ? getActionsCallback() : actions.load();
    }
    IFACEMETHODIMP Write(const void*, ULONG cb, ULONG* pcbWritten) noexcept override {
        writeCallCount++;
        if (rejectNextWrite.exchange(false)) {
            m_rejectedWriteObserved = true;
            if (pcbWritten) *pcbWritten = 0;
            return E_FAIL;
        }
        totalBytesWritten += cb;
        if (m_rejectedWriteObserved.load()) {
            bytesAcceptedAfterRejectedWrite += cb;
        }
        if (pcbWritten) *pcbWritten = cb;
        return S_OK;
    }
    ULONG BytesAcceptedAfterRejectedWrite() const noexcept { return bytesAcceptedAfterRejectedWrite.load(); }
    IFACEMETHODIMP GetRate(long*) noexcept override { return S_OK; }
    IFACEMETHODIMP GetVolume(USHORT*) noexcept override { return S_OK; }
    IFACEMETHODIMP GetSkipInfo(SPVSKIPTYPE*, long*) noexcept override { return S_OK; }
    IFACEMETHODIMP CompleteSkip(long) noexcept override { return S_OK; }

private:
    std::atomic_bool m_rejectedWriteObserved = false;
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
            return CopyMockTokenString(GetMockProviderPath(), ppszValue);
        }
        if (wcscmp(pszValueName, L"ProviderPipeName") == 0) {
            return CopyMockTokenString(L"msa_mock_provider", ppszValue);
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
