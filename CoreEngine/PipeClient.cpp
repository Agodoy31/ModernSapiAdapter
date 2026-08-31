#include "pch.h"
#include "PipeClient.h"
#include "PipeSecurityUtils.h"

PipeClient::PipeClient() = default;

PipeClient::~PipeClient()
{
#if defined(_DEBUG)
    ReleaseControlWriteForTest();
#endif
    Cancel();
    m_controlPipe.reset();
    m_audioPipe.reset();
}

HRESULT PipeClient::Connect(const std::wstring& pipeName, const std::wstring& exePath)
{
    constexpr ULONGLONG pipeReadyTimeoutMs = 1000;
    constexpr DWORD pipeProbeIntervalMs = 10;

    m_controlInputBuffer.clear();
    m_controlInputOffset = 0;
    m_controlSearchOffset = 0;
    m_controlInputBuffer.reserve(8192);

    std::wstring sid = PipeSecurityUtils::GetCurrentUserSidString();
    std::wstring controlPipePath = PipeSecurityUtils::BuildPipePath(pipeName, sid, L"control");
    std::wstring audioPipePath = PipeSecurityUtils::BuildPipePath(pipeName, sid, L"audio");

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

HRESULT PipeClient::CompleteOverlappedOperation(
    HANDLE pipe,
    OVERLAPPED& overlapped,
    DWORD& bytesTransferred,
    DWORD timeoutMs) noexcept
{
    const BOOL completed = timeoutMs == INFINITE
        ? GetOverlappedResult(pipe, &overlapped, &bytesTransferred, TRUE)
        : GetOverlappedResultEx(pipe, &overlapped, &bytesTransferred, timeoutMs, FALSE);
    if (completed)
    {
        return S_OK;
    }

    const DWORD completionError = GetLastError();
    if (completionError != WAIT_TIMEOUT)
    {
        return HRESULT_FROM_WIN32(completionError);
    }

    // OVERLAPPED and its event are caller-owned stack objects. They cannot be released
    // until the kernel has completed cancellation of this exact operation.
    DWORD cancellationError = ERROR_SUCCESS;
    if (!CancelIoEx(pipe, &overlapped))
    {
        cancellationError = GetLastError();
    }

    DWORD cancelledBytes = 0;
    if (!GetOverlappedResult(pipe, &overlapped, &cancelledBytes, TRUE))
    {
        const DWORD reapError = GetLastError();
        if (reapError != ERROR_OPERATION_ABORTED)
        {
            return HRESULT_FROM_WIN32(reapError);
        }
    }

    if (cancellationError != ERROR_SUCCESS && cancellationError != ERROR_NOT_FOUND)
    {
        return HRESULT_FROM_WIN32(cancellationError);
    }

    bytesTransferred = 0;
    return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
}

HRESULT PipeClient::SendControlMessageUtf8(
    const std::string& utf8String,
    DWORD timeoutMs)
{
    if (utf8String.size() > (std::numeric_limits<DWORD>::max)())
    {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }

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

    HRESULT hr = CompleteOverlappedOperation(
        m_controlPipe.get(), overlapped, bytesWritten, timeoutMs);
    if (FAILED(hr))
    {
        return hr;
    }

    if (bytesWritten != utf8String.size())
    {
        return HRESULT_FROM_WIN32(ERROR_WRITE_FAULT);
    }

    return S_OK;
}

void PipeClient::CompactControlBuffer()
{
    if (m_controlInputOffset == m_controlInputBuffer.size())
    {
        m_controlInputBuffer.clear();
        m_controlInputOffset = 0;
        m_controlSearchOffset = 0;
    }
    else if (m_controlInputOffset >= 4096)
    {
        m_controlInputBuffer.erase(0, m_controlInputOffset);
        m_controlInputOffset = 0;
        m_controlSearchOffset = 0;
    }
}

HRESULT PipeClient::ReadControlMessageUtf8(
    std::string_view& utf8View,
    DWORD timeoutMs)
{
    constexpr size_t maxControlRecordBytes = 16 * 1024 * 1024;
    if (!m_controlPipe) return E_UNEXPECTED;

    char chunk[4096];
    const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;

    while (m_controlInputBuffer.find('\n', m_controlSearchOffset) == std::string::npos)
    {
        if (m_controlInputBuffer.size() - m_controlInputOffset > maxControlRecordBytes)
        {
            return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
        }

        DWORD operationTimeout = INFINITE;
        if (timeoutMs != INFINITE)
        {
            const ULONGLONG now = GetTickCount64();
            if (now >= deadline)
            {
                return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
            }
            operationTimeout = static_cast<DWORD>(deadline - now);
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
            if (err != ERROR_IO_PENDING)
            {
                return HRESULT_FROM_WIN32(err);
            }
        }

        HRESULT hr = CompleteOverlappedOperation(
            m_controlPipe.get(), overlapped, bytesRead, operationTimeout);
        if (FAILED(hr))
        {
            return hr;
        }

        if (bytesRead > 0)
        {
            m_controlInputBuffer.append(chunk, bytesRead);
            m_controlSearchOffset = m_controlInputBuffer.size() - bytesRead;
        }
        else
        {
            return E_FAIL;
        }
    }

    const size_t newlinePos = m_controlInputBuffer.find('\n', m_controlSearchOffset);
    if (newlinePos - m_controlInputOffset > maxControlRecordBytes)
    {
        return HRESULT_FROM_WIN32(ERROR_BUFFER_OVERFLOW);
    }

    utf8View = std::string_view(m_controlInputBuffer.data() + m_controlInputOffset, newlinePos - m_controlInputOffset);
    if (utf8View.ends_with('\r'))
    {
        utf8View.remove_suffix(1);
    }

    m_controlInputOffset = newlinePos + 1;
    m_controlSearchOffset = m_controlInputOffset;
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

void PipeClient::FailNextSpeakMessageForTest()
{
    m_failNextSpeakMessageForTest = true;
}

void PipeClient::PauseNextControlWriteAfterLockForTest()
{
    std::lock_guard<std::mutex> lock(m_controlWriteTestMutex);
    m_pauseNextControlWriteAfterLockForTest = true;
    m_controlWritePausedForTest = false;
}

bool PipeClient::WaitForControlWritePauseForTest(DWORD timeoutMs)
{
    std::unique_lock<std::mutex> lock(m_controlWriteTestMutex);
    return m_controlWriteTestChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_controlWritePausedForTest;
    });
}

