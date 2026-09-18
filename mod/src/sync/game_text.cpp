// The game's own texts -- bonfire and map names -- in the game's current language
// (0.2.2 points 6 and 12: tell the other player where a player went and which bonfire
// it lit).
//
// exe+0x503620(category, id) -> const wchar_t*: the text of `id` in the loaded
// text:/Text/<language>/<file>.fmg picked by `category` (the table at exe+0x1585E70:
// 5 MapName, 18 BonfireName). The travel menu and the bonfire menu title ask it with
// category 18 and the bare bonfire id (exe+0xD5884, exe+0xD71F3). It never returns
// null: "" for a missing text, " " for id 0. It is a plain read only once every text
// file has finished loading -- the 27 pointers at [repo+0x10+0x10*i] are all zero,
// repo = *(exe+0x1616CB0); otherwise it finishes the loads itself. So it is asked on
// the game thread, only when they are all zero, the result is copied at once and the
// pointer never kept. The overlay's font has Latin-1, Cyrillic and some punctuation;
// a name with anything else falls back to the caller's number.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/utils.h"

#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kGetText      = 0x503620;    // (category, id) -> const wchar_t*
constexpr uint32_t kTextRepo     = 0x1616CB0;   // *(exe+...) = the text repository
constexpr uint32_t kLanguage     = 0x1616CB8;   // *(exe+...) = the game's language index
constexpr int      kTextFiles    = 27;
constexpr int      kCategoryMap  = 5;
constexpr int      kCategoryBonfire = 18;

using GetTextFn = const wchar_t*(__fastcall*)(int32_t category, int32_t id);

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

// Copies the text into Out (up to Max-1 wide characters). False when the texts are
// still loading or the call threw.
bool ReadTextSafe(int32_t Category, int32_t Id, wchar_t* Out, int Max, int32_t* Language) {
    Out[0] = L'\0';
    __try {
        const uintptr_t Repo = *reinterpret_cast<const uintptr_t*>(ExeBase() + kTextRepo);
        if (!Repo) return false;
        for (int I = 0; I < kTextFiles; ++I) {
            if (*reinterpret_cast<const uintptr_t*>(Repo + 0x10 + 0x10 * I) != 0) return false;   // still loading
        }
        *Language = *reinterpret_cast<const int32_t*>(ExeBase() + kLanguage);
        const wchar_t* Text = reinterpret_cast<GetTextFn>(ExeBase() + kGetText)(Category, Id);
        if (!Text) return true;
        int N = 0;
        for (; N < Max - 1 && Text[N]; ++N) Out[N] = Text[N];
        Out[N] = L'\0';
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Out[0] = L'\0';
        return false;
    }
}

bool FontHasAll(const wchar_t* Text) {
    for (const wchar_t* P = Text; *P; ++P) {
        const wchar_t C = *P;
        const bool Ok = (C >= 0x20 && C <= 0xFF) || (C >= 0x400 && C <= 0x52F) || (C >= 0x2010 && C <= 0x2027) ||
                        C == 0x2039 || C == 0x203A || C == 0x2116;
        if (!Ok) return false;
    }
    return true;
}

std::string ToUtf8(const wchar_t* Text) {
    const int Bytes = WideCharToMultiByte(CP_UTF8, 0, Text, -1, nullptr, 0, nullptr, nullptr);
    if (Bytes <= 1) return {};
    std::string Out(static_cast<size_t>(Bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, Text, -1, Out.data(), Bytes, nullptr, nullptr);
    return Out;
}

std::mutex g_cacheMutex;
std::map<std::tuple<int32_t, int32_t, int32_t>, std::string> g_cache;

// The name, trimmed, or "" when there is none this font can show.
std::string LookUp(int32_t Category, int32_t Id) {
    if (Id <= 0) return {};
    wchar_t Buffer[128];
    int32_t Language = -1;
    if (!ReadTextSafe(Category, Id, Buffer, 128, &Language)) return {};
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        const auto It = g_cache.find({ Language, Category, Id });
        if (It != g_cache.end()) return It->second;
    }
    std::wstring Text(Buffer);
    while (!Text.empty() && (Text.back() == L' ' || Text.back() == L'\n' || Text.back() == L'\r')) Text.pop_back();
    while (!Text.empty() && Text.front() == L' ') Text.erase(Text.begin());
    std::string Name = (!Text.empty() && FontHasAll(Text.c_str())) ? ToUtf8(Text.c_str()) : std::string();
    std::lock_guard<std::mutex> Lock(g_cacheMutex);
    g_cache[{ Language, Category, Id }] = Name;
    return Name;
}

} // namespace

std::string GameBonfireName(int32_t BonfireId) {
    return LookUp(kCategoryBonfire, BonfireId);
}

std::string GameMapName(int32_t RawMap) {
    const uint32_t R = static_cast<uint32_t>(RawMap);
    const uint32_t Area = ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
    return LookUp(kCategoryMap, static_cast<int32_t>(Area));
}

} // namespace DS2Coop::Sync
