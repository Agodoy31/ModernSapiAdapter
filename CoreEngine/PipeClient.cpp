#include "pch.h"
#include "PipeClient.h"
#include <sddl.h>

static std::wstring GetCurrentUserSid()
{
    std::wstring sidString = L"DefaultUser";
    HANDLE hToken = NULL;
    if (OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken))
    {
        DWORD dwLength = 0;
        GetTokenInformation(hToken, TokenUser, NULL, 0, &dwLength);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER)
        {
            PTOKEN_USER pTokenUser = (PTOKEN_USER)malloc(dwLength);
            if (pTokenUser)
            {
                if (GetTokenInformation(hToken, TokenUser, pTokenUser, dwLength, &dwLength))
                {
                    LPWSTR pSidStr = NULL;
                    if (ConvertSidToStringSidW(pTokenUser->User.Sid, &pSidStr))
                    {
                        sidString = pSidStr;
                        LocalFree(pSidStr);
                    }
                }
                free(pTokenUser);
            }
        }
        CloseHandle(hToken);
    }
    return sidString;
}

PipeClient::PipeClient() = default;

PipeClient::~PipeClient()
{
    Cancel();
    m_controlPipe.reset();
    m_audioPipe.reset();
}

HRESULT PipeClient::Connect(const std::wstring& pipeName, const std::wstring& exePath)
{
    constexpr ULONGLONG pipeReadyTimeoutMs = 1000;
    constexpr DWORD pipeProbeIntervalMs = 10;

    m_controlInputBuffer.clear();

    std::wstring sid = GetCurrentUserSid();
    std::wstring controlPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\control";
    std::wstring audioPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\audio";

    bool controlPipeOpened = false;
    HRESULT hr = TryConnectPipes(controlPipePath, audioPipePath, controlPipeOpened);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    if (HRESULT_CODE(hr) == ERROR_ACCESS_DENIED)
    {
        return hr;
    }

    // A missing control pipe cannot become available without its provider. In contrast,
    // a control pipe that is already open means the provider is starting and its audio
    // pipe must be given the bounded readiness window rather than launching a duplicate.
    if (!controlPipeOpened && HRESULT_CODE(hr) == ERROR_FILE_NOT_FOUND)
    {
        wil::unique_process_information processInfo;
        STARTUPINFOW si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        std::wstring fullCmdLine = L"\"" + exePath + L"\"";
        std::vector<wchar_t> cmdLine(fullCmdLine.begin(), fullCmdLine.end());
        cmdLine.push_back(L'\0');

        std::wstring exeDir = exePath.substr(0, exePath.find_last_of(L"\\/"));

        if (!CreateProcessW(
            nullptr,
            cmdLine.data(),
            nullptr,
            nullptr,
            FALSE,
            CREATE_NO_WINDOW,
            nullptr,
            exeDir.c_str(),
            &si,
            &processInfo))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        m_providerProcess = std::move(processInfo);
    }

    const ULONGLONG deadline = GetTickCount64() + pipeReadyTimeoutMs;
    while (GetTickCount64() < deadline)
    {
        if (m_providerProcess.hProcess && WaitForSingleObject(m_providerProcess.hProcess, 0) == WAIT_OBJECT_0)
        {
            return HRESULT_FROM_WIN32(ERROR_PROCESS_ABORTED);
        }

        const ULONGLONG remaining = deadline - GetTickCount64();
        const DWORD waitMs = static_cast<DWORD>(remaining < pipeProbeIntervalMs ? remaining : pipeProbeIntervalMs);
        if (HRESULT_CODE(hr) == ERROR_PIPE_BUSY)
        {
            const std::wstring& busyPipePath = controlPipeOpened ? audioPipePath : controlPipePath;
            WaitNamedPipeW(busyPipePath.c_str(), waitMs);
        }
        else
        {
            Sleep(waitMs);
        }

        controlPipeOpened = false;
        hr = TryConnectPipes(controlPipePath, audioPipePath, controlPipeOpened);
        if (SUCCEEDED(hr))
        {
            return S_OK;
        }

        if (HRESULT_CODE(hr) == ERROR_ACCESS_DENIED)
        {
            return hr;
        }
    }

    return hr;
}

HRESULT PipeClient::TryConnectPipes(
    const std::wstring& controlPipePath,
    const std::wstring& audioPipePath,
    bool& controlPipeOpened)
{
    controlPipeOpened = false;
    wil::unique_handle controlPipe(CreateFileW(
        controlPipePath.c_str(), 
        GENERIC_READ | GENERIC_WRITE, 
        0, 
        nullptr, 
        OPEN_EXISTING, 
        FILE_FLAG_OVERLAPPED, 
        nullptr));
        
    if (!controlPipe)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }
    controlPipeOpened = true;

    wil::unique_handle audioPipe(CreateFileW(
        audioPipePath.c_str(),
        GENERIC_READ,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED,
        nullptr));

    if (!audioPipe)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    DWORD mode = PIPE_READMODE_BYTE;
    if (!SetNamedPipeHandleState(controlPipe.get(), &mode, nullptr, nullptr))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    m_controlPipe = std::move(controlPipe);
    m_audioPipe = std::move(audioPipe);

    return S_OK;
}

