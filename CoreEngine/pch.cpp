#include "pch.h"
#include "AsyncLogger.h"

void CoreLog(const wchar_t* fmt, ...)
{
#ifdef _DEBUG
    va_list args;
    va_start(args, fmt);

    va_list argsCopy;
    va_copy(argsCopy, args);
    int len = _vscwprintf(fmt, argsCopy);
    va_end(argsCopy);

    if (len > 0)
    {
        std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
        _vsnwprintf_s(&buffer[0], buffer.size(), _TRUNCATE, fmt, args);
        buffer.resize(static_cast<size_t>(len));
        AsyncLogger::GetInstance().Log(buffer);
    }

    va_end(args);
#else
    (void)fmt;
#endif
}
