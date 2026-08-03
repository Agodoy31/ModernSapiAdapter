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

HRESULT PipeClient::Connect(const std::wstring& pipeName, const std::wstring& exePath)
{
    std::wstring sid = GetCurrentUserSid();
    std::wstring controlPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\control";
    std::wstring audioPipePath = L"\\\\.\\pipe\\" + pipeName + L"\\" + sid + L"\\audio";

    // Try to connect directly first.
    HRESULT hr = TryConnectPipes(controlPipePath, audioPipePath);
    if (SUCCEEDED(hr))
    {
        return S_OK;
    }

    // If WaitNamedPipe or CreateFile fails, invoke CreateProcessW to launch the provider
    wil::unique_process_information processInfo;
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE; // hidden/in-background

    // Create a mutable copy of the command line for CreateProcessW
    std::vector<wchar_t> cmdLine(exePath.begin(), exePath.end());
    cmdLine.push_back(L'\0');

    if (!CreateProcessW(
        nullptr,
        cmdLine.data(),
        nullptr,
        nullptr,
        FALSE,
        CREATE_NO_WINDOW,
        nullptr,
        nullptr,
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
        if (WaitNamedPipeW(controlPipePath.c_str(), NMPWAIT_USE_DEFAULT_WAIT))
        {
            hr = TryConnectPipes(controlPipePath, audioPipePath);
            if (SUCCEEDED(hr))
            {
                return S_OK;
            }
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

    DWORD mode = PIPE_READMODE_MESSAGE;
    SetNamedPipeHandleState(controlPipe.get(), &mode, nullptr, nullptr);

    m_controlPipe = std::move(controlPipe);
    m_audioPipe = std::move(audioPipe);

    return S_OK;
}

HRESULT PipeClient::SendControlMessage(const winrt::Windows::Data::Json::JsonObject& json)
{
    if (!m_controlPipe) return E_UNEXPECTED;

    std::wstring jsonStringW = json.Stringify().c_str();
    int utf8Len = WideCharToMultiByte(CP_UTF8, 0, jsonStringW.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (utf8Len == 0) return HRESULT_FROM_WIN32(GetLastError());

    std::string utf8String(utf8Len, 0);
    WideCharToMultiByte(CP_UTF8, 0, jsonStringW.c_str(), -1, utf8String.data(), utf8Len, nullptr, nullptr);
    utf8String.resize(utf8Len - 1); 

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
    if (!m_controlPipe) return E_UNEXPECTED;

    std::vector<uint8_t> buffer(4096);
    wil::unique_event overlappedEvent(CreateEventW(nullptr, TRUE, FALSE, nullptr));
    if (!overlappedEvent) return HRESULT_FROM_WIN32(GetLastError());

    OVERLAPPED overlapped = {};
    overlapped.hEvent = overlappedEvent.get();

    DWORD bytesRead = 0;
    BOOL result = ReadFile(m_controlPipe.get(), buffer.data(), static_cast<DWORD>(buffer.size()), nullptr, &overlapped);

    if (!result && GetLastError() != ERROR_IO_PENDING)
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (!GetOverlappedResult(m_controlPipe.get(), &overlapped, &bytesRead, TRUE))
    {
        return HRESULT_FROM_WIN32(GetLastError());
    }

    if (bytesRead == 0) return E_FAIL;

    std::string utf8String(reinterpret_cast<char*>(buffer.data()), bytesRead);
    int wideLen = MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.size()), nullptr, 0);
    if (wideLen == 0) return HRESULT_FROM_WIN32(GetLastError());

    std::wstring wideString(wideLen, 0);
    MultiByteToWideChar(CP_UTF8, 0, utf8String.c_str(), static_cast<int>(utf8String.size()), wideString.data(), wideLen);

    try
    {
        outJson = winrt::Windows::Data::Json::JsonObject::Parse(wideString);
        return S_OK;
    }
    catch (const winrt::hresult_error& e)
    {
        return e.code();
    }
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
