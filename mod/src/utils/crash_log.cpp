// Crash log: when the game dies on an unhandled exception, where it died goes
// into ds2_seamless_coop.log and a minidump is written beside it.
//
// The friend's game crashed while loading into the host's world (12.09) and
// left nothing: no WER dump on that machine, and the mod log just stops. Now the
// log ends with the exception, the faulting module+offset, the registers and
// the unwound call stack, and ds2_seamless_crash_<time>.dmp holds the rest.
//
// The work happens on a thread made up front. The crashing thread may be out of
// stack (a stack overflow) or hold the heap or the logger's lock, so all it
// does is hand its EXCEPTION_POINTERS over and wait.

#include "../../include/utils.h"
#include "../../include/hooks.h"
#include <DbgHelp.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>

namespace {

using WriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION,
                                  PMINIDUMP_USER_STREAM_INFORMATION, PMINIDUMP_CALLBACK_INFORMATION);
using SetFilterFn = LPTOP_LEVEL_EXCEPTION_FILTER(WINAPI*)(LPTOP_LEVEL_EXCEPTION_FILTER);

constexpr DWORD kWriterWaitMs = 20000;  // a crashing thread stops waiting after this
constexpr int kMaxFrames = 40;
constexpr int kScanQwords = 1024;
constexpr int kMaxScanHits = 24;
constexpr int kScanHitsPerLine = 6;

// Code sections of a module whose return addresses the raw stack scan picks out.
struct CodeRange {
    uintptr_t ModuleBase;
    uintptr_t Start;
    uintptr_t End;
    const char* Tag;
};

wchar_t g_logPath[MAX_PATH];
wchar_t g_dumpDir[MAX_PATH];
CodeRange g_ranges[2];
WriteDumpFn g_writeDump = nullptr;
HANDLE g_crashEvent = nullptr;  // crashing thread -> writer
HANDLE g_doneEvent = nullptr;   // writer -> crashing threads
EXCEPTION_POINTERS* g_crashInfo = nullptr;
DWORD g_crashThread = 0;
std::atomic<DWORD> g_writerThread{ 0 };
std::atomic<bool> g_crashing{ false };
std::atomic<LPTOP_LEVEL_EXCEPTION_FILTER> g_nextFilter{ nullptr };  // anyone else's, run after ours
SetFilterFn g_origSetFilter = nullptr;
thread_local bool t_inFilter = false;

// Appends a line to the mod log through a handle of its own: the logger's
// mutex may be held by the thread that crashed.
void Line(HANDLE file, const char* format, ...) {
    char text[1024];
    SYSTEMTIME t;
    GetLocalTime(&t);
    int length = snprintf(text, sizeof(text), "[%02u:%02u:%02u] [ERROR] [CRASH] ", t.wHour, t.wMinute, t.wSecond);
    const int room = static_cast<int>(sizeof(text)) - length - 2;  // keep two for CRLF
    va_list args;
    va_start(args, format);
    const int written = vsnprintf(text + length, room, format, args);
    va_end(args);
    if (written < 0) return;
    length += (written < room) ? written : room - 1;
    text[length++] = '\r';
    text[length++] = '\n';
    DWORD done = 0;
    WriteFile(file, text, static_cast<DWORD>(length), &done, nullptr);
}

// "DarkSoulsII.exe+0x5177C0" for an address inside a loaded module.
void Where(uintptr_t address, char* out, size_t size) {
    HMODULE module = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(address), &module) && module) {
        wchar_t path[MAX_PATH] = L"?";
        const DWORD length = GetModuleFileNameW(module, path, MAX_PATH);
        const wchar_t* name = path;
        for (DWORD i = 0; i < length; ++i) {
            if (path[i] == L'\\' || path[i] == L'/') name = path + i + 1;
        }
        snprintf(out, size, "%ls+0x%llX", name,
                 static_cast<unsigned long long>(address - reinterpret_cast<uintptr_t>(module)));
        return;
    }
    snprintf(out, size, "0x%llX", static_cast<unsigned long long>(address));
}

