// Being summoned from anywhere -- also from where the game takes no signs.
//
// The host's summon reaches the sign's owner as PushRequestSummonSign, and the
// sign manager answers it in exe+0x2A0FF0 (NetSvrSummonSignManager, listener
// vtable exe+0x10D6270 slot 0; RCX = manager+8). Before it joins it asks, in
// this order:
//
//   1. is this my live sign (manager+0xF0, +0x18, id at +0x20)   -- else reject, code 2
//   2. exe+0x275E80(manager, 0xD): capability 0xD, "can be
//      summoned", on the capability object [netRoot+0x38]         -- else reject, code 1
//   3. exe+0x275E20(manager, &type): the multiplayer manager lets
//      this sign type be summoned here (exe+0x2C6570), and 0xD    -- else reject, code 2
//   4. the multiplayer manager is not busy ([[netRoot+0x18]+8],
//      a counter raised with exe+0x2C5C70, lowered with
//      exe+0x2C93E0)                                              -- else reject, code 0
//
// and then takes the summon: it blocks 0xD and exe+0x2C6330 builds the join
// controller. Where signs are not allowed the capability is off, so a sign the
// mod put down there anyway was turned down the moment the summon arrived.
//
// For a sign the mod put down, 2 and 3 are answered yes for that one call, on
// the thread making it; 1 stays the game's. Everything else reads the real
// values, so the soapstone and the game's own signs keep their rules.
//
// 4 -- who raises the busy counter (callers of exe+0x2C5C70):
//   * the bonfire menu (exe+0x17F310 .. exe+0x17F8B0). Joining from inside it is
//     not something the game was built to survive, so this stays: the mod puts
//     no sign down while it is up (player_sync.cpp), and a summon that arrives
//     at such a moment is followed by a fresh sign once the player is free.
//   * the event manager (exe+0x4503E0), for as long as the area the player is
//     in takes no multiplayer (exe+0x3C0890, below). In Majula that is always:
//     on 12.09 (03:01-03:03) the counter stayed up for two minutes with nobody
//     at a bonfire, and no sign ever went down. While a mod lobby is up, a
//     loaded area answers "multiplayer allowed" instead.
//   * cutscenes (exe+0x4815C0), event results (exe+0x190B00), the scripts
//     (exe+0x4712F0): left as they are.
// Both counter functions are probed: every change is counted by call site, and
// once the counter has been up for 3 s the log names who raised it.
//
// Putting the sign down where the game finds no spot for one: exe+0x2A2780 asks
// exe+0x2A6240 where the sign goes -- the region id at the player's feet, else
// the last spot where signs were allowed (SummonSignSetCtrl). After a save
// loaded straight into Majula there is neither, and exe+0x2A2780 falls back to
// a spot cached in the manager (+0xF4..+0x10C) that is still FF FF FF FF: area
// 257575755, which exe+0x2D8920 does not know, so CreateSummonSign
// (exe+0x29DFA0) returns nothing and no request is made ("returned cleanly",
// 11.09 20:44). Walking in from another map worked only because the cache still
// held that map. For the mod's own placement the spot where the player stands
// is used instead (exe+0x29CF20, the game's own "current map" spot). A create
// that failed like that also leaves the live-sign flag (+0x18) up with no id
// (+0x20): the next create would run the remove chain with the bogus spot and
// crash once it succeeded (exe+0x2869E0), so the game's own cleanup
// exe+0x2A3FC0(manager, 0) takes it down before the mod places again.

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
#include <cstdio>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
// All hooked ones start with whole instructions of 5+ bytes (checked in the
// disassembly): 0x275E80 with a RIP-relative MOV, 0x2A0FF0 / 0x275E20 with
// pushes, the two busy functions with MOVZX EAX,[RCX+8] + CMP/TEST (6 bytes,
// no branch in them), 0x3C0890 with MOV [RSP+0x18],RBX, 0x2A6240 with
// MOV [RSP+8],RBX.
constexpr uint32_t kSummonPush     = 0x2A0FF0;   // (manager+8, push): someone summons my sign
constexpr uint32_t kCanDo          = 0x275E80;   // (manager, capability) -> allowed now
constexpr uint32_t kTypeHere       = 0x275E20;   // (manager, &sign type) -> summonable here
constexpr uint32_t kBusyUp         = 0x2C5C70;   // (mp): ++[mp+8], stops at 0xFF
constexpr uint32_t kBusyDown       = 0x2C93E0;   // (mp): --[mp+8], stops at 0
constexpr uint32_t kMpAllowedHere  = 0x3C0890;   // () -> the player's area takes multiplayer
constexpr uint32_t kCurrentArea    = 0x3BCD90;   // (mapMgr) -> the area the player is in
constexpr uint32_t kAreaRawId      = 0x3BA320;   // (area, &scratch) -> &raw map id
constexpr uint32_t kSignSpot       = 0x2A6240;   // (request, spot out) -> a spot for the sign by the game's rules
constexpr uint32_t kSpotHere       = 0x29CF20;   // (spot out): the spot where the player stands, 0x24 bytes
constexpr uint32_t kClearLive      = 0x2A3FC0;   // (manager, again): take the manager's live sign down
constexpr uint32_t kTypeInMode     = 0x14ED40;   // (&mode, &phantom type) -> AL: table exe+0x1568810[mode*20 + type]
constexpr uint32_t kArrivalTypeRet = 0x2C4651;   // return address of that call in join state 6 (exe+0x2C45B0)
constexpr uint32_t kSlotConfirm    = 0x2D2F70;   // (summon slots, type, id, flag) -> AL: the slot reserved for it
constexpr uint32_t kSlotReserve    = 0x2D35F0;   // (summon slots, type, id): reserve one
constexpr uint32_t kSlotConfirmRet = 0x2C0434;   // the accept controller's state 0xF (exe+0x2C03E0, via exe+0x2C08C0)
constexpr uint32_t kGameManagerImp = 0x16148F0;  // *(exe+...) = GameManagerImp; +0x38 map manager
constexpr uint32_t kNetRoot        = 0x1616CF8;  // *(exe+...) = network root; +0x18 multiplayer manager
constexpr int      kCapSummoned    = 0xD;        // "can be summoned through a sign"

