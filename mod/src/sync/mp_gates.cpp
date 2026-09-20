// What a host with a guest -- or a guest -- is kept from doing that a player alone
// can do (docs §3.39).
//
// Covenants. A host with the partner in its world could not join a covenant
// (16.09 evening, Targray): the talk opened, the yes/no came, then a message box
// from the talk script (exe+0x198920 from exe+0x46314C) -- the covenant write
// itself, exe+0x38BD80, has no session check at all. The refusal is a branch of
// the talk script on "is this multiplayer". While a talk with an NPC is open
// ([[[GMImp+0x70]+0x48]+0x40] != 0) the two questions a script can ask about it
// get the answer a player alone gets: exe+0x513580 "in multiplayer" from
// EventConditionNet_IsMultiPlay (return exe+0x46FE99) and from the character-
// script dispatcher (exe+0x45DDB4), and exe+0x45C520 "players in the session"
// from CompareMultiPlayNum (exe+0x46FCDE) and the dispatcher (exe+0x45E0EB).
//
// Summoning from Majula. The host's summon went out and the guest waited for an
// arrival that never came; the accept controller's constructor (exe+0x2BC3F0) had
// refused it. Of its three checks the one Majula fails is, by elimination, the
// area slot check exe+0x2C0700 at exe+0x2BC680 -- the same check the mod already
// passes for its own sign placement. For the lobby's host it lets the partner in;
// the other two checks are logged when they say no, in case it is one of them.
//
// The area's protection against invaders (17.09, point 8 of the second report). A
// Human Effigy burnt at a bonfire (exe+0x17F310, return exe+0x17F3C9) arms a per-area
// timer in NetSvrProperties ([[netRoot+0x30]+0x68]+0x18: up to 50 {area, seconds}),
// and exe+0x24F600(table, area) says "protected". The host's summon starter
// exe+0x2A2CA0 asks it for the current area (exe+0x24F690, return exe+0x2A2D01) and
// starts no job, and the accept constructor's sign-type check exe+0x291C30 asks it
// too (return exe+0x291C69) -- a refusal there is code 2 and a guest waiting forever.
// From 15:00:46 to 15:03:10 eight summons of the partner's signs in a protected Forest
// of Fallen Giants started nothing; the first after the host took the protection off
// at a bonfire went out. The protection is there for invaders: for those two questions,
// asked on the lobby partner's behalf, the answer is "not protected" (ini
// effigy_summon). Invasions, the HUD, the bonfire menu and the server status keep the
// game's answer.
//
// Cutscene transfers (17.09, point 9 of the second report: the eagle at the Pursuer's
// nest offered nothing to either player). The map's own event script hides them: event
// 7000 of event_m10_10_00_00.esd builds EventConditionNet_IsMultiPlay (command 140601,
// evaluated at exe+0x46FE70 = exe+0x513580(session) == [cond+0x10], the call returning
// to exe+0x46FE99) and enables the nest's action only once "multiplayer == 0" holds; a
// second group withdraws it on "multiplayer == 1". The ship at No-man's Wharf (event
// 14000) and eighteen more transfers -- giant memories, the Dark Chasm portals, the DLC
// entrances and exits -- are gated the same way. For those events alone, while a lobby
// is up, "in multiplayer" is answered no (ini transfer_events_solo); the other events
// that ask (bell keepers, boss-battle events) keep the game's answer. An event task's
// id is [task+0x28] and its map [[task+0x08]+0x18] (exe+0x1966D0, exe+0x196970).
//
// Probes. The ship's map table at No-man's Wharf never offered its prompt while a
// guest was in the host's world, most likely an event script asking "multiplayer?"
// -- each event that asks is logged once with its id, so the next session names
// it. And for the Pharros contraption a guest can press but that never does
// anything: every "object searched" hit is logged, and the IsHost answers of the
// next two seconds are logged by the IsHost detour (player_sync.cpp).

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
#include "../../include/session.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kGameManagerImp     = 0x16148F0;
constexpr uint32_t kInMultiplayer      = 0x513580;   // (session) -> AL
constexpr uint32_t kIsMultiPlayRet     = 0x46FE99;
constexpr uint32_t kInMpEsdRet         = 0x45DDB4;
constexpr uint32_t kPlayerCount        = 0x45C520;   // (kind, flag) -> count
constexpr uint32_t kCompareCountRet    = 0x46FCDE;
constexpr uint32_t kCountEsdRet        = 0x45E0EB;
constexpr uint32_t kSlotAreaCheck      = 0x2C0700;   // (slot area manager, sign type) -> AL
constexpr uint32_t kSlotAreaAcceptRet  = 0x2BC685;
constexpr uint32_t kSummonCheck1       = 0x291C30;   // (a, b, c, d) -> AL
constexpr uint32_t kSummonCheck1Ret    = 0x2BC594;
constexpr uint32_t kSummonCheck2       = 0x2C6460;   // (mp, desc) -> AL
constexpr uint32_t kSummonCheck2Ret    = 0x2BC5D6;
constexpr uint32_t kTaskUpdate         = 0x196B80;   // (event task, arg)
constexpr uint32_t kIsSearch           = 0x4705E0;   // (condition) -> AL
constexpr uint32_t kAreaProtected      = 0x24F600;   // (protection table, area) -> AL
constexpr uint32_t kProtectSecondsLeft = 0x24F520;   // (protection table, area) -> XMM0 seconds
constexpr uint32_t kNetRoot            = 0x1616CF8;
constexpr uint32_t kProtectStartRet    = 0x2A2D01;   // the summon starter exe+0x2A2CA0
constexpr uint32_t kProtectAcceptRet   = 0x291C69;   // the sign-type check exe+0x291C30
constexpr ULONGLONG kPartnerSummonFreshMs = 30000;

