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

    if (m_providerProcess.hProcess)
    {
        TerminateProcess(m_providerProcess.hProcess, 0);
        WaitForSingleObject(m_providerProcess.hProcess, 1000);
    }
}

HRESULT PipeClient::Connect(const std::wstring& pipeName, const std::wstring& exePath)
{
    std::wstring sid = GetCurrentUserSid();
    std::wstring controlPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\control";
    std::wstring audioPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\audio";

    // Try to connect directly first (retry briefly in case provider is re-initializing pipes).
    HRESULT hr = E_FAIL;
    for (int i = 0; i < 30; ++i)
    {
        hr = TryConnectPipes(controlPipePath, audioPipePath);
        if (SUCCEEDED(hr))
        {
            return S_OK;
        }
        Sleep(50);
    }

    // If WaitNamedPipe or CreateFile fails, invoke CreateProcessW to launch the provider
    wil::unique_process_information processInfo;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // hidden/in-background

    // Create a mutable copy of the command line for CreateProcessW
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

    // Wait and retry connection
    const int maxRetries = 50;
    for (int i = 0; i < maxRetries; ++i)
    {
        Sleep(100); // Wait for the provider to start and create pipes
        hr = TryConnectPipes(controlPipePath, audioPipePath);
        if (SUCCEEDED(hr))
        {
            return S_OK;
        }
    }

    return hr;
}

HRESULT PipeClient::TryConnectPipes(const std::wstring& controlPipePath, const std::wstring& audioPipePath)
{
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

    wil::unique_handle audioPipe;
    for (int retry = 0; retry < 20; ++retry)
    {
        audioPipe.reset(CreateFileW(
            audioPipePath.c_str(), 
            GENERIC_READ, 
            0, 
            nullptr, 
            OPEN_EXISTING, 
            FILE_FLAG_OVERLAPPED, 
            nullptr));

        if (audioPipe) break;
        Sleep(50);
    }

    if (!audioPipe)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(controlPipe.get(), &mode, nullptr, nullptr);

    m_controlPipe = std::move(controlPipe);
    m_audioPipe = std::move(audioPipe);

    return S_OK;
}

HRESULT PipeClient::SendControlMessage(const winrt::Windows::Data::Json::JsonObject& json)
{
    if (!m_controlPipe) return E_UNEXPECTED;

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
    outJson = nullptr;
    if (!m_controlPipe) return E_UNEXPECTED;

    std::vector<char> buffer;
    char chunk[256];

    while (true)
    {
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
                if (!GetOverlappedResult(m_controlPipe.get(), &overlapped, &bytesRead, TRUE))
                {
                    return HRESULT_FROM_WIN32(GetLastError());
                }
            }
            else
            {
                return HRESULT_FROM_WIN32(err);
            }
        }

        if (bytesRead > 0)
        {
            buffer.insert(buffer.end(), chunk, chunk + bytesRead);
            if (buffer.back() == '\n')
            {
                break;
            }
        }
        else
        {
            return E_FAIL;
        }
    }

    std::string utf8String(buffer.begin(), buffer.end());
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.size()), nullptr, 0);
    if (wideLen == 0) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring wideString(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.size()), wideString.data(), wideLen);

    while (!wideString.empty() && (wideString.back() == L'\r' || wideString.back() == L'\n' || wideString.back() == L'\0' || wideString.back() == L' '))
    {
        wideString.pop_back();
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
