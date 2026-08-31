#include "pch.h"
#include "AsyncLogger.h"

void CoreLog(const wchar_t* fmt, ...) noexcept
{
#ifdef _DEBUG
    va_list args;
    va_start(args, fmt);

    try
    {
        va_list argsCopy;
        va_copy(argsCopy, args);
        const int len = _vscwprintf(fmt, argsCopy);
        va_end(argsCopy);

        if (len > 0)
        {
            std::wstring buffer(static_cast<size_t>(len) + 1, L'\0');
            _vsnwprintf_s(buffer.data(), buffer.size(), _TRUNCATE, fmt, args);
            buffer.resize(static_cast<size_t>(len));
            if (auto* logger = AsyncLogger::GetInstance())
            {
                logger->Log(buffer);
            }
        }
    }
    catch (...)
    {
    }

    va_end(args);
#else
    (void)fmt;
#endif
}
