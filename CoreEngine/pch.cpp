#include "pch.h"
#include "AsyncLogger.h"

void CoreLog(const wchar_t* fmt, ...)
{
#ifdef _DEBUG
    va_list args;
    va_start(args, fmt);
    
    int len = _vscwprintf(fmt, args);
    if (len > 0)
    {
        std::wstring buffer(len, L'\0');
        _vsnwprintf_s(&buffer[0], len + 1, _TRUNCATE, fmt, args);
        AsyncLogger::GetInstance().Log(buffer);
    }
    
    va_end(args);
#else
    (void)fmt;
#endif
}
