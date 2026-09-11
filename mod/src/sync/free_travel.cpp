// Free travel with a partner: no co-op fog at area borders, and walking
// through one does not end the session.
//
// The fog a phantom meets at an area border is a "white door":
// MapObjWhiteDoorComponent (vtable exe+0x10C6458, param MapObjectWhiteDoorParam),
// listed per area by MapAreaWhiteDoorCtrl (area+0x1B0) and recomputed every
// frame (exe+0x1D1920). Its state comes from exe+0x1D24A0 out of three inputs:
// the door's kind (byte 0 of its param row), the session role at door+0x80 and
// an event flag at row+0x18. For kind 3, the co-op border:
//
//   role 0 (no session)      -> state 0: no fog at all
//   role 1 (host)            -> state 5: fog with a prompt; walking through ends co-op
//   role 2/3 (guest/invader) -> state 4: solid wall, no prompt ("туман мешает")
//
// Kinds 0-2 become walls for a guest as well. The role is read in exactly three
// places, each a CALL exe+0x25EA40 (*(*(roleObj+0x10)+0x68)); in a solo game the
// component writes 0 there itself. Doors (group A): those three calls become
// XOR EAX,EAX, so every door behaves as it does in a solo game.
//
// Walking through a door in multiplayer runs exe+0x1D1540: when the walker's
// ChrNetworkPhantomId ([chr+0xB0]+0x3C) is 12 it sends game packet 'J' to the
// others and posts end-of-co-op result 2 to EventResultManager (exe+0x192330),
// and every receiver of 'J' (exe+0x18AAF0) posts the same. That is how, in the
// stock game, the host walking out of the area sends everyone home. Crossing
// (group B): the send and both posts are taken out. The mod's older NOPs at
// exe+0x191C87 / exe+0x191D17 do not cover this path: they only catch queued
// replays, and these posts land directly.
//
// Both groups are checked byte for byte before anything is written and are put
// back exactly. The game thread runs this code every frame, so the bytes are
// only ever written from the game thread itself (FreeTravelGameTick, from the
// enemy generator update), never while it could be inside one of the sites.
// Probes log every door whose state changes (kind, the role the door stored,
// the session's real role, flag, state), the local phantom id when it changes,
// and every result posted to EventResultManager with its caller.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <intrin.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
constexpr uint32_t kGameManagerImp = 0x16148F0;   // *(exe+...) = GameManagerImp
constexpr uint32_t kDoorState      = 0x1D24A0;    // (door) -> state 0..6 in AL
constexpr uint32_t kResultPost     = 0x192330;    // (EventResultManager, request)

struct CodeSite {
    const char* Label;
    uint32_t    Rva;
    uint8_t     Expect[5];
    uint8_t     Replace[5];
};

// Group A: the three reads of the session role inside the door component
// (attach exe+0x1D1330, listener exe+0x1D1460, update exe+0x1D1920).
const CodeSite kDoorSites[] = {
    { "role on attach",   0x1D136F, { 0xE8, 0xCC, 0xD6, 0x08, 0x00 }, { 0x31, 0xC0, 0x90, 0x90, 0x90 } },
    { "role on listener", 0x1D14A6, { 0xE8, 0x95, 0xD5, 0x08, 0x00 }, { 0x31, 0xC0, 0x90, 0x90, 0x90 } },
    { "role on update",   0x1D195F, { 0xE8, 0xDC, 0xD0, 0x08, 0x00 }, { 0x31, 0xC0, 0x90, 0x90, 0x90 } },
};

// Group B: nothing after these calls reads what they return (checked in the
// disassembly), so they come out whole.
const CodeSite kCrossSites[] = {
    { "send 'J'",             0x1D1785, { 0xE8, 0x86, 0x97, 0xFB, 0xFF }, { 0x90, 0x90, 0x90, 0x90, 0x90 } },
    { "post result 2",        0x1D17C1, { 0xE8, 0x6A, 0x0B, 0xFC, 0xFF }, { 0x90, 0x90, 0x90, 0x90, 0x90 } },
    { "post result 2 on 'J'", 0x18AC63, { 0xE8, 0xC8, 0x76, 0x00, 0x00 }, { 0x90, 0x90, 0x90, 0x90, 0x90 } },
};

enum class SiteState { Stock, Patched, Foreign };

uintptr_t ExeBase() {
    return reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
}

SiteState ReadSite(const CodeSite& Site) {
    uint8_t Now[5] = {};
    __try {
        memcpy(Now, reinterpret_cast<const void*>(ExeBase() + Site.Rva), sizeof(Now));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SiteState::Foreign;
    }
    if (memcmp(Now, Site.Expect, sizeof(Now)) == 0) return SiteState::Stock;
    if (memcmp(Now, Site.Replace, sizeof(Now)) == 0) return SiteState::Patched;
    return SiteState::Foreign;
}