// The push (0x110 bytes): +0 the summoner's player id, +4 its name, +0x48 the
// sign id, +0x50 the session to join.
constexpr size_t kPushSummoner = 0x00;
constexpr size_t kPushSignId   = 0x48;

// The sign manager's live sign: +0x18 flag, +0x20 its id (0 until created).
constexpr size_t kLiveSignFlag = 0x18;
constexpr size_t kLiveSignId   = 0x20;

// A placed sign seldom waits longer; every placement arms it again.
constexpr ULONGLONG kArmedForMs = 10 * 60 * 1000;
// Free this long before a declined summon is followed by a fresh sign.
constexpr ULONGLONG kFreeSettleMs = 1500;
// Busy this long before the log names who holds the counter.
constexpr ULONGLONG kBusyReportMs = 3000;

using SummonPushFn  = void(__fastcall*)(void*, const uint8_t*);
using CanDoFn       = uint64_t(__fastcall*)(void*, int);
using TypeHereFn    = uint64_t(__fastcall*)(void*, const uint8_t*);
using BusyFn        = void(__fastcall*)(void*);
using MpAllowedFn   = uint64_t(__fastcall*)();
using CurrentAreaFn = uintptr_t(__fastcall*)(uintptr_t);
using AreaRawIdFn   = const uint32_t*(__fastcall*)(uintptr_t, void*);
using SignSpotFn    = uint64_t(__fastcall*)(const uint32_t*, uint32_t*);
using SpotHereFn    = void*(__fastcall*)(void*);
using ClearLiveFn   = void(__fastcall*)(void*, uint8_t);