using Fn1  = uint64_t(__fastcall*)(void*);
using Fn2  = uint64_t(__fastcall*)(void*, void*);
using Fn4  = uint64_t(__fastcall*)(void*, void*, void*, void*);
using CountFn = uint64_t(__fastcall*)(int32_t, char);
using SlotFn  = uint64_t(__fastcall*)(void*, uint8_t);
using ProtectFn = uint64_t(__fastcall*)(void* table, int32_t area);
using SecondsFn = float(__fastcall*)(void* table, int32_t area);

Fn1     g_inMultiplayer = nullptr;
CountFn g_playerCount   = nullptr;
SlotFn  g_slotArea      = nullptr;
Fn4     g_check1        = nullptr;
Fn2     g_check2        = nullptr;
Fn2     g_taskUpdate    = nullptr;
Fn1     g_isSearch      = nullptr;
ProtectFn g_areaProtected = nullptr;

std::atomic<bool>      g_effigySummon{ true };
std::atomic<bool>      g_transferSolo{ true };

// {raw map, event id} of every cutscene transfer hidden while in multiplayer
// (scratchpad re_022_nest\transfer_events_gated.txt).
struct GatedEvent {
    uint32_t Map;
    int32_t  Event;
};
constexpr GatedEvent kGatedTransfers[] = {
    { 0x0A0A0000, 7000 },     { 0x0A120000, 14000 },   { 0x0A0A0000, 37000 },   { 0x0A0A0000, 37010 },
    { 0x0A0A0000, 37020 },    { 0x0A190000, 4001 },    { 0x0A200000, 4001 },    { 0x14150000, 4001 },
    { 0x14180000, 1000000 },  { 0x32240000, 53000 },   { 0x32240000, 55000 },   { 0x0A130000, 2000000 },
    { 0x0A190000, 1000000 },  { 0x0A200000, 3000000 }, { 0x32230000, 52000 },   { 0x32240000, 52000 },
    { 0x32250000, 52000 },    { 0x32250000, 4000 },    { 0x0A0E0000, 9000 },    { 0x0A020000, 5000 },
};
std::atomic<ULONGLONG> g_partnerSummonAt{ 0 };
std::atomic<ULONGLONG> g_protectLogAt{ 0 };
thread_local bool      t_partnerStart = false;
thread_local bool      t_partnerAccept = false;