HRESULT PipeClient::SendControlMessage(const winrt::Windows::Data::Json::JsonObject& json)
{
    std::lock_guard<std::mutex> lock(m_controlWriteMutex);
    if (!m_controlPipe) return E_UNEXPECTED;

#if defined(_DEBUG)
    if (m_failNextCancellationMessageForTest &&
        json.HasKey(L"command") &&
        json.GetNamedValue(L"command").ValueType() == winrt::Windows::Data::Json::JsonValueType::String &&
        json.GetNamedString(L"command") == L"cancel")
    {
        m_failNextCancellationMessageForTest = false;
        return E_FAIL;
    }
#endif

    winrt::hstring jsonHString = json.Stringify();
    std::wstring jsonStringW(jsonHString.c_str(), jsonHString.size());
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, jsonStringW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len == 0) return HRESULT_FROM_WIN32(GetLastError());

    std::string utf8String(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, jsonStringW.c_str(), -1, utf8String.data(), utf8Len, nullptr, nullptr);
    utf8String.resize(utf8Len - 1); 
    utf8String += "\n";

    wil::unique_event overlappedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!overlappedEvent) return HRESULT_FROM_WIN32(GetLastError());

    OVERLAPPED overlapped = {};
    overlapped.hEvent = overlappedEvent.get();

    DWORD bytesWritten = 0;
    BOOL result = WriteFile(m_controlPipe.get(), utf8String.data(), static_cast<DWORD>(utf8String.size()), nullptr, &overlapped);
    
    if (!result && GetLastError() != ERROR_IO_PENDING)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!GetOverlappedResult(m_controlPipe.get(), &overlapped, &bytesWritten, TRUE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

HRESULT PipeClient::ReadControlMessage(winrt::Windows::Data::Json::JsonObject& outJson)
{
    // Keeps long screen-reader Read All requests valid without allowing unbounded buffering.
    constexpr size_t maxControlRecordBytes = 16 * 1024 * 1024;

    outJson = nullptr;
    if (!m_controlPipe) return E_UNEXPECTED;

    char chunk[4096];

    while (m_controlInputBuffer.find('\n') == std::string::npos)
    {
        if (m_controlInputBuffer.size() > maxControlRecordBytes)
        {
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        }

        wil::unique_event overlappedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
        if (!overlappedEvent) return HRESULT_FROM_WIN32(GetLastError());

        OVERLAPPED overlapped = {};
        overlapped.hEvent = overlappedEvent.get();

        DWORD bytesRead = 0;
        BOOL success = ReadFile(m_controlPipe.get(), chunk, sizeof(chunk), nullptr, &overlapped);
        if (!success)
        {
            DWORD err = GetLastError();
            if (err == ERROR_IO_PENDING)
            {
            }
            else
            {
                return HRESULT_FROM_WIN32(err);
            }
        }

        if (!GetOverlappedResult(m_controlPipe.get(), &overlapped, &bytesRead, TRUE))
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }

        if (bytesRead > 0)
        {
            m_controlInputBuffer.append(chunk, bytesRead);
        }
        else
        {
            return E_FAIL;
        }
    }

    const size_t newlinePos = m_controlInputBuffer.find('\n');
    if (newlinePos > maxControlRecordBytes)
    {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }

    std::string utf8String = m_controlInputBuffer.substr(0, newlinePos);
    m_controlInputBuffer.erase(0, newlinePos + 1);

    if (!utf8String.empty() && utf8String.back() == '\r')
    {
        utf8String.pop_back();
    }

    if (utf8String.empty())
    {
        return S_FALSE;
    }

    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8String.data(), static_cast<int>(utf8String.size()), nullptr, 0);
    if (wideLen == 0) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring wideString(wideLen, 0);
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8String.data(), static_cast<int>(utf8String.size()), wideString.data(), wideLen) == 0)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    try
    {
        outJson = winrt::Windows::Data::Json::JsonObject::Parse(wideString);
    }
    catch (const winrt::hresult_error& e)
    {
        return e.code();
    }
    catch (...)
    {
        return E_FAIL;
    }

    return S_OK;
}

HRESULT PipeClient::ReadAudioChunk(std::vector<uint8_t>& buffer, DWORD& bytesRead)
{
    if (!m_audioPipe) return E_UNEXPECTED;

    wil::unique_event overlappedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!overlappedEvent) return HRESULT_FROM_WIN32(GetLastError());

    OVERLAPPED overlapped = {};
    overlapped.hEvent = overlappedEvent.get();

    bytesRead = 0;
    BOOL result = ReadFile(m_audioPipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped);

    if (!result && GetLastError() != ERROR_IO_PENDING)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!GetOverlappedResult(m_audioPipe.get(), &overlapped, &bytesRead, TRUE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    return S_OK;
}

void PipeClient::Cancel()
{
    if (m_controlPipe) CancelIoEx(m_controlPipe.get(), nullptr);
    if (m_audioPipe) CancelIoEx(m_audioPipe.get(), nullptr);
}

#if defined(_DEBUG)
void PipeClient::FailNextCancellationMessageForTest()
{
    m_failNextCancellationMessageForTest = true;
}
#endif
