#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <mutex>

enum class LogLevel {
    Info,
    Debug,
    Warning,
    Error
};

class Logger {
public:
    static Logger& Instance();
    void Initialize(HMODULE dllModule);
    void Log(LogLevel level, const char* format, ...);

private:
    Logger() = default;
    ~Logger();
    
    std::wstring GetLogPath(HMODULE dllModule);

    HANDLE m_fileHandle = INVALID_HANDLE_VALUE;
    std::mutex m_mutex;
    bool m_initialized = false;
};

#define LOG_INFO(format, ...) Logger::Instance().Log(LogLevel::Info, format, ##__VA_ARGS__)
#define LOG_DEBUG(format, ...) Logger::Instance().Log(LogLevel::Debug, format, ##__VA_ARGS__)
#define LOG_WARN(format, ...) Logger::Instance().Log(LogLevel::Warning, format, ##__VA_ARGS__)
#define LOG_ERROR(format, ...) Logger::Instance().Log(LogLevel::Error, format, ##__VA_ARGS__)
