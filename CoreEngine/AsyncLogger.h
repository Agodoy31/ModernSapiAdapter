#pragma once
#include "pch.h"

#ifdef _DEBUG

class AsyncLogger
{
public:
    static AsyncLogger* GetInstance() noexcept;
    void Log(const std::wstring& message) noexcept;
    [[nodiscard]] bool Shutdown() noexcept;

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

private:
    enum class State
    {
        Stopped,
        Running,
        Stopping,
        StopFailed
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
    bool m_stopRequested = false;
    std::wofstream m_file;
};

#endif // _DEBUG
