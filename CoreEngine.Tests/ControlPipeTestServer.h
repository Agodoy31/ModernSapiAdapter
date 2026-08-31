/**
 * @file ControlPipeTestServer.h
 * @brief In-memory Named Pipe test server simulating mock provider control and audio channels.
 */

#pragma once

#include <windows.h>
#include <sddl.h>
#include <wil/resource.h>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include "TestPathUtils.h"

namespace TestInfrastructure
{

inline std::wstring GetCurrentUserSidForTest()
{
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
    {
        return L"DefaultUser";
    }

    wil::unique_handle tokenHandle(token);
    DWORD bytesRequired = 0;
    GetTokenInformation(tokenHandle.get(), TokenUser, nullptr, 0, &bytesRequired);
    if (GetLastError() != ERROR_INSUFFICIENT_BUFFER)
    {
        return L"DefaultUser";
    }

    std::vector<BYTE> tokenBuffer(bytesRequired);
    if (!GetTokenInformation(tokenHandle.get(), TokenUser, tokenBuffer.data(), bytesRequired, &bytesRequired))
    {
        return L"DefaultUser";
    }

    auto tokenUser = reinterpret_cast<TOKEN_USER*>(tokenBuffer.data());
    LPWSTR sidText = nullptr;
    if (!ConvertSidToStringSidW(tokenUser->User.Sid, &sidText))
    {
        return L"DefaultUser";
    }

    wil::unique_hlocal_string sid(sidText);
    return sid.get();
}

class ControlPipeTestServer
{
public:
    ControlPipeTestServer()
    {
        static std::atomic_uint64_t nextPipeId{0};
        m_pipeName = L"CoreEngineTests_" + std::to_wstring(GetCurrentProcessId()) + L"_" + std::to_wstring(++nextPipeId);

        const std::wstring basePath = L"\\\\.\\pipe\\" + m_pipeName + L"\\" + GetCurrentUserSidForTest();
        m_controlPipe.reset(CreateNamedPipeW(
            (basePath + L"\\control").c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));
        m_audioPipe.reset(CreateNamedPipeW(
            (basePath + L"\\audio").c_str(),
            PIPE_ACCESS_DUPLEX,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            1,
            4096,
            4096,
            0,
            nullptr));

        if (!m_controlPipe || !m_audioPipe)
        {
            m_createError = GetLastError();
            return;
        }

        m_connectThread = std::thread([this] {
            ConnectPipe(m_controlPipe.get());
            ConnectPipe(m_audioPipe.get());
        });
    }

    ~ControlPipeTestServer()
    {
        if (m_controlPipe)
        {
            CancelIoEx(m_controlPipe.get(), nullptr);
        }
        if (m_audioPipe)
        {
            CancelIoEx(m_audioPipe.get(), nullptr);
        }
        m_controlPipe.reset();
        m_audioPipe.reset();
        if (m_connectThread.joinable())
        {
            m_connectThread.join();
        }
    }

    const std::wstring& PipeName() const { return m_pipeName; }
    DWORD CreateError() const { return m_createError; }

    bool WriteControl(const std::string& text)
    {
        if (m_connectThread.joinable())
        {
            m_connectThread.join();
        }
        if (m_connectError != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD bytesWritten = 0;
        return WriteFile(m_controlPipe.get(), text.data(), static_cast<DWORD>(text.size()), &bytesWritten, nullptr) && bytesWritten == text.size();
    }

    bool WriteAudio(const std::vector<uint8_t>& bytes)
    {
        if (m_connectThread.joinable())
        {
            m_connectThread.join();
        }
        if (m_connectError != ERROR_SUCCESS)
        {
            return false;
        }

        DWORD bytesWritten = 0;
        return WriteFile(m_audioPipe.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &bytesWritten, nullptr) &&
            bytesWritten == bytes.size();
    }

    bool ReadControl(std::string& text)
    {
        if (m_connectThread.joinable())
        {
            m_connectThread.join();
        }
        if (m_connectError != ERROR_SUCCESS)
        {
            return false;
        }

        char buffer[512] = {};
        DWORD bytesRead = 0;
        if (!ReadFile(m_controlPipe.get(), buffer, sizeof(buffer), &bytesRead, nullptr) || bytesRead == 0)
        {
            return false;
        }

        text.assign(buffer, bytesRead);
        return true;
    }

    void Disconnect()
    {
        if (m_connectThread.joinable())
        {
            m_connectThread.join();
        }
        m_controlPipe.reset();
        m_audioPipe.reset();
    }

private:
    void ConnectPipe(HANDLE pipe)
    {
        if (m_connectError != ERROR_SUCCESS)
        {
            return;
        }

        if (!ConnectNamedPipe(pipe, nullptr))
        {
            const DWORD error = GetLastError();
            if (error != ERROR_PIPE_CONNECTED)
            {
                m_connectError = error;
            }
        }
    }

    std::wstring m_pipeName;
    wil::unique_handle m_controlPipe;
    wil::unique_handle m_audioPipe;
    std::thread m_connectThread;
    std::atomic<DWORD> m_createError{ERROR_SUCCESS};
    std::atomic<DWORD> m_connectError{ERROR_SUCCESS};
};

} // namespace TestInfrastructure