// Set by MH_CreateHook before the hook goes live, so a detour never sees null.
void* g_summonPushOriginal = nullptr;
void* g_canDoOriginal      = nullptr;
void* g_typeHereOriginal   = nullptr;
void* g_busyUpOriginal     = nullptr;
void* g_busyDownOriginal   = nullptr;
void* g_mpAllowedOriginal  = nullptr;
void* g_signSpotOriginal   = nullptr;

// Join state 6 (exe+0x2C45B0), the last step before a guest stands in the host's world,
// asks exe+0x14ED40(&mode, &phantom type) -- a byte table at exe+0x1568810, 20 phantom
// types per mode, mode = [[GMImp+0xD0]+0x490]+0x1AD, the byte after the hollowing level
// (rows 1 and 8 allow no type 1 or 2, the white phantoms; row 0 allows 0,1,2,7,8,12,14).
// A "no" -- or 20 s spent in state 6 -- writes code 0xF, leaves the network session
// (exe+0x5205D0) and goes to state 7 anyway, where exe+0x2C385C throws the guest out on
// its first frame. On 17.09 the guest was thrown out that way six times, each in the
// second it arrived (16:45:19, 16:45:45, 16:46:11, 16:49:19, 17:43:46, 17:44:13), and the
// host's accept controller ended in the same second; what stored the code is not in the
// log. For the lobby partner's join this check answers yes, and every call from state 6
// is logged with its numbers, so the next log says whether this was it.
using TypeInModeFn = uint64_t(__fastcall*)(const int32_t*, const uint8_t*);
void* g_typeInModeOriginal = nullptr;

std::atomic<ULONGLONG> g_armedUntil{ 0 };
std::atomic<bool>      g_replaceWhenFree{ false };   // a summon was declined while busy
ULONGLONG              g_freeSince = 0;              // game thread only (the tick)
thread_local bool      t_modPlacing = false;         // inside the mod's own call of exe+0x2A2780

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

// What the checks do during one answer to a summon, on the thread giving it.
struct PushScope {
    bool InPush   = false;
    bool Force    = false;
    int  CanDo    = -1;   // the game's own answers (-1: not asked)
    int  TypeHere = -1;
};
thread_local PushScope t_scope;

const char* Answer(int Value) {
    return Value < 0 ? "not asked" : (Value ? "yes" : "no");
}

// Both return a bool in AL; the rest of RAX is passed through untouched.
uint64_t __fastcall CanDoDetour(void* Manager, int Capability) {
    uint64_t Result = reinterpret_cast<CanDoFn>(g_canDoOriginal)(Manager, Capability);
    PushScope& Scope = t_scope;
    if (Scope.InPush && Capability == kCapSummoned) {
        if (Scope.CanDo < 0) Scope.CanDo = (Result & 0xFF) != 0;
        if (Scope.Force) Result = (Result & ~static_cast<uint64_t>(0xFF)) | 1;
    }
    return Result;
}

uint64_t __fastcall TypeHereDetour(void* Manager, const uint8_t* Type) {
    uint64_t Result = reinterpret_cast<TypeHereFn>(g_typeHereOriginal)(Manager, Type);
    PushScope& Scope = t_scope;
    if (Scope.InPush) {
        if (Scope.TypeHere < 0) Scope.TypeHere = (Result & 0xFF) != 0;
        if (Scope.Force) Result = (Result & ~static_cast<uint64_t>(0xFF)) | 1;
    }
    return Result;
}

