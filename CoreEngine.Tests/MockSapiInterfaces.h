#pragma once

inline std::wstring GetMockProviderPath()
{
    wchar_t modulePath[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, modulePath, ARRAYSIZE(modulePath)) == 0) return {};
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
    if (std::filesystem::exists(anyCpuPath)) return anyCpuPath.wstring();
    const auto platformPath = binaryDirectory / L"MockProvider" / platform / configuration / L"MockProvider.exe";
    if (std::filesystem::exists(platformPath)) return platformPath.wstring();
    return anyCpuPath.wstring();
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
    std::atomic<DWORD> writeDelayMs = 0;
    std::atomic<DWORD> actions = SPVES_CONTINUE;
    std::function<DWORD()> getActionsCallback;
    std::mutex eventsMutex;
    std::vector<SPEVENT> receivedEvents;
    std::mutex writesMutex;
    std::condition_variable writeCondition;
    std::vector<ULONG> requestedWriteSizes;
    std::vector<uint8_t> acceptedAudio;

    bool WaitForBytesWritten(ULONG expectedBytes, DWORD timeoutMs = 2000) {
        std::unique_lock<std::mutex> lock(writesMutex);
        return writeCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return totalBytesWritten.load() >= expectedBytes;
        });
    }

    void PauseNextWrite() {
        std::lock_guard<std::mutex> lock(m_writePauseMutex);
        m_pauseNextWrite = true;
        m_writePaused = false;
        m_releaseWrite = false;
    }

    bool WaitForWritePause(DWORD timeoutMs) {
        std::unique_lock<std::mutex> lock(m_writePauseMutex);
        return m_writePauseCondition.wait_for(lock, std::chrono::milliseconds(timeoutMs), [&] {
            return m_writePaused;
        });
    }

    void ReleaseWrite() {
        {
            std::lock_guard<std::mutex> lock(m_writePauseMutex);
            m_pauseNextWrite = false;
            m_releaseWrite = true;
        }
        m_writePauseCondition.notify_all();
    }

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
    IFACEMETHODIMP Write(const void* data, ULONG cb, ULONG* pcbWritten) noexcept override {
        {
            std::unique_lock<std::mutex> lock(m_writePauseMutex);
            if (m_pauseNextWrite) {
                m_pauseNextWrite = false;
                m_writePaused = true;
                m_writePauseCondition.notify_all();
                m_writePauseCondition.wait(lock, [&] { return m_releaseWrite; });
                m_writePaused = false;
                m_releaseWrite = false;
            }
        }
        const DWORD delay = writeDelayMs.load();
        if (delay > 0) {
            Sleep(delay);
        }
        writeCallCount++;
        {
            std::lock_guard<std::mutex> lock(writesMutex);
            requestedWriteSizes.push_back(cb);
        }
        if (rejectNextWrite.exchange(false)) {
            m_rejectedWriteObserved = true;
            if (pcbWritten) *pcbWritten = 0;
            return E_FAIL;
        }
        totalBytesWritten += cb;
        {
            std::lock_guard<std::mutex> lock(writesMutex);
            const auto bytes = static_cast<const uint8_t*>(data);
            acceptedAudio.insert(acceptedAudio.end(), bytes, bytes + cb);
        }
        writeCondition.notify_all();
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
    std::mutex m_writePauseMutex;
    std::condition_variable m_writePauseCondition;
    bool m_pauseNextWrite = false;
    bool m_writePaused = false;
    bool m_releaseWrite = false;
};

struct MockSpDataKey : winrt::implements<MockSpDataKey, ISpDataKey>
{
    IFACEMETHODIMP SetData(LPCWSTR, ULONG, const BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetData(LPCWSTR, ULONG*, BYTE*) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP SetStringValue(LPCWSTR, LPCWSTR) noexcept override { return E_NOTIMPL; }
    IFACEMETHODIMP GetStringValue(LPCWSTR pszValueName, LPWSTR* ppszValue) noexcept override {
        if (wcscmp(pszValueName, L"ProviderExecutablePath") == 0) {
            std::wstring path = GetMockProviderPath();
            size_t size = (path.length() + 1) * sizeof(wchar_t);
            *ppszValue = (LPWSTR)CoTaskMemAlloc(size);
            wcscpy_s(*ppszValue, size / sizeof(wchar_t), path.c_str());
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
