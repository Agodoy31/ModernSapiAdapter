#include "pch.h"

static std::mutex g_testLogMutex;
static std::vector<std::wstring> g_testLogs;

void CoreLog(const wchar_t *fmt, ...) noexcept
{
    va_list args;
    va_start(args, fmt);

    try
    {
        wchar_t buffer[1024];
        vswprintf_s(buffer, 1024, fmt, args);

        std::lock_guard<std::mutex> lock(g_testLogMutex);
        g_testLogs.push_back(buffer);
    }
    catch (...)
    {
    }

    va_end(args);
}

std::vector<std::wstring> GetTestLogs()
{
    std::lock_guard<std::mutex> lock(g_testLogMutex);
    return g_testLogs;
}

void ClearTestLogs()
{
    std::lock_guard<std::mutex> lock(g_testLogMutex);
    g_testLogs.clear();
}
