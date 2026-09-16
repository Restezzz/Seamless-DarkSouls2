#include "../../include/ui_settings.h"
#include "../../include/mod.h"
#include "../../include/utils.h"

#include <Windows.h>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cctype>

using namespace DS2Coop::Utils;

namespace DS2Coop::UI {

namespace {

std::atomic<int> g_language{ static_cast<int>(Language::English) };
std::atomic<int> g_menuKey{ VK_F1 };
std::atomic<int> g_menuSize{ 100 };

Language SystemLanguage() {
    return PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_RUSSIAN ? Language::Russian
                                                                     : Language::English;
}

struct NamedKey { int Vk; const char* Name; };

const NamedKey kNamedKeys[] = {
    { VK_INSERT, "Insert" },   { VK_DELETE, "Delete" },    { VK_HOME, "Home" },
    { VK_END, "End" },         { VK_PRIOR, "PageUp" },     { VK_NEXT, "PageDown" },
    { VK_PAUSE, "Pause" },     { VK_SCROLL, "ScrollLock" },{ VK_CAPITAL, "CapsLock" },
    { VK_NUMLOCK, "NumLock" }, { VK_TAB, "Tab" },          { VK_SPACE, "Space" },
    { VK_RETURN, "Enter" },    { VK_BACK, "Backspace" },   { VK_UP, "Up" },
    { VK_DOWN, "Down" },       { VK_LEFT, "Left" },        { VK_RIGHT, "Right" },
    { VK_OEM_3, "~" },         { VK_OEM_MINUS, "-" },      { VK_OEM_PLUS, "=" },
    { VK_OEM_4, "[" },         { VK_OEM_6, "]" },          { VK_OEM_5, "\\" },
    { VK_OEM_1, ";" },         { VK_OEM_7, "'" },          { VK_OEM_COMMA, "," },
    { VK_OEM_PERIOD, "." },    { VK_OEM_2, "/" },          { VK_MULTIPLY, "Num *" },
    { VK_ADD, "Num +" },       { VK_SUBTRACT, "Num -" },   { VK_DIVIDE, "Num /" },
    { VK_DECIMAL, "Num ." },
};

bool SameText(const std::string& a, const std::string& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++) {
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i]))) return false;
    }
    return true;
}

void Save() {
    SeamlessCoopMod::GetInstance().SetUiPreferences(
        GetLanguage() == Language::Russian ? "ru" : "en", KeyName(GetMenuKey()), GetMenuSize());
}

int ClampMenuSize(int percent) {
    return percent < kMinMenuSize ? kMinMenuSize : percent > kMaxMenuSize ? kMaxMenuSize : percent;
}

} // namespace

void InitUiSettings(const std::string& language, const std::string& menuKey, int menuSize) {
    Language Lang = SystemLanguage();
    if (SameText(language, "ru")) Lang = Language::Russian;
    else if (SameText(language, "en")) Lang = Language::English;
    g_language.store(static_cast<int>(Lang));

    int Vk = ParseKeyName(menuKey);
    if (Vk == 0 || WhyNotBindable(Vk) != nullptr) Vk = VK_F1;
    g_menuKey.store(Vk);

    g_menuSize.store(ClampMenuSize(menuSize));

    LOG_INFO("[UI] language %s, menu key %s, menu size %d%%",
             Lang == Language::Russian ? "ru" : "en", KeyName(Vk).c_str(), g_menuSize.load());
}

Language GetLanguage() { return static_cast<Language>(g_language.load()); }

void SetLanguage(Language lang) {
    g_language.store(static_cast<int>(lang));
    Save();
}

int GetMenuKey() { return g_menuKey.load(); }

void SetMenuKey(int vk) {
    g_menuKey.store(vk);
    Save();
    LOG_INFO("[UI] menu key is now %s", KeyName(vk).c_str());
}

int GetMenuSize() { return g_menuSize.load(); }

void SetMenuSize(int percent) {
    g_menuSize.store(ClampMenuSize(percent));
    Save();
    LOG_INFO("[UI] menu size is now %d%%", g_menuSize.load());
}

std::string Format(const char* fmt, ...) {
    char Buffer[512];
    va_list Args;
    va_start(Args, fmt);
    vsnprintf(Buffer, sizeof(Buffer), fmt, Args);
    va_end(Args);
    return Buffer;
}

std::string KeyName(int vk) {
    char Buffer[16];
    if (vk >= VK_F1 && vk <= VK_F24) {
        snprintf(Buffer, sizeof(Buffer), "F%d", vk - VK_F1 + 1);
        return Buffer;
    }
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
        snprintf(Buffer, sizeof(Buffer), "Num %d", vk - VK_NUMPAD0);
        return Buffer;
    }
    for (const NamedKey& K : kNamedKeys) {
        if (K.Vk == vk) return K.Name;
    }
    snprintf(Buffer, sizeof(Buffer), "Key %02X", vk);
    return Buffer;
}

int ParseKeyName(const std::string& name) {
    if (name.empty()) return 0;
    for (int Vk = 0x08; Vk <= 0xFE; Vk++) {
        if (SameText(KeyName(Vk), name)) return Vk;
    }
    return 0;
}

const char* WhyNotBindable(int vk) {
    switch (vk) {
    case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
    case VK_CANCEL:
        return Tr("Mouse buttons cannot open the menu.", "Кнопки мыши не подходят.");
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT:
    case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
    case VK_LWIN: case VK_RWIN: case VK_APPS:
        return Tr("Modifier keys cannot open the menu.", "Модификаторы (Shift, Ctrl, Alt) не подходят.");
    case VK_ESCAPE:
        return Tr("Esc belongs to the game.", "Esc занята игрой.");
    case VK_F12:
        return Tr("F12 takes Steam screenshots.", "F12 — скриншот в Steam.");
    case VK_SNAPSHOT:
        return Tr("Print Screen belongs to Windows.", "Print Screen занята Windows.");
    default:
        return nullptr;
    }
}

} // namespace DS2Coop::UI
