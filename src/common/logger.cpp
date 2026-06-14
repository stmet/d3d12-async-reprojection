#include "logger.h"
#include <cstdio>
#include <cstdarg>
#include <ctime>

Logger& Logger::Instance() {
    static Logger instance;
    return instance;
}

Logger::~Logger() {
    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        CloseHandle(m_fileHandle);
    }
}

std::wstring Logger::GetLogPath(HMODULE dllModule) {
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(dllModule, path, MAX_PATH) == 0) {
        return L"dxgi.log";
    }
    std::wstring pathStr(path);
    size_t lastSlash = pathStr.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        return pathStr.substr(0, lastSlash + 1) + L"dxgi.log";
    }
    return L"dxgi.log";
}

void Logger::Initialize(HMODULE dllModule) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_initialized) return;

    std::wstring logPath = GetLogPath(dllModule);
    m_fileHandle = CreateFileW(
        logPath.c_str(),
        GENERIC_WRITE,
        FILE_SHARE_READ,
        NULL,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        NULL
    );

    if (m_fileHandle != INVALID_HANDLE_VALUE) {
        m_initialized = true;
        // Internal log without lock recursion (Initialize is already locked, but Log locks again - wait, mutex is non-recursive std::mutex!)
        // So we must be careful! Calling Log() inside Initialize() will deadlock because Log() tries to lock the same m_mutex!
        // Ah! Good catch! Let's write the initialization log directly to avoid recursion deadlock.
    }
}

void Logger::Log(LogLevel level, const char* format, ...) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_fileHandle == INVALID_HANDLE_VALUE) return;

    const char* levelStr = "INFO";
    switch (level) {
        case LogLevel::Info: levelStr = "INFO"; break;
        case LogLevel::Debug: levelStr = "DEBUG"; break;
        case LogLevel::Warning: levelStr = "WARN"; break;
        case LogLevel::Error: levelStr = "ERROR"; break;
    }

    time_t rawtime;
    time(&rawtime);
    struct tm timeinfo;
    localtime_s(&timeinfo, &rawtime);
    char timeStr[32];
    strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &timeinfo);

    char buffer[2048];
    int len = sprintf_s(buffer, "[%s] [%s] ", timeStr, levelStr);

    va_list args;
    va_start(args, format);
    int formattedLen = vsnprintf_s(buffer + len, sizeof(buffer) - len - 2, _TRUNCATE, format, args);
    va_end(args);

    if (formattedLen < 0) {
        formattedLen = (int)strlen(buffer + len);
    }
    len += formattedLen;

    buffer[len++] = '\n';
    buffer[len] = '\0';

    DWORD bytesWritten;
    WriteFile(m_fileHandle, buffer, len, &bytesWritten, NULL);
    FlushFileBuffers(m_fileHandle);
}