void PipeClient::ReleaseControlWriteForTest()
{
    std::lock_guard<std::mutex> lock(m_controlWriteTestMutex);
    m_pauseNextControlWriteAfterLockForTest = false;
    m_controlWritePausedForTest = false;
    m_controlWriteTestChanged.notify_all();
}
#endif

HRESULT PipeClient::SendControlMessage(
    const nlohmann::json& json,
    DWORD timeoutMs)
{
    const ULONGLONG deadline = timeoutMs == INFINITE ? 0 : GetTickCount64() + timeoutMs;
#if defined(_DEBUG)
    const bool traceCancellation = json.contains("command") && json["command"].is_string() &&
        json["command"].get<std::string_view>() == "cancel";
    const uint64_t traceSpeakId = traceCancellation && json.contains("speak_id") && json["speak_id"].is_number_unsigned()
        ? json["speak_id"].get<uint64_t>()
        : 0;
    const ULONGLONG controlMutexWaitStart = GetTickCount64();
#endif
    std::unique_lock<std::timed_mutex> lock(m_controlWriteMutex, std::defer_lock);
    if (timeoutMs == INFINITE)
    {
        lock.lock();
    }
    else
    {
        const ULONGLONG beforeLock = GetTickCount64();
        if (beforeLock >= deadline ||
            !lock.try_lock_for(std::chrono::milliseconds(deadline - beforeLock)))
        {
#if defined(_DEBUG)
            if (traceCancellation)
            {
                CoreLog(L"[CancelTrace] speak_id=%llu control_write_mutex_timeout tick=%llu wait_ms=%llu.",
                    traceSpeakId, GetTickCount64(), GetTickCount64() - controlMutexWaitStart);
            }
#endif
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
    }
#if defined(_DEBUG)
    {
        std::unique_lock<std::mutex> testLock(m_controlWriteTestMutex);
        if (m_pauseNextControlWriteAfterLockForTest)
        {
            m_pauseNextControlWriteAfterLockForTest = false;
            m_controlWritePausedForTest = true;
            m_controlWriteTestChanged.notify_all();
            m_controlWriteTestChanged.wait(testLock, [this] {
                return !m_controlWritePausedForTest;
            });
        }
    }
#endif
#if defined(_DEBUG)
    if (traceCancellation)
    {
        CoreLog(L"[CancelTrace] speak_id=%llu control_write_mutex_acquired tick=%llu wait_ms=%llu.",
            traceSpeakId, GetTickCount64(), GetTickCount64() - controlMutexWaitStart);
    }
#endif
    if (!m_controlPipe) return E_UNEXPECTED;

#if defined(_DEBUG)
    if (m_failNextSpeakMessageForTest &&
        json.contains("command") &&
        json["command"].is_string() &&
        json["command"].get<std::string>() == "sapi_speak")
    {
        m_failNextSpeakMessageForTest = false;
        return E_FAIL;
    }

    if (m_failNextCancellationMessageForTest &&
        json.contains("command") &&
        json["command"].is_string() &&
        json["command"].get<std::string>() == "cancel")
    {
        m_failNextCancellationMessageForTest = false;
        return E_FAIL;
    }
#endif

    std::string utf8String = json.dump() + "\n";
    DWORD writeTimeout = INFINITE;
    if (timeoutMs != INFINITE)
    {
        const ULONGLONG now = GetTickCount64();
        if (now >= deadline)
        {
            return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        }
        writeTimeout = static_cast<DWORD>(deadline - now);
    }
    HRESULT hr = SendControlMessageUtf8(utf8String, writeTimeout);
#if defined(_DEBUG)
    if (traceCancellation)
    {
        CoreLog(L"[CancelTrace] speak_id=%llu control_write_complete tick=%llu total_ms=%llu hr=0x%08x.",
            traceSpeakId, GetTickCount64(), GetTickCount64() - controlMutexWaitStart, hr);
    }
#endif
    return hr;
}

HRESULT PipeClient::ReadControlMessage(
    nlohmann::json& outJson,
    DWORD timeoutMs)
{
    outJson.clear();
    std::string_view utf8View;
    HRESULT hr = ReadControlMessageUtf8(utf8View, timeoutMs);
    if (FAILED(hr)) return hr;

    bool isEmpty = utf8View.empty();
    HRESULT parseHr = S_OK;

    if (isEmpty)
    {
        parseHr = S_FALSE;
    }
    else
    {
        try {
            outJson = nlohmann::json::parse(utf8View);
        } catch (const nlohmann::json::exception& e) {
            CoreLog(L"JSON Parse Error: %S", e.what());
            parseHr = E_FAIL; 
        }
    }

    CompactControlBuffer();
    return parseHr;
}
