#include "../../include/utils.h"
#include <cstdarg>
#include <ctime>
#include <iomanip>
#include <Psapi.h>

using namespace DS2Coop::Utils;

Logger& Logger::GetInstance() {
    static Logger instance;
    return instance;
}

void Logger::Initialize(const std::wstring& logFilePath) {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_initialized) return;
    
    m_logFile.open(logFilePath, std::ios::out | std::ios::app);
    if (m_logFile.is_open()) {
        m_initialized = true;
        
        // Write header
        auto now = std::time(nullptr);
        auto tm = std::localtime(&now);
        m_logFile << "=== DS2 Seamless Co-op Log ===" << std::endl;
        m_logFile << "Started: " << std::put_time(tm, "%Y-%m-%d %H:%M:%S") << std::endl;
        m_logFile << "===============================" << std::endl;
        m_logFile.flush();
    }
}

void Logger::Shutdown() {
    std::lock_guard<std::mutex> lock(m_mutex);
    
    if (m_logFile.is_open()) {
        auto now = std::time(nullptr);
        auto tm = std::localtime(&now);
        m_logFile << "===============================" << std::endl;
        m_logFile << "Ended: " << std::put_time(tm, "%Y-%m-%d %H:%M:%S") << std::endl;
        m_logFile.close();
    }
    
    m_initialized = false;
}

void Logger::Log(LogLevel level, const char* format, ...) {
    if (!m_initialized || level < m_minLevel) return;
    
    std::lock_guard<std::mutex> lock(m_mutex);
    
    // Get timestamp
    auto now = std::time(nullptr);
    auto tm = std::localtime(&now);
    
    // Get level string
    const char* levelStr;
    switch (level) {
        case LogLevel::Debug:   levelStr = "DEBUG"; break;
        case LogLevel::Info:    levelStr = "INFO "; break;
        case LogLevel::Warning: levelStr = "WARN "; break;
        case LogLevel::Error:   levelStr = "ERROR"; break;
        default:                levelStr = "?????"; break;
    }
    
    // Format message
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    // Write to file
    if (m_logFile.is_open()) {
        m_logFile << "[" << std::put_time(tm, "%H:%M:%S") << "] "
                  << "[" << levelStr << "] "
                  << buffer << std::endl;
        m_logFile.flush();
    }
    
    // Also output to debugger if attached
    if (IsDebuggerPresent()) {
        char debugBuffer[4196];
        snprintf(debugBuffer, sizeof(debugBuffer), "[DS2Coop] [%s] %s\n", levelStr, buffer);
        OutputDebugStringA(debugBuffer);
    }
}

void Logger::LogDebug(const char* format, ...) {
    if (!m_initialized || LogLevel::Debug < m_minLevel) return;
    
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Log(LogLevel::Debug, "%s", buffer);
}

void Logger::LogInfo(const char* format, ...) {
    if (!m_initialized || LogLevel::Info < m_minLevel) return;
    
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Log(LogLevel::Info, "%s", buffer);
}

void Logger::LogWarning(const char* format, ...) {
    if (!m_initialized || LogLevel::Warning < m_minLevel) return;
    
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Log(LogLevel::Warning, "%s", buffer);
}

void Logger::LogError(const char* format, ...) {
    if (!m_initialized || LogLevel::Error < m_minLevel) return;
    
    va_list args;
    va_start(args, format);
    char buffer[4096];
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    
    Log(LogLevel::Error, "%s", buffer);
}

uintptr_t Memory::FindPattern(const char* module, const char* pattern, const char* mask) {
    uintptr_t base = GetModuleBase(module);
    if (!base) return 0;
    
    size_t size = GetModuleSize(module);
    return FindPattern(base, size, pattern, mask);
}

uintptr_t Memory::FindPattern(uintptr_t start, size_t size, const char* pattern, const char* mask) {
    size_t patternLen = strlen(mask);
    
    for (size_t i = 0; i < size - patternLen; i++) {
        bool found = true;
        for (size_t j = 0; j < patternLen; j++) {
            if (mask[j] != '?' && pattern[j] != *reinterpret_cast<char*>(start + i + j)) {
                found = false;
                break;
            }
        }
        if (found) {
            return start + i;
        }
    }
    
    return 0;
}

bool Memory::Protect(void* address, size_t size, DWORD protection, DWORD* oldProtection) {
    DWORD temp;
    return VirtualProtect(address, size, protection, oldProtection ? oldProtection : &temp) != 0;
}

bool Memory::Unprotect(void* address, size_t size) {
    DWORD oldProtect;
    return Protect(address, size, PAGE_EXECUTE_READWRITE, &oldProtect);
}

uintptr_t Memory::GetModuleBase(const char* moduleName) {
    HMODULE hModule = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
    return reinterpret_cast<uintptr_t>(hModule);
}

size_t Memory::GetModuleSize(const char* moduleName) {
    HMODULE hModule = moduleName ? GetModuleHandleA(moduleName) : GetModuleHandleA(nullptr);
    if (!hModule) return 0;
    
    MODULEINFO modInfo;
    if (GetModuleInformation(GetCurrentProcess(), hModule, &modInfo, sizeof(modInfo))) {
        return modInfo.SizeOfImage;
    }
    
    return 0;
}

