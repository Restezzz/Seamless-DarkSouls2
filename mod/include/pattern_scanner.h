#pragma once

#include <Windows.h>
#include <cstdint>
#include <vector>

namespace DS2Coop {
namespace Utils {

class PatternScanner {
public:
    /**
     * Scan for a byte pattern in the game's memory
     * 
     * @param pattern - byte pattern to search for (e.g., "\x48\x8B\x05\x00\x00\x00\x00")
     * @param mask - mask string (e.g., "xxx????") where 'x' = match, '?' = wildcard
     * @param module_name - name of the module to scan (nullptr = current process)
     * @return Address of the first match, or 0 if not found
     */
    static uintptr_t FindPattern(const char* pattern, const char* mask, const char* module_name = nullptr);
    
    /**
     * Scan for multiple patterns and return all matches
     */
    static std::vector<uintptr_t> FindPatternAll(const char* pattern, const char* mask, const char* module_name = nullptr);
    
    /**
     * Get module base address and size
     */
    static bool GetModuleInfo(const char* module_name, uintptr_t& base, size_t& size);
    
    /**
     * Resolve a RIP-relative address (x64 instruction pointer relative addressing)
     * 
     * @param instruction_addr - Address of the instruction
     * @param offset - Offset within the instruction where the RIP-relative value is
     * @param instruction_size - Size of the entire instruction
     * @return Absolute address
     */
    static uintptr_t ResolveRIP(uintptr_t instruction_addr, int offset, int instruction_size);
    
    /**
     * Read a pointer from memory safely
     */
    static bool ReadPointer(uintptr_t address, uintptr_t& out_value);

private:
    static bool ComparePattern(const uint8_t* data, const char* pattern, const char* mask);
};

} // namespace Utils
} // namespace DS2Coop