std::atomic<bool>      g_enabled{ true };
std::atomic<uint32_t>  g_talkAnswers{ 0 };
std::atomic<ULONGLONG> g_searchHitAt{ 0 };
thread_local uintptr_t t_task = 0;

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool ReadPtr(uintptr_t Addr, uintptr_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uintptr_t*>(Addr);
        return *Out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// A talk with an NPC is open (npc_talk.cpp). Until 0.2.2 this read eight bytes where the
// handle is four and was true all session long -- every event script's "in multiplayer?"
// answered "no" in a lobby (17.09, second report).
bool TalkOpen() {
    return IsTalkOpenNearby();
}

bool InLobby() {
    return Session::SessionManager::GetInstance().IsActive();
}

// An event task's id ([task+0x28]) and the raw id of its map ([[task+0x08]+0x18]).
bool ReadTaskKey(uintptr_t Task, uint32_t* Map, int32_t* Event) {
    __try {
        *Event = *reinterpret_cast<const int32_t*>(Task + 0x28);
        const uintptr_t Area = *reinterpret_cast<const uintptr_t*>(Task + 0x08);
        *Map = Area ? *reinterpret_cast<const uint32_t*>(Area + 0x18) : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Probe (19.09, the host's black screen after making its character): ESD command 132242 raises
// or lowers the event manager's hold counter [[GMImp+0x70]+0x1B4] (exe+0x44E930 / exe+0x44FEE0) and,
// between 0 and 1, writes [[GMImp+0xB8]+0x1A4]; the crones' offer (m10_02 event 16000) raises it
// for the whole of the making and lowers it at the very end. Both logged as they change.
bool ReadEventViewSafe(int32_t* Hold, int32_t* Byte) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + 0x16148F0);
        if (!Gm) return false;
        const uintptr_t EvMgr = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        const uintptr_t B8 = *reinterpret_cast<const uintptr_t*>(Gm + 0xB8);
        if (!EvMgr || !B8) return false;
        *Hold = *reinterpret_cast<const int32_t*>(EvMgr + 0x1B4);
        *Byte = *reinterpret_cast<const uint8_t*>(B8 + 0x1A4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool IsGatedTransfer(uint32_t Map, int32_t Event) {
    for (const GatedEvent& G : kGatedTransfers) {
        if (G.Map == Map && G.Event == Event) return true;
    }
    return false;
}

// Each event that asks about multiplayer, logged once per map and event (the first 64).
void NoteTaskAsking(const char* What, uintptr_t Task, uint64_t Answer) {
    static std::atomic<uint64_t> s_seen[64];
    static std::atomic<uint32_t> s_count{ 0 };
    uint32_t Map = 0;
    int32_t Event = 0;
    if (!ReadTaskKey(Task, &Map, &Event)) return;
    const uint64_t Key = (static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event);
    const uint32_t N = s_count.load();
    for (uint32_t I = 0; I < N && I < 64; ++I) {
        if (s_seen[I].load() == Key) return;
    }
    if (N >= 64) return;
    s_seen[N].store(Key);
    s_count.store(N + 1);
    LOG_INFO("[GATES] event %d of map 0x%08X asks %s -> %llu", Event, Map, What,
             static_cast<unsigned long long>(Answer & 0xFF));
}

uint64_t __fastcall InMultiplayerDetour(void* Session) {
    const uint64_t Stock = g_inMultiplayer(Session);
    if (!(Stock & 0xFF) || !g_enabled.load()) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    if (Ret != kIsMultiPlayRet && Ret != kInMpEsdRet) return Stock;
    if (!InLobby()) return Stock;
    if (TalkOpen()) {
        const uint32_t N = g_talkAnswers.fetch_add(1) + 1;
        if (N <= 5 || N % 200 == 0) {
            LOG_INFO("[GATES] a talk script asked whether this is multiplayer -- answered no while talking (%u so far)", N);
        }
        return Stock & ~static_cast<uint64_t>(0xFF);
    }
    if (Ret == kIsMultiPlayRet && t_task) {
        uint32_t Map = 0;
        int32_t Event = 0;
        if (g_transferSolo.load() && ReadTaskKey(t_task, &Map, &Event) && IsGatedTransfer(Map, Event)) {
            // Once per event: four of them are asked every frame in turn, and "not the last one"
            // wrote 109 729 lines in one evening (18.09).
            static uint64_t s_told[32] = {};
            static uint32_t s_toldCount = 0;
            const uint64_t Key = (static_cast<uint64_t>(Map) << 32) | static_cast<uint32_t>(Event);
            bool Told = false;
            for (uint32_t I = 0; I < s_toldCount && !Told; ++I) Told = s_told[I] == Key;
            if (!Told && s_toldCount < 32) s_told[s_toldCount++] = Key;
            if (!Told) {
                LOG_INFO("[GATES] cutscene transfer event %d of map 0x%08X asks whether this is multiplayer -- "
                         "answered no, so its prompt is offered in co-op", Event, Map);
            }
            return Stock & ~static_cast<uint64_t>(0xFF);
        }
        NoteTaskAsking("IsMultiPlay", t_task, Stock);
    }
    return Stock;
}

uint64_t __fastcall PlayerCountDetour(int32_t Kind, char Flag) {
    const uint64_t Stock = g_playerCount(Kind, Flag);
    if (!g_enabled.load() || !(Stock & 0xFFFFFFFFull)) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    if (Ret != kCompareCountRet && Ret != kCountEsdRet) return Stock;
    if (!InLobby()) return Stock;
    if (TalkOpen()) {
        const uint32_t N = g_talkAnswers.fetch_add(1) + 1;
        if (N <= 5 || N % 200 == 0) {
            LOG_INFO("[GATES] a talk script asked how many players are here (%llu) -- answered 0 while talking (%u so far)",
                     static_cast<unsigned long long>(Stock & 0xFFFFFFFFull), N);
        }
        return 0;
    }
    if (Ret == kCompareCountRet && t_task) NoteTaskAsking("CompareMultiPlayNum", t_task, Stock);
    return Stock;
}

uint64_t __fastcall SlotAreaCheckDetour(void* Mgr, uint8_t Type) {
    const uint64_t Stock = g_slotArea(Mgr, Type);
    if (Stock & 0xFF) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    auto& Lobby = Session::SessionManager::GetInstance();
    if (Ret != kSlotAreaAcceptRet || !g_enabled.load() || !Lobby.IsActive() || !Lobby.IsHost()) return Stock;
    LOG_INFO("[GATES] summoning the partner: this area's slot check said no (sign type %u) -- let through for the lobby",
             static_cast<unsigned>(Type));
    return (Stock & ~static_cast<uint64_t>(0xFF)) | 1;
}

bool PartnerSummonRecent() {
    auto& Lobby = Session::SessionManager::GetInstance();
    const ULONGLONG At = g_partnerSummonAt.load();
    return Lobby.IsActive() && Lobby.IsHost() && At && GetTickCount64() - At < kPartnerSummonFreshMs;
}

uint64_t __fastcall SummonCheck1Detour(void* A, void* B, void* C, void* D) {
    const bool FromAccept = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase() == kSummonCheck1Ret;
    const bool Outer = t_partnerAccept;
    if (FromAccept && PartnerSummonRecent()) t_partnerAccept = true;
    const uint64_t Stock = g_check1(A, B, C, D);
    t_partnerAccept = Outer;
    if (!(Stock & 0xFF) && FromAccept && InLobby()) {
        LOG_INFO("[GATES] summoning the partner: the sign-type check (exe+0x291C30) said no");
    }
    return Stock;
}

// The two questions about the area's protection asked on the lobby partner's behalf
// get "not protected"; every other question keeps the game's answer.
uint64_t __fastcall AreaProtectedDetour(void* Table, int32_t Area) {
    const uint64_t Stock = g_areaProtected(Table, Area);
    if (!(Stock & 0xFF)) return Stock;
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    const bool Start = Ret == kProtectStartRet && t_partnerStart;
    const bool Accept = Ret == kProtectAcceptRet && t_partnerAccept;
    // A third caller asks it too, and that one is what kept the guest out on 21.09 evening
    // (checklist 22): the summon job started and went out -- "the partner's summon start let through"
    // is in the host's log at 19:58:50 -- and then the accept controller appeared already at state
    // 0x12 with code 2 and vanished, with the sign quietly gone. Since every player in a lobby is on
    // the host's own server, where there is nobody to keep out, the protection is answered "not
    // protected" for every caller while this game hosts a lobby; the return address is logged so the
    // site can be named. Invaders outside a lobby, and anything with effigy_summon off, keep the
    // game's own answer.
    const bool AnyCaller = !Start && !Accept;
    if (AnyCaller && !(g_effigySummon.load() && Session::SessionManager::GetInstance().IsActive() &&
                       Session::SessionManager::GetInstance().IsHost())) {
        return Stock;
    }
    const ULONGLONG Now = GetTickCount64();
    const bool Say = Now - g_protectLogAt.load() > 10000;
    if (Say) g_protectLogAt.store(Now);
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!g_effigySummon.load() || !Lobby.IsActive() || !Lobby.IsHost()) {
        if (Say) {
            LOG_INFO("[GATES] area %d is protected against invaders (effigy): the partner's summon %s is refused "
                     "(effigy_summon=false)", Area, Start ? "start" : "arrival");
        }
        return Stock;
    }
    if (Say) {
        LOG_INFO("[GATES] area %d is protected against invaders (effigy) -- the lobby partner's summon %s let "
                 "through (asked from exe+0x%llX)", Area,
                 Start ? "start" : Accept ? "arrival" : "step", static_cast<unsigned long long>(Ret));
    }
    return Stock & ~static_cast<uint64_t>(0xFF);
}

uint64_t __fastcall SummonCheck2Detour(void* Mp, void* Desc) {
    const uint64_t Stock = g_check2(Mp, Desc);
    if (!(Stock & 0xFF) && reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase() == kSummonCheck2Ret && InLobby()) {
        LOG_INFO("[GATES] summoning the partner: the session check (exe+0x2C6460) said no");
    }
    return Stock;
}

uint64_t __fastcall TaskUpdateDetour(void* Task, void* Arg) {
    const uintptr_t Outer = t_task;
    t_task = reinterpret_cast<uintptr_t>(Task);
    const uint64_t R = g_taskUpdate(Task, Arg);
    t_task = Outer;
    return R;
}

uint64_t __fastcall IsSearchDetour(void* Condition) {
    const uint64_t Stock = g_isSearch(Condition);
    if (!(Stock & 0xFF) || !InLobby()) return Stock;
    g_searchHitAt.store(GetTickCount64());
    static std::atomic<int32_t> s_lastObject{ 0 };
    int32_t Object = 0;
    __try {
        Object = *reinterpret_cast<const int32_t*>(reinterpret_cast<uintptr_t>(Condition) + 0x10);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    if (s_lastObject.exchange(Object) != Object) {
        int32_t Event = 0;
        uint32_t Map = 0;
        if (t_task) ReadTaskKey(t_task, &Map, &Event);
        LOG_INFO("[GATES] object %d searched (event %d) -- the IsHost answers of the next two seconds follow", Object, Event);
    }
    return Stock;
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[GATES] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

} // namespace

void EventViewProbeTick() {
    static int32_t s_hold = -1, s_byte = -1;
    static uint32_t s_lines = 0;
    int32_t Hold = 0, Byte = 0;
    if (!ReadEventViewSafe(&Hold, &Byte)) return;
    if (Hold == s_hold && Byte == s_byte) return;
    const bool First = s_hold < 0;
    s_hold = Hold;
    s_byte = Byte;
    if (First || s_lines >= 200) return;
    ++s_lines;
    LOG_INFO("[VIEW] the event manager's hold counter is %d, [[GMImp+0xB8]+0x1A4] = %d (ESD 132242)", Hold, Byte);
}

// The event task this thread is updating right now (exe+0x196B80), 0 outside one -- what tells
// an event script from a talk script when both ask the same thing (guest_world.cpp).
uintptr_t CurrentEventTask() {
    return t_task;
}

bool ReadEventTaskKey(uintptr_t Task, uint32_t* Map, int32_t* Event) {
    return ReadTaskKey(Task, Map, Event);
}

bool InstallMpGates(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        HookAt(kInMultiplayer, reinterpret_cast<void*>(&InMultiplayerDetour), reinterpret_cast<void**>(&g_inMultiplayer),
               "in multiplayer");
        HookAt(kPlayerCount, reinterpret_cast<void*>(&PlayerCountDetour), reinterpret_cast<void**>(&g_playerCount),
               "players in the session");
        HookAt(kSlotAreaCheck, reinterpret_cast<void*>(&SlotAreaCheckDetour), reinterpret_cast<void**>(&g_slotArea),
               "the summon area slot check");
        HookAt(kSummonCheck1, reinterpret_cast<void*>(&SummonCheck1Detour), reinterpret_cast<void**>(&g_check1),
               "summon check 1");
        HookAt(kSummonCheck2, reinterpret_cast<void*>(&SummonCheck2Detour), reinterpret_cast<void**>(&g_check2),
               "summon check 2");
        HookAt(kTaskUpdate, reinterpret_cast<void*>(&TaskUpdateDetour), reinterpret_cast<void**>(&g_taskUpdate),
               "event task update");
        HookAt(kIsSearch, reinterpret_cast<void*>(&IsSearchDetour), reinterpret_cast<void**>(&g_isSearch),
               "object searched");
        HookAt(kAreaProtected, reinterpret_cast<void*>(&AreaProtectedDetour),
               reinterpret_cast<void**>(&g_areaProtected), "the area's protection against invaders");
    }
    LOG_INFO("[GATES] %s", Enabled
        ? "while talking, NPC scripts see a player alone (covenants); the lobby's host can summon from Majula"
        : "off (mp_gates=false): the game's own answers, probes only");
    return g_inMultiplayer != nullptr;
}

void SetTransferEventsSolo(bool On) {
    g_transferSolo.store(On);
    LOG_INFO("[GATES] cutscene transfers (the eagle, the ship, portals, DLC entrances): %s",
             On ? "offered in co-op" : "the game's own rule (transfer_events_solo=false): not while in multiplayer");
}

void SetEffigySummon(bool On) {
    g_effigySummon.store(On);
    LOG_INFO("[GATES] effigy protection: %s", On ? "invaders kept out, the lobby partner's summon let through"
                                                 : "the game's own rule (effigy_summon=false): no summons while protected");
}

void SetPartnerSummonStarting(bool On) {
    t_partnerStart = On;
}

void NotePartnerSummonSent() {
    g_partnerSummonAt.store(GetTickCount64());
}

// The current area's protection, read the game's own way: protected and seconds left.
bool ReadAreaProtection(int32_t* AreaOut, float* SecondsOut) {
    *AreaOut = 0;
    *SecondsOut = 0.0f;
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        if (!Root) return false;
        const uintptr_t Props = *reinterpret_cast<const uintptr_t*>(Root + 0x30);
        const uintptr_t Owner = Props ? *reinterpret_cast<const uintptr_t*>(Props + 0x68) : 0;
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Root + 0x20);
        const uintptr_t Local = List ? *reinterpret_cast<const uintptr_t*>(List + 0x5B8) : 0;
        if (!Owner || !Local) return false;
        const int32_t Area = *reinterpret_cast<const int32_t*>(Local + 0x18);
        void* Table = reinterpret_cast<void*>(Owner + 0x18);
        *AreaOut = Area;
        const bool Protected = (reinterpret_cast<ProtectFn>(ExeBase() + kAreaProtected)(Table, Area) & 0xFF) != 0;
        if (Protected) *SecondsOut = reinterpret_cast<SecondsFn>(ExeBase() + kProtectSecondsLeft)(Table, Area);
        return Protected;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool RecentSearchHit() {
    const ULONGLONG At = g_searchHitAt.load();
    return At && GetTickCount64() - At < 2000;
}

} // namespace DS2Coop::Sync
