#pragma once
#include "pch.h"

#ifdef _DEBUG

#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <string>
#include <atomic>
#include <fstream>

class AsyncLogger
{
public:
    static AsyncLogger& GetInstance();
    
    void Log(const std::wstring& message);

    AsyncLogger(const AsyncLogger&) = delete;
    AsyncLogger& operator=(const AsyncLogger&) = delete;

private:
    AsyncLogger();
    ~AsyncLogger();

    void WorkerThread();

    std::queue<std::wstring> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::thread m_worker;
    std::atomic<bool> m_exit;
    std::wofstream m_file;
};

#endif // _DEBUG
