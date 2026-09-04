#include "pch.h"
#include "AsyncLogger.h"

#ifdef _DEBUG

#if defined(COREENGINE_TESTING)
static std::wstring g_testLogFilePath;

void AsyncLogger::SetLogFilePathForTesting(const std::wstring& path)
{
    g_testLogFilePath = path;
}
#endif

namespace
{

[[nodiscard]] std::wstring ResolveLogFilePath()
{
#if defined(COREENGINE_TESTING)
    if (!g_testLogFilePath.empty())
    {
        return g_testLogFilePath;
    }
#endif
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
        if (m_admission != Admission::Open || m_state == State::StopFailed)
        {
            return;
        }

        if (m_state == State::Stopped && m_worker.joinable())
        {
            std::thread completedWorker = std::move(m_worker);
            lock.unlock();
            completedWorker.join();
            lock.lock();

            if (m_admission != Admission::Open || m_state == State::StopFailed)
            {
                return;
            }
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
#if defined(COREENGINE_TESTING)
                WriteCallback writeCallback;
                {
                    std::lock_guard lock(m_mutex);
                    writeCallback = m_writeCallback;
                }
                if (writeCallback)
                {
                    writeCallback(message);
                }
                else
#endif
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
            if (m_state == State::Running || m_state == State::DrainingForUnload)
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

    try
    {
        std::lock_guard lock(m_mutex);
        if (m_state == State::DrainingForUnload)
        {
            m_state = State::Stopped;
            if (m_resumeWhenStopped)
            {
                m_resumeWhenStopped = false;
                m_stopRequested = false;
                m_admission = Admission::Open;
            }
        }
    }
    catch (...)
    {
    }
    m_cv.notify_all();
}

bool AsyncLogger::Shutdown() noexcept
{
    if (!BeginUnloadQuiescence(INFINITE))
    {
        return false;
    }

    ResumeAfterUnloadRejected();
    return true;
}

bool AsyncLogger::BeginUnloadQuiescence(const DWORD timeoutMs) noexcept
{
    std::unique_lock lock(m_mutex);
    m_admission = Admission::Quiescent;
    m_resumeWhenStopped = false;
    if (m_state == State::StopFailed)
    {
        return false;
    }

    if (m_state == State::Running)
    {
        m_state = State::DrainingForUnload;
        m_stopRequested = true;
        lock.unlock();
        m_cv.notify_all();
        lock.lock();
    }

    const auto stopped = [this] { return m_state == State::Stopped || m_state == State::StopFailed; };
    if (!stopped() && !m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), stopped))
    {
        m_resumeWhenStopped = true;
        return false;
    }

    if (m_state != State::Stopped)
    {
        return false;
    }

    std::thread completedWorker = std::move(m_worker);
    lock.unlock();
    if (completedWorker.joinable())
    {
        completedWorker.join();
    }
    return true;
}

void AsyncLogger::ResumeAfterUnloadRejected() noexcept
{
    std::unique_lock lock(m_mutex);
    if (m_state != State::Stopped)
    {
        return;
    }

    std::thread completedWorker = std::move(m_worker);
    lock.unlock();
    if (completedWorker.joinable())
    {
        completedWorker.join();
    }
    lock.lock();
    m_stopRequested = false;
    m_resumeWhenStopped = false;
    m_admission = Admission::Open;
    lock.unlock();
    m_cv.notify_all();
}

#if defined(COREENGINE_TESTING)
void AsyncLogger::SetWriteCallbackForTesting(WriteCallback callback) noexcept
{
    std::lock_guard lock(m_mutex);
    m_writeCallback = std::move(callback);
}

bool AsyncLogger::WaitForWorkerStoppedForTesting(const DWORD timeoutMs) noexcept
{
    std::unique_lock lock(m_mutex);
    return m_cv.wait_for(lock, std::chrono::milliseconds(timeoutMs), [this] {
        return m_state == State::Stopped;
    });
}
#endif

#endif // _DEBUG
