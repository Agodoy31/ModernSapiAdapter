#include "pch.h"
#include "AsyncLogger.h"

#ifdef _DEBUG

AsyncLogger& AsyncLogger::GetInstance()
{
    static AsyncLogger instance;
    return instance;
}

AsyncLogger::AsyncLogger() : m_exit(false)
{
    PWSTR appDataPath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appDataPath)))
    {
        std::wstring logPath = std::wstring(appDataPath) + L"\\ModernSapiAdapter";
        CreateDirectoryW(logPath.c_str(), nullptr);
        logPath += L"\\CoreEngine.log";
        CoTaskMemFree(appDataPath);

        m_file.open(logPath, std::ios::out | std::ios::app);
    }

    m_worker = std::thread(&AsyncLogger::WorkerThread, this);
}

AsyncLogger::~AsyncLogger()
{
    m_exit = true;
    m_cv.notify_all();
    if (m_worker.joinable())
    {
        m_worker.join();
    }
}

void AsyncLogger::Log(const std::wstring& message)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(message);
    }
    m_cv.notify_one();
}

void AsyncLogger::WorkerThread()
{
    while (!m_exit)
    {
        std::wstring msg;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || m_exit; });

            if (m_queue.empty())
                continue;

            msg = std::move(m_queue.front());
            m_queue.pop();
        }

        if (m_file.is_open())
        {
            m_file << msg << std::endl;
            m_file.flush();
        }
    }

    // Flush remaining messages
    std::lock_guard<std::mutex> lock(m_mutex);
    while (!m_queue.empty() && m_file.is_open())
    {
        m_file << m_queue.front() << std::endl;
        m_queue.pop();
    }
}

#endif // _DEBUG