bool WriteSite(uint32_t Rva, const uint8_t* Bytes) {
    void* Addr = reinterpret_cast<void*>(ExeBase() + Rva);
    DWORD Old = 0;
    if (!VirtualProtect(Addr, 5, PAGE_EXECUTE_READWRITE, &Old)) return false;
    memcpy(Addr, Bytes, 5);
    VirtualProtect(Addr, 5, Old, &Old);
    FlushInstructionCache(GetCurrentProcess(), Addr, 5);
    return true;
}

// True only when every site of the group holds the patched bytes.
bool GroupPatched(const CodeSite* Sites, size_t Count) {
    for (size_t I = 0; I < Count; ++I) {
        if (ReadSite(Sites[I]) != SiteState::Patched) return false;
    }
    return true;
}

// Every site of a group on or off. A site holding neither the stock nor the
// patched bytes belongs to something else (another mod, another game version)
// and stops the whole group before anything is written. Game thread only.
void SetGroup(const CodeSite* Sites, size_t Count, bool On, const char* Group) {
    for (size_t I = 0; I < Count; ++I) {
        if (ReadSite(Sites[I]) == SiteState::Foreign) {
            LOG_WARNING("[TRAVEL] %s: exe+0x%X (%s) holds unexpected bytes -- left alone",
                        Group, Sites[I].Rva, Sites[I].Label);
            return;
        }
    }
    for (size_t I = 0; I < Count; ++I) {
        if ((ReadSite(Sites[I]) == SiteState::Patched) == On) continue;
        if (!WriteSite(Sites[I].Rva, On ? Sites[I].Replace : Sites[I].Expect)) {
            LOG_ERROR("[TRAVEL] %s: could not write exe+0x%X (%lu)", Group, Sites[I].Rva, GetLastError());
            return;
        }
    }
    LOG_INFO("[TRAVEL] %s: %s", Group, On ? "ON" : "OFF (stock)");
}

// What the ini or a key asked for, carried out on the game thread:
// -1 nothing pending, 0 off, 1 on.
std::atomic<int>  g_doorsWanted{ -1 };
std::atomic<int>  g_crossWanted{ -1 };
std::atomic<bool> g_doorsToast{ false };   // announce the change (keys only)
std::atomic<bool> g_crossToast{ false };

// --- probes --------------------------------------------------------------------
using DoorStateFn  = uint64_t(__fastcall*)(void*);
using ResultPostFn = void(__fastcall*)(void*, const int32_t*);

// Set by MH_CreateHook before the hook goes live, so a detour never sees null.
void* g_doorStateOriginal  = nullptr;
void* g_resultPostOriginal = nullptr;

struct DoorInfo {
    uint8_t  Kind;
    int32_t  StoredRole;
    uint32_t Flag;
};