bool ReadPush(const uint8_t* Push, uint32_t* Summoner, uint32_t* SignId) {
    __try {
        *Summoner = *reinterpret_cast<const uint32_t*>(Push + kPushSummoner);
        *SignId   = *reinterpret_cast<const uint32_t*>(Push + kPushSignId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The multiplayer manager's busy counter ([[netRoot+0x18]+8]); -1 unreadable.
int ReadBusyCount() {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        if (!Root) return -1;
        const uintptr_t Mp = *reinterpret_cast<const uintptr_t*>(Root + 0x18);
        if (!Mp) return -1;
        return *reinterpret_cast<const uint8_t*>(Mp + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// --- multiplayer in areas that take none --------------------------------------
// exe+0x3C0890 answers "yes" only for an area that is loaded ([area+0x30]+8 ==
// 3), listed in the map manager's table (+0x28, by map number) and with byte
// +0x16 of its param clear. Its one caller is the event manager's update. The
// mod answers "yes" for a loaded area while a lobby is up; an area still
// loading keeps its "no", so a summon never lands in the middle of a load.
struct AreaInfo {
    bool     Loaded;
    uint32_t Map;   // online area number (10040000 = Majula)
};

AreaInfo ReadCurrentArea() {
    AreaInfo Info{ false, 0 };
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t MapMgr = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x38) : 0;
        if (!MapMgr) return Info;
        const uintptr_t Area = reinterpret_cast<CurrentAreaFn>(ExeBase() + kCurrentArea)(MapMgr);
        if (!Area) return Info;
        const uintptr_t Load = *reinterpret_cast<const uintptr_t*>(Area + 0x30);
        Info.Loaded = Load && *reinterpret_cast<const uint8_t*>(Load + 8) == 3;
        uint8_t Scratch[16] = {};
        const uint32_t* Raw = reinterpret_cast<AreaRawIdFn>(ExeBase() + kAreaRawId)(Area, Scratch);
        if (Raw) {
            const uint32_t R = *Raw;
            Info.Map = ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Info.Loaded = false;
    }
    return Info;
}

std::atomic<uint32_t> g_mpOpenedMap{ 0 };   // the map last opened for the log, 0 none

uint64_t __fastcall MpAllowedHereDetour() {
    const uint64_t Stock = reinterpret_cast<MpAllowedFn>(g_mpAllowedOriginal)();
    if ((Stock & 0xFF) || !DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return Stock;
    const AreaInfo Here = ReadCurrentArea();
    if (!Here.Loaded) return Stock;
    if (g_mpOpenedMap.exchange(Here.Map) != Here.Map) {
        LOG_INFO("[JOIN] map %u takes no multiplayer -- kept open while the co-op lobby is up", Here.Map);
    }
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

// --- a spot for the mod's sign where the game finds none -------------------------
bool WriteSpotHere(uint32_t* Out) {
    __try {
        reinterpret_cast<SpotHereFn>(ExeBase() + kSpotHere)(Out);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Returns a bool in AL; the rest of RAX is passed through untouched.
uint64_t __fastcall SignSpotDetour(const uint32_t* Request, uint32_t* Out) {
    const uint64_t Stock = reinterpret_cast<SignSpotFn>(g_signSpotOriginal)(Request, Out);
    if ((Stock & 0xFF) || !t_modPlacing || !Out) return Stock;
    if (!WriteSpotHere(Out)) return Stock;
    LOG_INFO("[PLACE] the game has no spot for a sign here -- using where I stand (area %u)", Out[0]);
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

bool ReadLiveSign(uintptr_t Manager, uint8_t* Live, uint32_t* Id) {
    __try {
        *Live = *reinterpret_cast<const uint8_t*>(Manager + kLiveSignFlag);
        *Id   = *reinterpret_cast<const uint32_t*>(Manager + kLiveSignId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CallClearLive(void* Manager) {
    __try {
        reinterpret_cast<ClearLiveFn>(ExeBase() + kClearLive)(Manager, 0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- who moves the busy counter ------------------------------------------------
// Every change, counted per call site (the return address into the caller).
// Only the game thread raises or lowers it; a rare race would only blur a count.
struct BusyCaller {
    uint32_t Rva;
    uint32_t Ups;
    uint32_t Downs;
};
constexpr int kBusyCallerSlots = 24;
BusyCaller            g_busyCallers[kBusyCallerSlots] = {};
std::atomic<uint32_t> g_busyLastRaiser{ 0 };
ULONGLONG             g_busySince = 0;      // game thread only (the tick)
bool                  g_busyReported = false;

void CountBusyCaller(uintptr_t ReturnAddress, bool Up) {
    const uint32_t Rva = static_cast<uint32_t>(ReturnAddress - ExeBase());
    for (BusyCaller& Slot : g_busyCallers) {
        if (Slot.Rva != Rva && Slot.Rva != 0) continue;
        Slot.Rva = Rva;
        if (Up) ++Slot.Ups;
        else ++Slot.Downs;
        return;
    }
}

void __fastcall BusyUpDetour(void* Mp) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    reinterpret_cast<BusyFn>(g_busyUpOriginal)(Mp);
    CountBusyCaller(Caller, true);
    g_busyLastRaiser.store(static_cast<uint32_t>(Caller - ExeBase()));
}

void __fastcall BusyDownDetour(void* Mp) {
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress());
    reinterpret_cast<BusyFn>(g_busyDownOriginal)(Mp);
    CountBusyCaller(Caller, false);
}

// Once the counter has been up for 3 s, the call sites that raised and lowered
// it so far and the one that raised it last; and when it drops, for how long.
void BusyWatchTick() {
    const int Busy = ReadBusyCount();
    const ULONGLONG Now = GetTickCount64();
    if (Busy <= 0) {
        if (g_busyReported) LOG_INFO("[BUSY] free again after %.1f s", (Now - g_busySince) / 1000.0);
        g_busySince = 0;
        g_busyReported = false;
        return;
    }
    if (!g_busySince) {
        g_busySince = Now;
        return;
    }
    if (g_busyReported || Now - g_busySince < kBusyReportMs) return;
    g_busyReported = true;
    char Seen[640] = {};
    int Length = 0;
    for (const BusyCaller& Slot : g_busyCallers) {
        if (!Slot.Rva || Length >= static_cast<int>(sizeof(Seen)) - 48) break;
        Length += snprintf(Seen + Length, sizeof(Seen) - Length, " exe+0x%X(+%u/-%u)", Slot.Rva, Slot.Ups, Slot.Downs);
    }
    LOG_INFO("[BUSY] busy (count %d) for 3 s -- raised last from exe+0x%X; call sites so far:%s",
             Busy, g_busyLastRaiser.load(), Length ? Seen : " none seen");
}

void __fastcall SummonPushDetour(void* Listener, const uint8_t* Push) {
    const bool Armed = GetTickCount64() < g_armedUntil.load();
    const int  Busy  = ReadBusyCount();
    uint32_t Summoner = 0, SignId = 0;
    ReadPush(Push, &Summoner, &SignId);

    // Pushes are handed out one after another, but should one ever arrive
    // inside another, the outer answer gets its own state back afterwards.
    const PushScope Outer = t_scope;
    t_scope = PushScope{ true, Armed, -1, -1 };
    reinterpret_cast<SummonPushFn>(g_summonPushOriginal)(Listener, Push);
    const PushScope Mine = t_scope;
    t_scope = Outer;

    // The game's own answers. Both "not asked": the sign was not ours or not
    // alive (rejected, code 2). A "no" while armed is what was answered yes.
    LOG_INFO("[JOIN] summon on sign %u by player %u: can be summoned %s, sign type allowed here %s, busy %d%s",
             SignId, Summoner, Answer(Mine.CanDo), Answer(Mine.TypeHere), Busy,
             Armed ? " -- the mod's sign, let through" : "");

    if (Armed && Busy > 0) {
        LOG_INFO("[JOIN] declined by the game: busy (raised last from exe+0x%X) -- a fresh sign goes down once free",
                 g_busyLastRaiser.load());
        g_replaceWhenFree.store(true);
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            DS2Coop::UI::Tr("The game is busy (bonfire, menu or an event) -- the summon is tried again once it is over.",
                            "Игра занята (костёр, меню или событие) \xE2\x80\x94 призыв повторится, как только освободится."),
            6.0f, DS2Coop::UI::NotifyKind::Warning);
    }
}

bool ReadModeAndType(const int32_t* Mode, const uint8_t* Type, int32_t* ModeOut, uint32_t* TypeOut) {
    __try {
        *ModeOut = *Mode;
        *TypeOut = *Type;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A guest thrown out right on arrival (17.09: six times, and endlessly at the Cathedral of Blue
// bonfire, 0.2.2 report point 4; and whenever the host crossed into another area while the guest
// joined, point 10). The host's accept controller, in state 0xF, confirms the summon slot it
// reserved when it was built (exe+0x2C0980 -> exe+0x2D35F0, result ignored): exe+0x2C08C0 jumps to
// exe+0x2D2F70. The reservations follow the host's online "mode" ([[mp+0x38]+0x10], applied every
// tick by exe+0x2C09A0), and a change of mode keeps only what the new one allows -- white summons
// are allowed in modes 3, 8, 13 and 14 only. So a summon started where the area's slot check said
// no (the mod lets that through, "this area's slot check said no", four of the six), or a host who
// walked across a border between summon and arrival (the other two, 8.9 and 10.9 m), had no
// reservation left: the confirm said no, code 1 went to the guest, and its join state 6 threw it
// out. The slot is reserved again here and asked once more; if the game still says no, the co-op
// lobby's join goes through anyway, as the rest of the summon does (ini join_slot_confirm).
using SlotConfirmFn = uint64_t(__fastcall*)(void*, uint32_t, uint32_t, uint32_t);
using SlotReserveFn = void(__fastcall*)(void*, uint32_t, uint32_t);
void* g_slotConfirmOriginal = nullptr;
std::atomic<bool> g_slotConfirmFix{ true };

bool ReserveAgainSafe(void* Slots, uint32_t Type, uint32_t Id) {
    __try {
        reinterpret_cast<SlotReserveFn>(ExeBase() + kSlotReserve)(Slots, Type, Id);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The host's online mode record [[mp+0x38]]: +8, +0xC and the mode at +0x10.
bool ReadOnlineModeSafe(int32_t* A, int32_t* B, int32_t* Mode) {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t Mp = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x18) : 0;
        const uintptr_t Rec = Mp ? *reinterpret_cast<const uintptr_t*>(Mp + 0x38) : 0;
        if (!Rec) return false;
        *A = *reinterpret_cast<const int32_t*>(Rec + 8);
        *B = *reinterpret_cast<const int32_t*>(Rec + 0xC);
        *Mode = *reinterpret_cast<const int32_t*>(Rec + 0x10);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall SlotConfirmDetour(void* Slots, uint32_t Type, uint32_t Id, uint32_t Flag) {
    const auto Original = reinterpret_cast<SlotConfirmFn>(g_slotConfirmOriginal);
    const uint64_t Stock = Original(Slots, Type, Id, Flag);
    if ((Stock & 0xFF) || reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kSlotConfirmRet) return Stock;
    if (!DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return Stock;
    int32_t A = 0, B = 0, Mode = -1;
    ReadOnlineModeSafe(&A, &B, &Mode);
    float X = 0.0f, Y = 0.0f, Z = 0.0f, Rot = 0.0f;
    GetLocalPlayerPosition(X, Y, Z, Rot);
    if (!g_slotConfirmFix.load()) {
        LOG_INFO("[JOIN] a guest's arrival: summon slot (type %u, id %u) not confirmed by the game -- online mode %d "
                 "(%d, %d) at (%.1f, %.1f, %.1f); left as the game has it (join_slot_confirm=false): the guest is "
                 "thrown out", Type & 0xFF, Id, Mode, A, B, X, Y, Z);
        return Stock;
    }
    const bool Reserved = ReserveAgainSafe(Slots, Type & 0xFF, Id);
    const uint64_t Again = Reserved ? Original(Slots, Type, Id, Flag) : 0;
    LOG_INFO("[JOIN] a guest's arrival: summon slot (type %u, id %u) not confirmed by the game -- online mode %d "
             "(%d, %d) at (%.1f, %.1f, %.1f); %s", Type & 0xFF, Id, Mode, A, B, X, Y, Z,
             (Again & 0xFF) ? "reserved again and confirmed"
             : Reserved     ? "still refused after reserving again -- let through: the co-op lobby's join"
                            : "reserving again threw -- let through: the co-op lobby's join");
    if (Again & 0xFF) return Again;
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

// Probe: the host's online mode whenever it changes, with where the host stands -- the borders
// where a reservation can be dropped between a summon and the guest's arrival.
void ModeWatchTick() {
    if (!DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive()) return;
    static int32_t s_mode = -2;
    int32_t A = 0, B = 0, Mode = -1;
    if (!ReadOnlineModeSafe(&A, &B, &Mode) || Mode == s_mode) return;
    float X = 0.0f, Y = 0.0f, Z = 0.0f, Rot = 0.0f;
    GetLocalPlayerPosition(X, Y, Z, Rot);
    LOG_INFO("[JOIN] online mode here: %d -> %d (%d, %d) at (%.1f, %.1f, %.1f)", s_mode, Mode, A, B, X, Y, Z);
    s_mode = Mode;
}

uint64_t __fastcall TypeInModeDetour(const int32_t* Mode, const uint8_t* Type) {
    const uint64_t Stock = reinterpret_cast<TypeInModeFn>(g_typeInModeOriginal)(Mode, Type);
    if (reinterpret_cast<uintptr_t>(_ReturnAddress()) != ExeBase() + kArrivalTypeRet) return Stock;
    int32_t M = -1;
    uint32_t T = 0;
    ReadModeAndType(Mode, Type, &M, &T);
    const bool Lobby = DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive();
    const bool Allowed = (Stock & 0xFF) != 0;
    static std::atomic<uint32_t> s_lines{ 0 };
    if (!Allowed || s_lines.fetch_add(1) < 6) {
        LOG_INFO("[JOIN] arriving (join state 6): phantom type %u in mode %d is %s%s", T, M,
                 Allowed ? "allowed" : "NOT allowed by the game's table",
                 !Allowed && Lobby ? " -- let through: the co-op lobby's join (otherwise thrown out on arrival)" : "");
    }
    if (Allowed || !Lobby) return Stock;
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (DS2Coop::Hooks::HookManager::GetInstance().InstallHook(
            reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[JOIN] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

bool InstallSummonAccept() {
    // Each hook at most once, so a second call never hooks a live target again.
    if (!g_canDoOriginal) {
        HookAt(kCanDo, reinterpret_cast<void*>(&CanDoDetour), &g_canDoOriginal, "capability check");
    }
    if (!g_typeHereOriginal) {
        HookAt(kTypeHere, reinterpret_cast<void*>(&TypeHereDetour), &g_typeHereOriginal, "sign type check");
    }
    // Without both checks the push hook could only log; leave the game alone.
    if (!g_summonPushOriginal && g_canDoOriginal && g_typeHereOriginal) {
        HookAt(kSummonPush, reinterpret_cast<void*>(&SummonPushDetour), &g_summonPushOriginal, "summon push");
    }
    if (!g_mpAllowedOriginal) {
        HookAt(kMpAllowedHere, reinterpret_cast<void*>(&MpAllowedHereDetour), &g_mpAllowedOriginal,
               "multiplayer-allowed-here check");
    }
    if (!g_signSpotOriginal) {
        HookAt(kSignSpot, reinterpret_cast<void*>(&SignSpotDetour), &g_signSpotOriginal, "sign spot lookup");
    }
    if (!g_typeInModeOriginal) {
        HookAt(kTypeInMode, reinterpret_cast<void*>(&TypeInModeDetour), &g_typeInModeOriginal,
               "the phantom-type table (join arrival)");
    }
    if (!g_slotConfirmOriginal) {
        HookAt(kSlotConfirm, reinterpret_cast<void*>(&SlotConfirmDetour), &g_slotConfirmOriginal,
               "the summon slot confirm (a guest's arrival)");
    }
    // Probes: they only count.
    if (!g_busyUpOriginal) {
        HookAt(kBusyUp, reinterpret_cast<void*>(&BusyUpDetour), &g_busyUpOriginal, "busy counter +1");
    }
    if (!g_busyDownOriginal) {
        HookAt(kBusyDown, reinterpret_cast<void*>(&BusyDownDetour), &g_busyDownOriginal, "busy counter -1");
    }
    const bool Ready = g_canDoOriginal && g_typeHereOriginal && g_summonPushOriginal;
    LOG_INFO("[JOIN] summons from anywhere: %s%s%s",
             Ready ? "ready (exe+0x2A0FF0, exe+0x275E80, exe+0x275E20)" : "NOT available",
             g_mpAllowedOriginal ? "; areas without multiplayer kept open during a lobby (exe+0x3C0890)" : "",
             g_signSpotOriginal ? "; the mod's sign goes down where the game finds no spot (exe+0x2A6240)" : "");
    return Ready;
}

void ArmSummonAccept() {
    g_armedUntil.store(GetTickCount64() + kArmedForMs);
}

bool IsSummonBusy() {
    return ReadBusyCount() > 0;
}

void WarnBusyForSign() {
    DS2Coop::UI::Overlay::GetInstance().ShowNotification(
        DS2Coop::UI::Tr("The game is busy (bonfire, menu or an event) -- the sign goes down once it is over.",
                        "Игра занята (костёр, меню или событие) \xE2\x80\x94 знак поставится, как только освободится."),
        5.0f, DS2Coop::UI::NotifyKind::Warning);
}

void SetModSignPlacement(bool On) {
    t_modPlacing = On;
}

bool ClearStaleLiveSign(void* Manager) {
    if (!Manager) return false;
    uint8_t Live = 0;
    uint32_t Id = 0;
    if (!ReadLiveSign(reinterpret_cast<uintptr_t>(Manager), &Live, &Id) || !Live || Id) return false;
    if (!CallClearLive(Manager)) {
        LOG_WARNING("[PLACE] a failed create left the live-sign flag up, and taking it down threw");
        return false;
    }
    LOG_INFO("[PLACE] a failed create had left the live-sign flag up without an id -- taken down first (exe+0x2A3FC0)");
    return true;
}

// The sign manager's live sign, taken down once the server has given it an id:
// exe+0x2A3FC0(manager, 0) is the game's own removal (its sign tick exe+0x2A4460
// calls it the same way). With an id it sends RequestRemoveSign and clears the
// manager; asked before the id is there it would only clear the manager and leave
// the sign on the server, with the create's answer still to come -- so it waits.
PlacedSignTakeDown TakeDownPlacedSign(void* Manager) {
    if (!Manager) return PlacedSignTakeDown::NoSign;
    uint8_t Live = 0;
    uint32_t Id = 0;
    if (!ReadLiveSign(reinterpret_cast<uintptr_t>(Manager), &Live, &Id) || !Live) return PlacedSignTakeDown::NoSign;
    if (!Id) return PlacedSignTakeDown::NotCreatedYet;
    if (!CallClearLive(Manager)) {
        LOG_WARNING("[PLACE] taking sign %u down threw", Id);
        return PlacedSignTakeDown::Failed;
    }
    LOG_INFO("[PLACE] sign %u taken down (exe+0x2A3FC0)", Id);
    return PlacedSignTakeDown::TakenDown;
}

void SetJoinSlotConfirm(bool On) {
    g_slotConfirmFix.store(On);
}

void SummonAcceptGameTick() {
    BusyWatchTick();
    ModeWatchTick();
    if (!g_replaceWhenFree.load()) return;
    const ULONGLONG Now = GetTickCount64();
    if (IsSummonBusy()) {
        g_freeSince = 0;
        return;
    }
    if (!g_freeSince) {
        g_freeSince = Now;
        return;
    }
    if (Now - g_freeSince < kFreeSettleMs) return;
    g_replaceWhenFree.store(false);
    g_freeSince = 0;
    LOG_INFO("[JOIN] free again -- putting the sign down once more");
    RequestRejoinSignPlacement();
}

} // namespace DS2Coop::Sync
