#pragma once

#include <string>

// Interface preferences: the menu language and the key that opens the menu.
//
// Both are read from the ini at startup, changed from the menu, and written
// back at once. Every place that shows the key asks KeyName(GetMenuKey()), so a
// rebind shows up in the hint, the notifications, the menu and the window title
// alike.
namespace DS2Coop::UI {

enum class Language { English, Russian };

// language: "auto" (follow Windows), "en" or "ru". menuKey: a key name, "F1".
void InitUiSettings(const std::string& language, const std::string& menuKey);

Language GetLanguage();
void     SetLanguage(Language lang);   // saves the ini
int      GetMenuKey();                 // virtual-key code
void     SetMenuKey(int vk);           // saves the ini

// The string for the current language. Both arguments are UTF-8.
inline const char* Tr(const char* en, const char* ru) {
    return GetLanguage() == Language::Russian ? ru : en;
}

// printf into a std::string; used to put names and keys into translated text.
std::string Format(const char* fmt, ...);

// "F1", "Insert", "Num 5", "~" and back. ParseKeyName returns 0 if unknown.
std::string KeyName(int vk);
int         ParseKeyName(const std::string& name);

// Why a key cannot open the menu (translated), or nullptr if it can.
const char* WhyNotBindable(int vk);

} // namespace DS2Coop::UI