bool ReadDoor(uintptr_t Door, DoorInfo* Out) {
    __try {
        const uint8_t* Row = *reinterpret_cast<uint8_t* const*>(Door + 0x58);
        Out->Kind = Row[0];
        Out->Flag = *reinterpret_cast<const uint32_t*>(Row + 0x18);
        Out->StoredRole = *reinterpret_cast<const int32_t*>(Door + 0x80);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// What exe+0x25EA40 would say now: -1 without a session, -2 unreadable.
int32_t ReadSessionRole() {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return -1;
        const uintptr_t Session = *reinterpret_cast<const uintptr_t*>(Gm + 0x22F0);
        if (!Session) return -1;
        const uintptr_t RoleHolder = *reinterpret_cast<const uintptr_t*>(Session + 0x3B8);
        if (!RoleHolder) return -1;
        const uintptr_t RoleObj = *reinterpret_cast<const uintptr_t*>(RoleHolder + 0x10);
        if (!RoleObj) return -1;
        return *reinterpret_cast<const int32_t*>(RoleObj + 0x68);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

// ChrNetworkPhantomId of the local player ([[GMImp+0xD0]+0xB0]+0x3C), -1 if none.
int32_t ReadPhantomId() {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return -1;
        const uintptr_t Player = *reinterpret_cast<const uintptr_t*>(Gm + 0xD0);
        if (!Player) return -1;
        const uintptr_t Net = *reinterpret_cast<const uintptr_t*>(Player + 0xB0);
        if (!Net) return -1;
        return *reinterpret_cast<const uint8_t*>(Net + 0x3C);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}

bool ReadRequest(const int32_t* Request, int32_t* Out) {
    __try {
        Out[0] = Request[0];
        Out[1] = Request[1];
        Out[2] = Request[2];
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* DoorStateName(uint8_t State) {
    switch (State) {
        case 0:  return "open, no fog";
        case 1:  return "fog, two prompts";
        case 2:
        case 3:  return "fog, one prompt";
        case 4:  return "WALL";
        case 5:  return "fog, prompt (crossing)";
        case 6:  return "fog (guest only)";
        default: return "?";
    }
}

// Last state logged per door, in a small table indexed by the door's address.
// Only the game thread writes it; two doors sharing a slot just log again.
struct DoorSlot {
    uintptr_t Door;
    uint8_t   State;
};
DoorSlot g_doorSlots[512] = {};

std::atomic<int32_t>   g_lastPhantomId{ -3 };
std::atomic<ULONGLONG> g_phantomCheckAt{ 0 };

void NotePhantomId() {
    const ULONGLONG Now = GetTickCount64();
    if (Now < g_phantomCheckAt.load()) return;
    g_phantomCheckAt.store(Now + 1000);
    const int32_t Id = ReadPhantomId();
    if (Id != g_lastPhantomId.exchange(Id)) {
        LOG_INFO("[TRAVEL] local phantom id %d (session role %d)", Id, ReadSessionRole());
    }
}

void NoteDoor(uintptr_t Door, uint8_t State) {
    DoorSlot& Slot = g_doorSlots[(Door >> 4) % _countof(g_doorSlots)];
    if (Slot.Door == Door && Slot.State == State) return;
    Slot.Door = Door;
    Slot.State = State;
    DoorInfo Info{};
    if (!ReadDoor(Door, &Info)) return;
    LOG_INFO("[TRAVEL] door %p: kind %u, stored role %d, session role %d, flag %u -> state %u (%s)",
             reinterpret_cast<void*>(Door), Info.Kind, Info.StoredRole, ReadSessionRole(),
             Info.Flag, State, DoorStateName(State));
}

// In the host's world: the join controller ([[netRoot+0x18]+0x40],
// NetSummonJoinMultiplayCtrl) is in state 7.
bool IsGuestInWorld() {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + 0x1616CF8);
        if (!Root) return false;
        const uintptr_t Mp = *reinterpret_cast<const uintptr_t*>(Root + 0x18);
        if (!Mp) return false;
        const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(Mp + 0x40);
        if (!Ctrl || *reinterpret_cast<const uintptr_t*>(Ctrl) != ExeBase() + 0x10D7BD8) return false;
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8) == 7;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A boss fog (kind 1) for a guest in the host's world, with the doors behaving
// as in a solo game:
//   state 3 -- the host has gone through (+0x85 == 100). The guest had no way
//              through it (the Last Giant, 12.09: ee stood outside while Restez
//              was in), so it is opened.
//   state 2 -- nobody is through yet. The fight starts from the host: a guest
//              who went in first found the boss idle (12.09 00:45). It stays a
//              wall until the host is in -- or until the host reports a fight
//              running, for a guest whose copy of the door missed the crossing.
uint64_t __fastcall DoorStateDetour(void* Door) {
    uint64_t Result = reinterpret_cast<DoorStateFn>(g_doorStateOriginal)(Door);
    const uint8_t Stock = static_cast<uint8_t>(Result);
    if ((Stock == 2 || Stock == 3) && GroupPatched(kDoorSites, _countof(kDoorSites)) && IsGuestInWorld()) {
        DoorInfo Info{};
        if (ReadDoor(reinterpret_cast<uintptr_t>(Door), &Info) && Info.Kind == 1) {
            const bool Open = Stock == 3 || IsHostInBossFight();
            Result = (Result & ~static_cast<uint64_t>(0xFF)) | (Open ? 0u : 4u);   // 0 open, 4 wall
        }
    }
    NoteDoor(reinterpret_cast<uintptr_t>(Door), static_cast<uint8_t>(Result));
    NotePhantomId();
    return Result;
}

void __fastcall ResultPostDetour(void* Manager, const int32_t* Request) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    int32_t Fields[3] = { -1, -1, -1 };
    ReadRequest(Request, Fields);
    LOG_INFO("[TRAVEL] result posted: type %d, id %d, %d (from exe+0x%llX)",
             Fields[0], Fields[1], Fields[2],
             static_cast<unsigned long long>(Caller - ExeBase()));
    reinterpret_cast<ResultPostFn>(g_resultPostOriginal)(Manager, Request);
}

// --- guard: state for areas this client does not have loaded ------------------
// The host sends state for the areas it has loaded (packet 0x14, handled by
// exe+0x518920). exe+0x5177C0 looks each entry up in the area's table at
// +0x168 without the load check it makes for the table at +0x160 -- harmless
// while a phantom cannot leave the host's area, but with free travel the guest
// walked towards Majula, an area the host still had loaded was half gone here,
// and +0x168 read null (12.09 23:58:12, access violation at exe+0x517843).
// Such an entry is skipped now, the way the game skips an entry it cannot find.
constexpr uint32_t kAreaEntryGet = 0x5177C0;   // (sync, index, uint* out) -> found
constexpr uint32_t kAreaById     = 0x3BCE40;   // (mapMgr, areaId) -> area

using AreaEntryGetFn = uint64_t(__fastcall*)(void*, uint32_t, uint32_t*);
using AreaByIdFn     = uintptr_t(__fastcall*)(uintptr_t, uint32_t);

void* g_areaEntryGetOriginal = nullptr;
std::atomic<uint32_t> g_lastSkippedArea{ 0 };

// True when the lookup would read an area table that is not there (yet).
bool AreaEntryUnready(const void* Sync, uint32_t Index, uint32_t* AreaId) {
    __try {
        *AreaId = *reinterpret_cast<const uint32_t*>(reinterpret_cast<uintptr_t>(Sync) + 0x18);
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t MapMgr = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x38) : 0;
        if (!MapMgr) return false;
        const uintptr_t Area = reinterpret_cast<AreaByIdFn>(ExeBase() + kAreaById)(MapMgr, *AreaId);
        if (!Area || (Index & 0x800) != 0) return false;   // both handled by the game itself
        return *reinterpret_cast<const uintptr_t*>(Area + 0x168) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}

uint64_t __fastcall AreaEntryGetDetour(void* Sync, uint32_t Index, uint32_t* Out) {
    uint32_t AreaId = 0;
    if (AreaEntryUnready(Sync, Index, &AreaId)) {
        if (g_lastSkippedArea.exchange(AreaId) != AreaId) {
            LOG_INFO("[TRAVEL] state for area %u arrived while that area is not loaded here -- skipped", AreaId);
        }
        return 0;
    }
    return reinterpret_cast<AreaEntryGetFn>(g_areaEntryGetOriginal)(Sync, Index, Out);
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[TRAVEL] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

void ShowToggle(const char* En, const char* Ru, bool On) {
    UI::Overlay::GetInstance().ShowNotification(
        UI::Format(UI::Tr("%s: %s", "%s: %s"), UI::Tr(En, Ru),
                   On ? UI::Tr("on", "вкл") : UI::Tr("off (stock)", "выкл (как в игре)")),
        3.0f, UI::NotifyKind::Player);
}

} // namespace

bool InstallFreeTravel(bool Enabled) {
    static bool Installed = false;
    if (!Installed) {
        Installed = true;
        // Probes first; they only watch.
        HookAt(kDoorState, reinterpret_cast<void*>(&DoorStateDetour), &g_doorStateOriginal, "door state");
        HookAt(kResultPost, reinterpret_cast<void*>(&ResultPostDetour), &g_resultPostOriginal, "result post");
        // Not a probe: keeps a guest who walked away from the host from crashing
        // on state for an area it no longer has loaded (see AreaEntryGetDetour).
        HookAt(kAreaEntryGet, reinterpret_cast<void*>(&AreaEntryGetDetour), &g_areaEntryGetOriginal,
               "area state lookup");
    }
    // Written by the game thread on its next enemy generator update.
    g_doorsWanted.store(Enabled ? 1 : 0);
    g_crossWanted.store(Enabled ? 1 : 0);
    LOG_INFO("[TRAVEL] free travel %s -- applied on the game thread once the world runs",
             Enabled ? "requested" : "off (free_travel=false)");
    return true;
}

void ToggleFreeTravelDoors() {
    g_doorsToast.store(true);
    g_doorsWanted.store(GroupPatched(kDoorSites, _countof(kDoorSites)) ? 0 : 1);
}

void ToggleFreeTravelCrossing() {
    g_crossToast.store(true);
    g_crossWanted.store(GroupPatched(kCrossSites, _countof(kCrossSites)) ? 0 : 1);
}

void FreeTravelGameTick() {
    const int Doors = g_doorsWanted.exchange(-1);
    if (Doors >= 0) {
        SetGroup(kDoorSites, _countof(kDoorSites), Doors == 1, "doors as in a solo game (no co-op fog walls)");
        if (g_doorsToast.exchange(false)) {
            // The toast names the fog, so "on" is the fog being back.
            ShowToggle("Border fog", "Туман на границах", !GroupPatched(kDoorSites, _countof(kDoorSites)));
        }
    }
    const int Cross = g_crossWanted.exchange(-1);
    if (Cross >= 0) {
        SetGroup(kCrossSites, _countof(kCrossSites), Cross == 1, "walking through a door keeps co-op");
        if (g_crossToast.exchange(false)) {
            ShowToggle("Crossing keeps co-op", "Переход не рвёт кооп", GroupPatched(kCrossSites, _countof(kCrossSites)));
        }
    }
}

} // namespace DS2Coop::Sync
