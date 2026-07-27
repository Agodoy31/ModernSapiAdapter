#include "pch.h"
#include "Logger.h"
#include <stdarg.h>

static std::mutex g_logMutex;
static std::ofstream g_logFile;

void LogInit() {
#ifdef _DEBUG
    std::lock_guard<std::mutex> lock(g_logMutex);
    
    PWSTR path = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
        std::filesystem::path dir = path;
        CoTaskMemFree(path);
        
        dir /= L"ModernSapiAdapter";
        dir /= L"Logs";
        
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        
        dir /= L"AzureEmbeddedSpeechProvider.log";
        g_logFile.open(dir, std::ios::app);
    }
#endif
}

void LogShutdown() {
#ifdef _DEBUG
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile.is_open()) {
        g_logFile.close();
    }
#endif
}

static void LogInternal(const char* level, const char* fmt, va_list args) {
#ifdef _DEBUG
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (!g_logFile.is_open()) return;

    auto now = std::chrono::system_clock::now();
    std::time_t time = std::chrono::system_clock::to_time_t(now);
    std::tm tm_buf;
    localtime_s(&tm_buf, &time);

    char buffer[2048];
    vsnprintf(buffer, sizeof(buffer), fmt, args);

    g_logFile << std::format("[{:04}-{:02}-{:02} {:02}:{:02}:{:02}] [{}] {}\n",
        tm_buf.tm_year + 1900, tm_buf.tm_mon + 1, tm_buf.tm_mday,
        tm_buf.tm_hour, tm_buf.tm_min, tm_buf.tm_sec,
        level, buffer);
    g_logFile.flush();
#else
    (void)level; (void)fmt; (void)args;
#endif
}

void LogInfo(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogInternal("INFO", fmt, args);
    va_end(args);
}

void LogWarn(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogInternal("WARN", fmt, args);
    va_end(args);
}

void LogError(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    LogInternal("ERROR", fmt, args);
    va_end(args);
}
