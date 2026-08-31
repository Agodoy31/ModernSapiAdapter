#include "pch.h"
#include "AsyncLogger.h"

#ifdef _DEBUG

namespace
{

[[nodiscard]] std::wstring ResolveLogFilePath()
{
    wil::unique_cotaskmem_string appDataPath;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
    {
        return {};
    }

    if (!appDataPath)
    {
        return {};
    }

    const std::wstring directoryPath = std::wstring(appDataPath.get()) + L"\\ModernSapiAdapter";
    CreateDirectoryW(directoryPath.c_str(), nullptr);
    return directoryPath + L"\\CoreEngine.log";
}

} // namespace

AsyncLogger* AsyncLogger::GetInstance() noexcept
{
    static AsyncLogger* instance = new (std::nothrow) AsyncLogger();
    return instance;
}

bool AsyncLogger::StartLocked() noexcept
{
    try
    {
        const std::wstring logPath = ResolveLogFilePath();
        if (logPath.empty())
        {
            return false;
        }

        m_file.clear();
        m_file.open(logPath, std::ios::out | std::ios::app);
        if (!m_file.is_open())
        {
            m_file.clear();
            return false;
        }

        m_stopRequested = false;
        try
        {
            m_worker = std::thread(&AsyncLogger::WorkerThread, this);
            m_state = State::Running;
            return true;
        }
        catch (...)
        {
            if (m_file.is_open())
            {
                m_file.close();
            }
            m_file.clear();
            return false;
        }
    }
    catch (...)
    {
        try
        {
            if (m_file.is_open())
            {
                m_file.close();
            }
            m_file.clear();
        }
        catch (...)
        {
        }
        return false;
    }
}

void AsyncLogger::Log(const std::wstring& message) noexcept
{
    try
    {
        SYSTEMTIME now = {};
        GetLocalTime(&now);

        wchar_t prefix[32] = {};
        swprintf_s(prefix, L"[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            now.wYear, now.wMonth, now.wDay,
            now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);

        std::wstring formattedMessage = std::wstring(prefix) + message;
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] {
            return m_state != State::Stopping;
        });

        if (m_state == State::StopFailed)
        {
            return;
        }

        if (m_state == State::Stopped && !StartLocked())
        {
            return;
        }

        if (m_state != State::Running)
        {
            return;
        }

        m_queue.push(std::move(formattedMessage));
        lock.unlock();
        m_cv.notify_one();
    }
    catch (...)
    {
    }
}

void AsyncLogger::WorkerThread() noexcept
{
    try
    {
        for (;;)
        {
            std::wstring message;
            {
                std::unique_lock lock(m_mutex);
                m_cv.wait(lock, [this] {
                    return m_stopRequested || !m_queue.empty();
                });

                if (m_queue.empty())
                {
                    if (m_stopRequested)
                    {
                        break;
                    }
                    continue;
                }

                message = std::move(m_queue.front());
                m_queue.pop();
            }

            try
            {
                if (m_file.is_open())
                {
                    m_file << message << L'\n';
                    m_file.flush();
                }
            }
            catch (...)
            {
            }
        }
    }
    catch (...)
    {
        try
        {
            std::lock_guard lock(m_mutex);
            if (m_state == State::Running)
            {
                m_state = State::StopFailed;
            }
        }
        catch (...)
        {
        }
        m_cv.notify_all();
    }

    try
    {
        if (m_file.is_open())
        {
            m_file.flush();
            m_file.close();
        }
    }
    catch (...)
    {
    }
}

bool AsyncLogger::Shutdown() noexcept
{
    try
    {
        std::unique_lock lock(m_mutex);
        m_cv.wait(lock, [this] {
            return m_state != State::Stopping;
        });

        if (m_state == State::Stopped)
        {
            return true;
        }

        const bool workerFailed = m_state == State::StopFailed;
        m_state = State::Stopping;
        m_stopRequested = true;
        lock.unlock();
        m_cv.notify_all();

        bool stopped = false;
        try
        {
            m_worker.join();
            stopped = true;
        }
        catch (...)
        {
        }

        lock.lock();
        if (workerFailed)
        {
            stopped = false;
        }
        if (!m_queue.empty())
        {
            stopped = false;
        }
        m_state = stopped ? State::Stopped : State::StopFailed;
        if (stopped)
        {
            m_stopRequested = false;
        }
        lock.unlock();
        m_cv.notify_all();
        return stopped;
    }
    catch (...)
    {
        return false;
    }
}

#endif // _DEBUG
