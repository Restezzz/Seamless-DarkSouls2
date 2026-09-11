// Network Hooks - Winsock interception + Server redirect
//
// Two functions:
// 1. Hook Winsock connect() to redirect game connections from FromSoft → custom server
// 2. Patch hostname + RSA key in game memory so the game resolves to our server

// WinSock2 MUST be included before Windows.h
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>

#pragma comment(lib, "ws2_32.lib")

#include <Psapi.h>
#pragma comment(lib, "psapi.lib")

#include "../../include/hooks.h"
#include "../../include/addresses.h"
#include "../../include/utils.h"
#include <fstream>
#include <string>
#include <vector>
#include <algorithm>

using namespace DS2Coop::Hooks;
using namespace DS2Coop::Utils;
using namespace DS2Coop::Addresses;

// ============================================================================
// Winsock redirect state
// ============================================================================
static int (WSAAPI* g_originalConnect)(SOCKET s, const sockaddr* name, int namelen) = nullptr;
static bool g_gameOnline = false;
static bool g_redirectActive = false;
static std::string g_redirectIP = "127.0.0.1";
static uint16_t g_redirectPort = 50031;

// ============================================================================
// Hooked Winsock connect() — redirects FromSoft server to custom server
// ============================================================================
static int WSAAPI ConnectHook(SOCKET s, const sockaddr* name, int namelen) {
    if (name && name->sa_family == AF_INET) {
        sockaddr_in* addr = const_cast<sockaddr_in*>(reinterpret_cast<const sockaddr_in*>(name));
        uint16_t port = ntohs(addr->sin_port);

        char ipStr[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &addr->sin_addr, ipStr, sizeof(ipStr));

        LOG_INFO("[NET] Game connecting to %s:%u", ipStr, port);

        // Redirect all game server connections (login=50031, auth=50000, game=50010+)
        if (g_redirectActive && (port == DS2_LOGIN_PORT || port == 50000 ||
            (port >= 50010 && port <= 50100))) {
            LOG_INFO("[NET] REDIRECTING %s:%u to custom server %s:%u (keeping port)",
                     ipStr, port, g_redirectIP.c_str(), port);

            // Rewrite destination IP only — keep the port the same
            // The server listens on all these ports locally
            inet_pton(AF_INET, g_redirectIP.c_str(), &addr->sin_addr);

            char newIp[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &addr->sin_addr, newIp, sizeof(newIp));
            LOG_INFO("[NET] Connection redirected to %s:%u", newIp, port);

            g_gameOnline = true;
        } else if (port == DS2_LOGIN_PORT) {
            LOG_INFO("[NET] Detected DS2 login server connection (port %u) — redirect OFF", DS2_LOGIN_PORT);
            g_gameOnline = true;
        }
    }

    return g_originalConnect(s, name, namelen);
}

// ============================================================================
// WinsockHooks public interface
// ============================================================================
void WinsockHooks::SetServerRedirect(const std::string& ip, uint16_t port) {
    g_redirectIP = ip;
    g_redirectPort = port;
    g_redirectActive = true;
    LOG_INFO("[NET] Server redirect configured: %s:%u", ip.c_str(), port);
}

bool WinsockHooks::IsRedirectActive() {
    return g_redirectActive;
}

bool WinsockHooks::InstallHooks() {
    LOG_INFO("Installing Winsock hooks...");

    HMODULE ws2 = GetModuleHandleA("ws2_32.dll");
    if (!ws2) {
        ws2 = LoadLibraryA("ws2_32.dll");
    }

    if (!ws2) {
        LOG_WARNING("ws2_32.dll not loaded yet");
        return true;
    }

    void* connectAddr = GetProcAddress(ws2, "connect");
    if (!connectAddr) {
        LOG_WARNING("Could not find connect() in ws2_32.dll");
        return true;
    }

    if (HookManager::GetInstance().InstallHook(
        connectAddr,
        reinterpret_cast<void*>(&ConnectHook),
        reinterpret_cast<void**>(&g_originalConnect)
    )) {
        LOG_INFO("  HOOKED Winsock connect()");
        return true;
    }

    LOG_WARNING("  Failed to hook connect() (non-critical)");
    return true;
}

void WinsockHooks::UninstallHooks() {
    LOG_INFO("Uninstalling Winsock hooks...");
}

// ============================================================================
// Server Redirect — Hostname + RSA key patching in game memory
//
// Adapted from ds3os DS2_ReplaceServerAddressHook.cpp
// DS2's hostname is NOT encrypted (unlike DS3), so we can patch directly.
// ============================================================================