bool ReadQword(DWORD64 address, DWORD64& value) {
    __try {
        value = *reinterpret_cast<const DWORD64*>(address);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One frame up, with the unwind tables every x64 module carries (.pdata), the
// way a debugger does it. False when the stack cannot be followed further.
bool Unwind(CONTEXT& context) {
    const DWORD64 pc = context.Rip;
    const DWORD64 sp = context.Rsp;
    __try {
        DWORD64 imageBase = 0;
        PRUNTIME_FUNCTION function = RtlLookupFunctionEntry(pc, &imageBase, nullptr);
        if (function) {
            PVOID handlerData = nullptr;
            DWORD64 establisherFrame = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, imageBase, pc, function, &context, &handlerData,
                             &establisherFrame, nullptr);
        } else {
            // No unwind data: a leaf function, or a call through a bad pointer
            // (rip 0). Either way the return address is on top of the stack.
            context.Rip = *reinterpret_cast<const DWORD64*>(sp);
            context.Rsp = sp + 8;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return context.Rsp > sp;  // a stack that does not move is a loop
}

void CallStack(HANDLE file, const CONTEXT& start) {
    CONTEXT context = start;
    for (int frame = 0; frame < kMaxFrames && context.Rip; ++frame) {
        char where[MAX_PATH + 32];
        Where(static_cast<uintptr_t>(context.Rip), where, sizeof(where));
        Line(file, "  #%02d %s", frame, where);
        if (!Unwind(context)) break;
    }
}

// A raw look down the stack for return addresses into the game or the mod, in
// case the unwind was derailed (a corrupted frame, a hook trampoline).
void StackScan(HANDLE file, DWORD64 sp) {
    char text[512];
    int length = 0;
    int hits = 0;
    for (int i = 0; i < kScanQwords && hits < kMaxScanHits; ++i) {
        DWORD64 value = 0;
        if (!ReadQword(sp + static_cast<DWORD64>(i) * 8, value)) break;
        for (const CodeRange& range : g_ranges) {
            if (!range.Start || value < range.Start || value >= range.End) continue;
            length += snprintf(text + length, sizeof(text) - length, " %s+0x%llX", range.Tag,
                               static_cast<unsigned long long>(value - range.ModuleBase));
            if (++hits % kScanHitsPerLine == 0) {
                Line(file, "  scan:%s", text);
                length = 0;
            }
            break;
        }
    }
    if (length) Line(file, "  scan:%s", text);
}

void Describe(HANDLE file, const EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* record = info->ExceptionRecord;
    const CONTEXT& c = *info->ContextRecord;
    char where[MAX_PATH + 32];
    Where(reinterpret_cast<uintptr_t>(record->ExceptionAddress), where, sizeof(where));
    Line(file, "==== the game crashed: exception 0x%08lX at %s, thread %lu ====", record->ExceptionCode, where,
         g_crashThread);
    if ((record->ExceptionCode == EXCEPTION_ACCESS_VIOLATION || record->ExceptionCode == EXCEPTION_IN_PAGE_ERROR) &&
        record->NumberParameters >= 2) {
        const ULONG_PTR op = record->ExceptionInformation[0];
        Line(file, "%s of address 0x%llX", op == 0 ? "read" : op == 1 ? "write" : op == 8 ? "execute" : "access",
             static_cast<unsigned long long>(record->ExceptionInformation[1]));
    }
    Line(file, "rax=%016llX rbx=%016llX rcx=%016llX rdx=%016llX", c.Rax, c.Rbx, c.Rcx, c.Rdx);
    Line(file, "rsi=%016llX rdi=%016llX rbp=%016llX rsp=%016llX", c.Rsi, c.Rdi, c.Rbp, c.Rsp);
    Line(file, "r8 =%016llX r9 =%016llX r10=%016llX r11=%016llX", c.R8, c.R9, c.R10, c.R11);
    Line(file, "r12=%016llX r13=%016llX r14=%016llX r15=%016llX", c.R12, c.R13, c.R14, c.R15);
    Line(file, "call stack:");
    CallStack(file, c);
    StackScan(file, c.Rsp);
}

// 0 on success, otherwise the error.
DWORD WriteDump(EXCEPTION_POINTERS* info, wchar_t (&path)[MAX_PATH]) {
    if (!g_writeDump) return ERROR_PROC_NOT_FOUND;
    SYSTEMTIME t;
    GetLocalTime(&t);
    swprintf_s(path, L"%lsds2_seamless_crash_%04u%02u%02u_%02u%02u%02u.dmp", g_dumpDir, t.wYear, t.wMonth, t.wDay,
               t.wHour, t.wMinute, t.wSecond);
    HANDLE file = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return GetLastError();
    DWORD error = ERROR_GEN_FAILURE;
    __try {
        MINIDUMP_EXCEPTION_INFORMATION exception{ g_crashThread, info, FALSE };
        const auto type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory |
                                                     MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
        if (g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), file, type, &exception, nullptr, nullptr)) {
            error = 0;
        } else if (const DWORD last = GetLastError()) {
            error = last;
        }
    } __finally {
        CloseHandle(file);  // also when the dump writer itself faults
    }
    return error;
}

void Report(EXCEPTION_POINTERS* info) {
    HANDLE file = CreateFileW(g_logPath, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    const bool haveLog = file != INVALID_HANDLE_VALUE;
    __try {
        if (haveLog) Describe(file, info);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        if (haveLog) Line(file, "(the report itself faulted here)");
    }
    wchar_t dumpPath[MAX_PATH] = L"";
    DWORD dumpError = ERROR_GEN_FAILURE;
    __try {
        dumpError = WriteDump(info, dumpPath);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (!haveLog) return;
    if (dumpError == 0) Line(file, "minidump: %ls", dumpPath);
    else Line(file, "minidump failed (error %lu)", dumpError);
    CloseHandle(file);
}

DWORD WINAPI Writer(LPVOID) {
    g_writerThread.store(GetCurrentThreadId());
    // Loaded now rather than at crash time, when the loader lock may be taken.
    wchar_t dbghelp[MAX_PATH];
    if (GetSystemDirectoryW(dbghelp, MAX_PATH) && wcscat_s(dbghelp, L"\\dbghelp.dll") == 0) {
        if (HMODULE module = LoadLibraryW(dbghelp)) {
            g_writeDump = reinterpret_cast<WriteDumpFn>(GetProcAddress(module, "MiniDumpWriteDump"));
        }
    }
    WaitForSingleObject(g_crashEvent, INFINITE);
    Report(g_crashInfo);
    SetEvent(g_doneEvent);
    return 0;
}

LONG WINAPI CrashFilter(EXCEPTION_POINTERS* info) {
    // A chained filter that got ours as its "previous" calls back in: pass.
    if (t_inFilter) return EXCEPTION_CONTINUE_SEARCH;
    t_inFilter = true;
    // One report per process. A thread that crashes while it is being written
    // waits for it too -- a corrupted heap often takes several threads down at
    // once, and going straight on could end the process mid-report -- unless
    // that thread is the writer itself.
    if (!g_crashing.exchange(true)) {
        g_crashInfo = info;
        g_crashThread = GetCurrentThreadId();
        SetEvent(g_crashEvent);
        WaitForSingleObject(g_doneEvent, kWriterWaitMs);
    } else if (GetCurrentThreadId() != g_writerThread.load()) {
        WaitForSingleObject(g_doneEvent, kWriterWaitMs);
    }
    const LPTOP_LEVEL_EXCEPTION_FILTER next = g_nextFilter.load();
    const LONG result = next ? next(info) : EXCEPTION_CONTINUE_SEARCH;
    t_inFilter = false;
    return result;
}

// Keeps ours first: a filter installed later by the game or a library runs
// after ours instead of replacing it.
LPTOP_LEVEL_EXCEPTION_FILTER WINAPI SetFilterDetour(LPTOP_LEVEL_EXCEPTION_FILTER filter) {
    if (filter == CrashFilter) return g_origSetFilter(filter);
    return g_nextFilter.exchange(filter);
}

CodeRange CodeOf(HMODULE module, const char* tag) {
    CodeRange range{ 0, 0, 0, tag };
    if (!module) return range;
    const auto base = reinterpret_cast<uintptr_t>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const IMAGE_SECTION_HEADER* section = IMAGE_FIRST_SECTION(nt);
    range.ModuleBase = base;
    for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i, ++section) {
        if (!(section->Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t start = base + section->VirtualAddress;
        const uintptr_t end = start + section->Misc.VirtualSize;
        if (!range.Start || start < range.Start) range.Start = start;
        if (end > range.End) range.End = end;
    }
    return range;
}

}  // namespace

namespace DS2Coop::Utils {

void InstallCrashLog() {
    if (g_crashEvent) return;
    const DWORD length = GetFullPathNameW(L"ds2_seamless_coop.log", MAX_PATH, g_logPath, nullptr);
    if (!length || length >= MAX_PATH) {
        LOG_WARNING("[CRASH] no path for the log -- crash log off");
        return;
    }
    wcscpy_s(g_dumpDir, g_logPath);
    if (wchar_t* slash = wcsrchr(g_dumpDir, L'\\')) slash[1] = L'\0';
    else g_dumpDir[0] = L'\0';

    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&InstallCrashLog), &self);
    g_ranges[0] = CodeOf(GetModuleHandleW(nullptr), "exe");
    g_ranges[1] = CodeOf(self, "mod");

    g_crashEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_doneEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HANDLE writer = (g_crashEvent && g_doneEvent) ? CreateThread(nullptr, 0, Writer, nullptr, 0, nullptr) : nullptr;
    if (!writer) {
        LOG_WARNING("[CRASH] could not start the crash writer (error %lu) -- crash log off", GetLastError());
        if (g_crashEvent) CloseHandle(g_crashEvent);
        if (g_doneEvent) CloseHandle(g_doneEvent);
        g_crashEvent = g_doneEvent = nullptr;
        return;
    }
    CloseHandle(writer);
    g_nextFilter.store(SetUnhandledExceptionFilter(CrashFilter));
    LOG_INFO("[CRASH] crash filter installed");
}

void GuardCrashLog() {
    if (!g_crashEvent || g_origSetFilter) return;
    HMODULE kernelBase = GetModuleHandleW(L"kernelbase.dll");
    void* target = kernelBase ? reinterpret_cast<void*>(GetProcAddress(kernelBase, "SetUnhandledExceptionFilter"))
                              : nullptr;
    if (!target) {
        LOG_WARNING("[CRASH] SetUnhandledExceptionFilter not found -- the crash filter is unguarded");
        return;
    }
    // Take the top spot back in case something replaced ours since start-up.
    const LPTOP_LEVEL_EXCEPTION_FILTER current = SetUnhandledExceptionFilter(CrashFilter);
    if (current && current != CrashFilter) g_nextFilter.store(current);
    if (!Hooks::HookManager::GetInstance().InstallHook(target, reinterpret_cast<void*>(&SetFilterDetour),
                                                       reinterpret_cast<void**>(&g_origSetFilter))) {
        LOG_WARNING("[CRASH] could not guard the crash filter -- a later one may replace it");
        return;
    }
    LOG_INFO("[CRASH] crash log armed: report into this log, dump into %ls", g_dumpDir);
}

}  // namespace DS2Coop::Utils
