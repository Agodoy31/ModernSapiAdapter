#pragma once

#ifndef COREENGINE_TESTING
#define COREENGINE_TESTING
#endif

#include "../CoreEngine/AsyncLogger.h"

#if defined(_DEBUG)

class AsyncLoggerTestAccess final
{
  public:
    using WriteCallback = std::function<void(const std::wstring &)>;

    static void SetWriteCallback(AsyncLogger &logger, WriteCallback callback)
    {
        logger.SetWriteCallbackForTesting(std::move(callback));
    }

    [[nodiscard]] static bool WaitForWorkerStopped(AsyncLogger &logger, DWORD timeoutMs) noexcept
    {
        return logger.WaitForWorkerStoppedForTesting(timeoutMs);
    }

    static void SetLogFilePath(const std::wstring &path) noexcept
    {
        AsyncLogger::SetLogFilePathForTesting(path);
    }

  private:
    AsyncLoggerTestAccess() = delete;
};

#endif // _DEBUG