// Search game memory for a wide string
static std::vector<uintptr_t> SearchWideString(const wchar_t* needle) {
    std::vector<uintptr_t> results;

    HMODULE gameModule = GetModuleHandleA("DarkSoulsII.exe");
    if (!gameModule) {
        LOG_ERROR("[REDIRECT] DarkSoulsII.exe module not found");
        return results;
    }

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));

    uintptr_t base = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
    size_t moduleSize = modInfo.SizeOfImage;
    size_t needleLen = wcslen(needle);
    size_t needleBytes = needleLen * sizeof(wchar_t);

    if (needleBytes >= moduleSize) return results;

    for (size_t i = 0; i < moduleSize - needleBytes; i++) {
        if (memcmp(reinterpret_cast<void*>(base + i), needle, needleBytes) == 0) {
            results.push_back(base + i);
        }
    }

    return results;
}

// Search game memory for an ASCII string
static std::vector<uintptr_t> SearchAsciiString(const char* needle) {
    std::vector<uintptr_t> results;

    HMODULE gameModule = GetModuleHandleA("DarkSoulsII.exe");
    if (!gameModule) {
        LOG_ERROR("[REDIRECT] DarkSoulsII.exe module not found");
        return results;
    }

    MODULEINFO modInfo = {};
    GetModuleInformation(GetCurrentProcess(), gameModule, &modInfo, sizeof(modInfo));

    uintptr_t base = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
    size_t moduleSize = modInfo.SizeOfImage;
    size_t needleLen = strlen(needle);

    if (needleLen >= moduleSize) return results;

    for (size_t i = 0; i < moduleSize - needleLen; i++) {
        if (memcmp(reinterpret_cast<void*>(base + i), needle, needleLen) == 0) {
            results.push_back(base + i);
        }
    }

    return results;
}

// Build a byte-swapped copy of a wide string for searching
static std::vector<wchar_t> MakeSwappedWideString(const wchar_t* src) {
    size_t len = wcslen(src);
    std::vector<wchar_t> swapped(len + 1);
    for (size_t i = 0; i <= len; i++) {
        wchar_t c = src[i];
        char* p = reinterpret_cast<char*>(&c);
        std::swap(p[0], p[1]);
        swapped[i] = c;
    }
    return swapped;
}

bool ServerRedirect::PatchHostname(const std::string& newHostname) {
    LOG_INFO("[REDIRECT] Patching server hostname to: %s", newHostname.c_str());

    const wchar_t* originalHostname = DS2_SERVER_HOSTNAME;

    // DS2 may store the hostname in normal byte order OR byte-swapped.
    // ds3os flips endian, so we search for both.
    auto swappedHostname = MakeSwappedWideString(originalHostname);

    int attempts = 0;
    const int maxAttempts = 60; // 30 seconds max wait

    while (attempts < maxAttempts) {
        // Search for normal byte order first
        auto matches = SearchWideString(originalHostname);
        // Also search for byte-swapped version
        auto swappedMatches = SearchWideString(swappedHostname.data());
        // Merge both result sets
        for (auto addr : swappedMatches) {
            matches.push_back(addr);
        }

        bool patched = false;
        for (uintptr_t addr : matches) {
            // Force memory writable
            DWORD oldProtect = 0;
            size_t hostnameBytes = (wcslen(originalHostname) + 1) * sizeof(wchar_t);
            if (!VirtualProtect(reinterpret_cast<void*>(addr),
                hostnameBytes, PAGE_READWRITE, &oldProtect)) {
                LOG_WARNING("[REDIRECT] VirtualProtect failed for hostname at 0x%p", reinterpret_cast<void*>(addr));
                continue;
            }

            // Convert hostname to wide string and write it
            // ds3os flips endian because FromSoft stores wchars byte-swapped.
            // Our SearchWideString uses memcmp against normal wchar_t, so if the
            // search matched, the memory is in normal byte order — check by reading
            // the first char to see if it's byte-swapped or not.
            std::wstring wideHostname(newHostname.begin(), newHostname.end());

            wchar_t* ptr = reinterpret_cast<wchar_t*>(addr);
            bool isSwapped = false;
            {
                // Check if 'f' (0x0066) is stored as 0x6600 (swapped)
                uint8_t* raw = reinterpret_cast<uint8_t*>(ptr);
                if (raw[0] == 0x66 && raw[1] == 0x00) {
                    isSwapped = false; // Normal LE order
                } else if (raw[0] == 0x00 && raw[1] == 0x66) {
                    isSwapped = true;  // Byte-swapped
                }
            }

            for (size_t i = 0; i < wideHostname.size() + 1; i++) {
                wchar_t chr = (i < wideHostname.size()) ? wideHostname[i] : L'\0';

                if (isSwapped) {
                    char* source = reinterpret_cast<char*>(&chr);
                    std::swap(source[0], source[1]);
                }

                memcpy(ptr, &chr, sizeof(wchar_t));
                ptr++;
            }

            // Restore protection
            VirtualProtect(reinterpret_cast<void*>(addr),
                hostnameBytes, oldProtect, &oldProtect);

            LOG_INFO("[REDIRECT] Patched hostname at 0x%p", reinterpret_cast<void*>(addr));
            patched = true;
        }

        if (patched) {
            LOG_INFO("[REDIRECT] Hostname patching complete");
            return true;
        }

        attempts++;
        Sleep(500);
    }

    LOG_ERROR("[REDIRECT] Failed to find hostname in game memory after %d attempts", maxAttempts);
    return false;
}

