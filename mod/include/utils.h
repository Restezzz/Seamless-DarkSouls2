#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <string>
#include <fstream>
#include <mutex>

namespace DS2Coop::Utils {

// Logging utility
enum class LogLevel {
    Debug,
    Info,
    Warning,
    Error
};

class Logger {
public:
    static Logger& GetInstance();
    
    void Initialize(const std::wstring& logFilePath);
    void Shutdown();
    
    void Log(LogLevel level, const char* format, ...);
    void LogDebug(const char* format, ...);
    void LogInfo(const char* format, ...);
    void LogWarning(const char* format, ...);
    void LogError(const char* format, ...);
    
    void SetMinLevel(LogLevel level) { m_minLevel = level; }

private:
    Logger() = default;
    ~Logger() = default;
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;
    
    std::ofstream m_logFile;
    std::mutex m_mutex;
    LogLevel m_minLevel = LogLevel::Info;
    bool m_initialized = false;
};

// Memory utility functions
class Memory {
public:
    // Pattern scanning
    static uintptr_t FindPattern(const char* module, const char* pattern, const char* mask);
    static uintptr_t FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask);
    
    // Memory protection
    static bool Protect(void* address, size_t size, DWORD protection, DWORD* oldProtection = nullptr);
    static bool Unprotect(void* address, size_t size);
    
    // Memory reading/writing
    template<typename T>
    static bool Read(uintptr_t address, T* value) {
        if (!address || !value) return false;
        __try {
            *value = *reinterpret_cast<T*>(address);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            return false;
        }
    }
    
    template<typename T>
    static bool Write(uintptr_t address, const T& value) {
        if (!address) return false;
        DWORD oldProtect;
        if (!Protect(reinterpret_cast<void*>(address), sizeof(T), PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        
        __try {
            *reinterpret_cast<T*>(address) = value;
            Protect(reinterpret_cast<void*>(address), sizeof(T), oldProtect);
            return true;
        }
        __except(EXCEPTION_EXECUTE_HANDLER) {
            Protect(reinterpret_cast<void*>(address), sizeof(T), oldProtect);
            return false;
        }
    }
    
    // Get module information
    static uintptr_t GetModuleBase(const char* moduleName = nullptr);
    static size_t GetModuleSize(const char* moduleName = nullptr);
};

// Crash log (utils/crash_log.cpp): on an unhandled exception the faulting
// address, registers and call stack go into the mod log, a minidump beside it.
void InstallCrashLog();  // early, off the loader lock
void GuardCrashLog();    // once hooks are up: later filters chain behind ours

} // namespace DS2Coop::Utils

// Logging macros
#define LOG_DEBUG(...) DS2Coop::Utils::Logger::GetInstance().LogDebug(__VA_ARGS__)
#define LOG_INFO(...) DS2Coop::Utils::Logger::GetInstance().LogInfo(__VA_ARGS__)
#define LOG_WARNING(...) DS2Coop::Utils::Logger::GetInstance().LogWarning(__VA_ARGS__)
#define LOG_ERROR(...) DS2Coop::Utils::Logger::GetInstance().LogError(__VA_ARGS__)

