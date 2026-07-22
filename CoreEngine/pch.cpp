#include "pch.h"
#include <stdarg.h>

static std::mutex g_coreLogMutex;
static std::wofstream g_coreLogFile;

void CoreLog(const wchar_t* fmt, ...)
{
    std::lock_guard<std::mutex> lock(g_coreLogMutex);
    if (!g_coreLogFile.is_open())
    {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)))
        {
            std::wstring logPath = path;
            CoTaskMemFree(path);
            logPath += L"\\ModernSapiAdapter\\Logs";
            CreateDirectoryW(logPath.c_str(), nullptr);
            logPath += L"\\CoreEngine.log";
            g_coreLogFile.open(logPath, std::ios::app);
        }
    }

    if (g_coreLogFile.is_open())
    {
        wchar_t buffer[2048];
        va_list args;
        va_start(args, fmt);
        _vsnwprintf_s(buffer, _countof(buffer), _TRUNCATE, fmt, args);
        va_end(args);

        g_coreLogFile << buffer << L"\n";
        g_coreLogFile.flush();
    }
}
