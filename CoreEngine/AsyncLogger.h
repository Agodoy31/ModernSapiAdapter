#pragma once
#include "pch.h"

#ifdef _DEBUG

class AsyncLogger
{
public:
    static AsyncLogger* GetInstance() noexcept;
    void Log(const std::wstring& message) noexcept;
    [[nodiscard]] bool Shutdown() noexcept;
    [[nodiscard]] bool BeginUnloadQuiescence(DWORD timeoutMs) noexcept;
    void ResumeAfterUnloadRejected() noexcept;

#if defined(COREENGINE_TESTING)
    using WriteCallback = std::function<void(const std::wstring&)>;
    void SetWriteCallbackForTesting(WriteCallback callback) noexcept;
    [[nodiscard]] bool WaitForWorkerStoppedForTesting(DWORD timeoutMs) noexcept;
    static void SetLogFilePathForTesting(const std::wstring& path) noexcept;
#endif

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

private:
    enum class State
    {
        Stopped,
        Running,
        DrainingForUnload,
        StopFailed
    };

    enum class Admission
    {
        Open,
        Quiescent
    };

    AsyncLogger() noexcept = default;
    ~AsyncLogger() = default;

    [[nodiscard]] bool StartLocked() noexcept;
    void WorkerThread() noexcept;

    std::queue<std::wstring> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    State m_state = State::Stopped;
    Admission m_admission = Admission::Open;
    bool m_stopRequested = false;
    bool m_resumeWhenStopped = false;
    std::wofstream m_file;
#if defined(COREENGINE_TESTING)
    WriteCallback m_writeCallback;
#endif
};

#endif // _DEBUG