bool ServerRedirect::PatchRSAKey(const std::string& newPublicKey) {
    LOG_INFO("[REDIRECT] Patching RSA public key...");

    // FromSoft's original RSA public key (hardcoded in DS2 binary)
    const char* originalKey =
        "-----BEGIN RSA PUBLIC KEY-----\n"
        "MIIBCAKCAQEAxSeDuBTm3AytrIOGjDKpwJY+437i1F8leMBASVkknYdzM5HB4z8X\n"
        "YTXDylr/N6XAhgr/LcFFZ68yQNQ4AquriMONB+TWUiX0xu84ixYH3AqRtIVqLQbQ\n"
        "xKZsTfyCRC94n9EnvPeS+ueM495YhLIJQBf9T2aCeoHZBFDh2CghJQCdyd4dOT/E\n"
        "9ZxPImwj1t2fZkkKo4smpGk7GcCask2SGsnk/P2jUJxsOyFlCojaW1IldPxn+lXH\n"
        "dlgHSLjQvMlWiZ2SmOwvJqPWMv6XyUXYqsOdejRJJQjV7jeDzYG8trX+bSQxnTAw\n"
        "ENjvjslEcjBmzOCiqFTA/9H1jMjReZpI/wIBAw==\n"
        "-----END RSA PUBLIC KEY-----\n";

    int attempts = 0;
    const int maxAttempts = 60;

    while (attempts < maxAttempts) {
        auto matches = SearchAsciiString(originalKey);

        bool patched = false;
        for (uintptr_t addr : matches) {
            // Copy new key over old key (ds3os just does a straight memcpy)
            size_t copyLen = newPublicKey.size() + 1;
            size_t originalLen = strlen(originalKey) + 1;
            size_t patchLen = copyLen > originalLen ? copyLen : originalLen;

            // Force memory writable — always VirtualProtect regardless of current state
            DWORD oldProtect = 0;
            if (!VirtualProtect(reinterpret_cast<void*>(addr),
                patchLen, PAGE_READWRITE, &oldProtect)) {
                LOG_WARNING("[REDIRECT] VirtualProtect failed at 0x%p (error %u)",
                    reinterpret_cast<void*>(addr), GetLastError());
                continue;
            }

            memcpy(reinterpret_cast<void*>(addr), newPublicKey.c_str(), copyLen);

            // Zero-fill remaining bytes if new key is shorter
            if (copyLen < originalLen) {
                memset(reinterpret_cast<void*>(addr + copyLen), 0, originalLen - copyLen);
            }

            VirtualProtect(reinterpret_cast<void*>(addr),
                patchLen, oldProtect, &oldProtect);

            LOG_INFO("[REDIRECT] Patched RSA key at 0x%p", reinterpret_cast<void*>(addr));
            patched = true;
        }

        if (patched) {
            LOG_INFO("[REDIRECT] RSA key patching complete");
            return true;
        }

        attempts++;
        Sleep(500);
    }

    LOG_ERROR("[REDIRECT] Failed to find RSA key in game memory after %d attempts", maxAttempts);
    return false;
}

bool ServerRedirect::Install(const std::string& serverIp, const std::string& publicKeyPath) {
    LOG_INFO("[REDIRECT] Installing server redirect to %s", serverIp.c_str());

    // Read the public key from file
    std::string publicKey;
    std::ifstream keyFile(publicKeyPath);
    if (keyFile.is_open()) {
        publicKey.assign(std::istreambuf_iterator<char>(keyFile),
                         std::istreambuf_iterator<char>());
        keyFile.close();
        LOG_INFO("[REDIRECT] Loaded public key from %s (%zu bytes)", publicKeyPath.c_str(), publicKey.size());
    } else {
        LOG_ERROR("[REDIRECT] Could not open public key file: %s", publicKeyPath.c_str());
        return false;
    }

    // Patch hostname — the game will resolve this IP instead of FromSoft's server
    if (!PatchHostname(serverIp)) {
        LOG_ERROR("[REDIRECT] Hostname patching failed");
        return false;
    }

    // Patch RSA key — the game will use our server's key for encryption
    if (!PatchRSAKey(publicKey)) {
        LOG_ERROR("[REDIRECT] RSA key patching failed");
        return false;
    }

    LOG_INFO("[REDIRECT] Server redirect installed successfully");
    return true;
}
