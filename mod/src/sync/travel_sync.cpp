// Travelling in a co-op session without leaving it (docs §3.38).
//
// What a warp leaves behind, from the 16.09 logs and the code:
//
//   * Every warp unloads the world, and the unload destroys every remote player's
//     character and the queue of records they are made from (exe+0x513340 ->
//     exe+0x51BFF0). Nothing makes them again -- the game only queues a player on
//     a join (the guest's join state 5, exe+0x2C3C80; the host on packet 0x0D,
//     exe+0x2C8330). So after either player travelled, neither saw the other.
//   * With the partner's character gone the session's mode (mp+0x68) drops to 0,
//     and in mode 0 the network enemy manager never attaches: enemies stop being
//     shared. Its table meanwhile still points into the map it was filled for,
//     and the per-frame claim and state packets write through those pointers --
//     the guest crashes after a travel look just like the host's of 18:47:40.
//
// So, in a lobby, on both sides:
//   1. the record the game queues for the partner (exe+0x51B0E0) is kept: its peer
//      id through the game's own copy (reference-counted, exe+0xA3DBD0) and the
//      0x5E4-byte record;
//   2. each side sends the map it is really standing in -- the game's own value,
//      not the sign map the mod uses elsewhere, which for a guest is its home;
//   3. an enemy table filled for a map this player is no longer in is emptied at
//      once (exe+0x517080). For a host that reset is the detach loop exe+0x517E70,
//      which sets bit 48 of record+0x3C through every entry and calls into the
//      record's character, so every entry that does not point at a record loaded
//      right now is zeroed first;
//   4. when both stand in the same map, settled, and the partner has no character
//      here, the kept record is queued again -- the game makes the character the
//      way it does on a join;
//   5. then, with the character back and the table empty and unarmed, the enemy
//      sync is armed again the way the session events arm it (exe+0x517040; a
//      guest also marks the snapshot applied, exe+0x516370, and pins the join's
//      map to the one it stands in, [joinCtrl+0x19C]), and the game's own tick
//      attaches it.
//
// Packets numbered for a map (17.09, point 11: enemies doubled or gone after rests
// and travels). The game's generator packets 'N'-'R' (exe+0x1F6FD0) and event-area
// packets 'E'-'G' (exe+0x18A500) carry no map: the receiver applies them to record
// N of the map exe+0x2C6DE0 names -- a host's own map, a guest's join map
// [joinCtrl+0x19C]. Once the players stand in different maps, 'O' destroys the
// character of whatever record N is here and 'P' spawns one at the sender's
// coordinates. So while both players' own map values differ, those packets are
// dropped.
//
// 3b. A table still attached while the players stand in different maps is let go
// (0.2.2 point 3: after the host went to Majula the guest's enemies stood still and
// took no hits). A client's attach (exe+0x517880) hands every enemy to the network:
// the authority bit (record +0x42 bit 0) cleared, the character's manipulator
// switched to network control (vt[0x38](0)). Only a claim, a handover or the host's
// detach give it back -- the client reset exe+0x517A80 only empties the entries. With
// the host's character gone nobody sends those enemies' state and nothing takes them
// over (exe+0x518230 needs two players in the session), so they froze. The reset
// exe+0x517080 runs the host's detach exe+0x517E70 for state 1, which sets the
// authority bit back and calls vt[0x38](1) for every entry, so state 1 is written
// first on either side; the table ends at state 0, unarmed, and step 5 arms it again
// once both share a map. Ini enemy_detach_when_apart.
//
// Steps 3 to 5 wait while a warp is under way and for kSettleMs after every load.
// On 17.09 step 5 ran in the very second of the host's travel, while the fade still
// showed the old map: the game's tick (exe+0x5170E0 -> exe+0x517BF0) attached the
// table to the old map's generator records, the load freed them, and the per-frame
// pass (exe+0x518230) and the arrival reset wrote through the freed memory into the
// new map's objects. Four host crashes, Majula -> Forest of Fallen Giants, each with
// "armed again" in the second of the warp; none in nine travels without it (§3.43).
// Every step is logged.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kNetRoot          = 0x1616CF8;
constexpr uint32_t  kGameManagerImp   = 0x16148F0;
constexpr uint32_t  kQueuePlayer      = 0x51B0E0;   // (player list, peer id*, record*, flag) -> queued
constexpr uint32_t  kFindById         = 0x51D4B0;   // (player list, peer id*) -> the character slot, or 0
constexpr uint32_t  kPeerIdCtor       = 0xA3DA40;   // (peer id)
constexpr uint32_t  kPeerIdCopy       = 0xA3DBD0;   // (peer id, source peer id*)
constexpr uint32_t  kPeerIdClear      = 0xA3E2B0;   // (peer id): releases the reference, +0 = 0
constexpr uint32_t  kEnemyReset       = 0x517080;   // (enemy manager)
constexpr uint32_t  kEnemyArm         = 0x517040;   // (enemy manager): +0x74 = 1, +0x198 = 0
constexpr uint32_t  kEnemySnapApplied = 0x516370;   // (enemy manager): +0x198 = 1
constexpr uint32_t  kNetEnemyVtable   = 0x10FB580;
constexpr uint32_t  kJoinCtrlVtable   = 0x10D7BD8;
constexpr uint32_t  kGenAreaFind      = 0x419A70;   // (generator manager, map) -> the map's generator block, or 0
constexpr uint32_t  kEnemyEntries     = 255;        // the table at [enemy manager+0x10]
constexpr uint32_t  kEnemyEntrySize   = 0x18;       // +0x10 the record pointer
constexpr uint32_t  kGenRecordSize    = 0xA0;       // [block+0x18] + i * 0xA0, [block+0x20] of them
constexpr uint8_t   kWarpUnderWay     = 0x02;       // [GMImp+0x24B1]: set by exe+0x1C2A80, cleared at exe+0x1BF8C7 / exe+0x1C034A
constexpr uint32_t  kRecordSize       = 0x5E4;
constexpr ULONGLONG kSettleMs         = 3000;
constexpr ULONGLONG kRetryMs          = 10000;
constexpr ULONGLONG kMapSendMs        = 2000;
constexpr ULONGLONG kPartnerMapFresh  = 8000;
constexpr ULONGLONG kTravelNoteMs     = 120000;   // an arrival this soon after my travel is logged as its end
constexpr ULONGLONG kApartMs          = 5000;     // in different maps this long before the table is let go
constexpr uint32_t  kEventPackets     = 0x18A500;   // listener (self, type 'E'-'G', data, size)
constexpr uint32_t  kGenPackets       = 0x1F6FD0;   // listener (self, type 'N'-'R', data, size)

using QueueFn = uint64_t(__fastcall*)(void* list, void* id, void* record, uint8_t flag);
using FindFn  = void*(__fastcall*)(void* list, void* id);
using AreaFn  = uintptr_t(__fastcall*)(void* generatorManager, int32_t map);
using ObjFn   = void(__fastcall*)(void* obj);
using CopyFn  = void*(__fastcall*)(void* dst, void* src);

using MapPacketFn = void(__fastcall*)(void* listener, char type, uint8_t* data, uint32_t size);

QueueFn g_queueOriginal = nullptr;
MapPacketFn g_eventPacketsOriginal = nullptr;
MapPacketFn g_genPacketsOriginal   = nullptr;
std::atomic<uint32_t> g_mapPacketsDropped{ 0 };
std::atomic<bool> g_enabled{ true };
std::atomic<bool> g_detachWhenApart{ true };

// The partner's record, as the game last queued it.
std::mutex g_recordMutex;
alignas(16) uint8_t g_peerId[0x40] = {};
alignas(16) uint8_t g_record[kRecordSize] = {};
bool      g_peerIdMade  = false;
bool      g_haveRecord  = false;
ULONGLONG g_recordAt    = 0;
thread_local bool t_queueingOurselves = false;

std::atomic<int32_t>   g_partnerMap{ 0 };
std::atomic<ULONGLONG> g_partnerMapAt{ 0 };
std::atomic<ULONGLONG> g_localTravelAt{ 0 };

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

bool ReadI32(uintptr_t Addr, int32_t* Out) {
    __try {
        *Out = *reinterpret_cast<const int32_t*>(Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadU8(uintptr_t Addr, uint8_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uint8_t*>(Addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The game's map ids: 0x0A1F0000 is map 10310000.
bool LooksLikeRawMap(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) >= 10 && ((R >> 24) & 0xFF) <= 60 && ((R >> 16) & 0xFF) < 100 &&
           ((R >> 8) & 0xFF) < 100 && (R & 0xFF) < 100;
}

uint32_t RawToArea(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
}

// The map this player stands in, the game's own value ([[[netRoot+0x20]+0x5B8]+0xC]).
bool ReadLocalRawMap(int32_t* Out) {
    uintptr_t Root = 0, List = 0, Local = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x20, &List) || !ReadPtr(List + 0x5B8, &Local)) {
        return false;
    }
    return ReadI32(Local + 0xC, Out) && LooksLikeRawMap(*Out);
}

uintptr_t PlayerList() {
    uintptr_t Root = 0, List = 0;
    return ReadPtr(ExeBase() + kNetRoot, &Root) && ReadPtr(Root + 0x20, &List) ? List : 0;
}

// Loaded and standing: the local character exists, the game says "in game"
// ([GMImp+0x24AC] == 0x1E) and no warp is under way. RequestWarp sets the warp bit
// the moment it takes a warp, and the old map stays loaded through the whole fade
// that follows. Both places that clear the bit move the game out of state 0x1E in
// the same write -- to 0x1F when the fade has run out (exe+0x1BF8C7), to 0x1D in
// exe+0x1C0250 -- so from the request until the next map is in, this is false.
bool Standing() {
    uintptr_t Gm = 0, Player = 0;
    int32_t State = 0;
    uint8_t Flags = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0xD0, &Player) &&
           ReadI32(Gm + 0x24AC, &State) && State == 0x1E && ReadU8(Gm + 0x24B1, &Flags) &&
           !(Flags & kWarpUnderWay);
}

int JoinState(uintptr_t* CtrlOut) {
    *CtrlOut = 0;
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -1;
    int32_t State = -1;
    if (!ReadI32(Ctrl + 0xF8, &State)) return -1;
    *CtrlOut = Ctrl;
    return State;
}

struct EnemyTable {
    uintptr_t Mgr;
    int32_t   State;
    int32_t   Area;
    uint8_t   Armed;
    bool      Ok;
};

EnemyTable ReadEnemyTable() {
    EnemyTable T{};
    uintptr_t Root = 0, Mgr = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x28, &Mgr) || !ReadPtr(Mgr, &Vtbl)) return T;
    if (Vtbl != ExeBase() + kNetEnemyVtable) return T;
    __try {
        T.Mgr   = Mgr;
        T.State = *reinterpret_cast<const int32_t*>(Mgr + 8);
        T.Area  = *reinterpret_cast<const int32_t*>(Mgr + 0x18);
        T.Armed = *reinterpret_cast<const uint8_t*>(Mgr + 0x74);
        T.Ok    = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        T.Ok = false;
    }
    return T;
}

// --- the partner's record ------------------------------------------------------------
bool CopyRecordSafe(void* Id, void* Record) {
    __try {
        if (!g_peerIdMade) {
            reinterpret_cast<ObjFn>(ExeBase() + kPeerIdCtor)(g_peerId);
            g_peerIdMade = true;
        }
        reinterpret_cast<CopyFn>(ExeBase() + kPeerIdCopy)(g_peerId, Id);
        std::memcpy(g_record, Record, kRecordSize);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall QueuePlayerDetour(void* List, void* Id, void* Record, uint8_t Flag) {
    const uint64_t Queued = g_queueOriginal(List, Id, Record, Flag);
    if ((Queued & 0xFF) && !t_queueingOurselves && Id && Record &&
        Session::SessionManager::GetInstance().IsActive()) {
        std::lock_guard<std::mutex> Lock(g_recordMutex);
        g_haveRecord = CopyRecordSafe(Id, Record);
        g_recordAt = GetTickCount64();
        LOG_INFO("[TRAVEL] the game queued the partner's character -- its record is kept for after a travel (%s)",
                 g_haveRecord ? "copied" : "copy failed");
    }
    return Queued;
}

// A kept record is only ever good for the stay it was taken in. Once the partner
// is out of this world -- or the lobby is gone -- it is let go, reference and all:
// queued later it would make a character for a player who is not here (code review
// of f4027d0: a host standing in the map a guest was standing in AT HOME, after an
// earlier summon, would have queued that old record).
bool ClearRecordSafe() {
    __try {
        if (g_peerIdMade) reinterpret_cast<ObjFn>(ExeBase() + kPeerIdClear)(g_peerId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ForgetRecord(const char* Why) {
    std::lock_guard<std::mutex> Lock(g_recordMutex);
    if (!g_haveRecord) return;
    g_haveRecord = false;
    ClearRecordSafe();
    LOG_INFO("[TRAVEL] the partner's kept record let go: %s", Why);
}

// The host's side of "the partner is in my world": an accept controller that has
// taken the guest all the way in (state 0x10).
bool GuestFullyInMyWorld(bool* AnyController) {
    *AnyController = false;
    uintptr_t Root = 0, Mp = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return false;
    __try {
        uintptr_t It  = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (!Ctrl) continue;
            *AnyController = true;
            if (*reinterpret_cast<const int32_t*>(Ctrl + 0x150) == 0x10) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

bool PartnerCharacterHereSafe(uintptr_t List) {
    __try {
        return reinterpret_cast<FindFn>(ExeBase() + kFindById)(reinterpret_cast<void*>(List), g_peerId) != nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;   // unreadable: do nothing
    }
}

bool QueueRecordSafe(uintptr_t List, uint64_t* Result) {
    __try {
        *Result = g_queueOriginal(reinterpret_cast<void*>(List), g_peerId, g_record, 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// True when a packet numbered for the partner's map would land on a different map
// here: a lobby, both map values known and fresh, and they differ.
bool PartnerPacketForAnotherMap() {
    if (!Session::SessionManager::GetInstance().IsActive()) return false;
    const ULONGLONG TheirAt = g_partnerMapAt.load(std::memory_order_relaxed);
    if (!TheirAt || GetTickCount64() - TheirAt >= kPartnerMapFresh) return false;
    int32_t MyMap = 0;
    if (!ReadLocalRawMap(&MyMap)) return false;
    return g_partnerMap.load(std::memory_order_relaxed) != MyMap;
}

// Probe (19.09, the host's black screen after making its character): which event task a packet
// 'E'/'F'/'G' moves. The packet names the task by its index in the area of the map exe+0x2C6DE0
// names (byte 0, [task+0x2C]); the receiver finds the area as exe+0x18A500 does --
// exe+0x452FB0([[GMImp+0x70]+0x10], map) -- and the task at [[area+0x38] + index*8]: event id
// [task+0x28], raw map [[task+8]+0x18], network byte [task+0x2F]. Once per type, map and event.
constexpr uint32_t kJoinMapOf   = 0x2C6DE0;   // (multiplayer manager, u32* out) -> u32*: the map packets are for
constexpr uint32_t kAreaForMap  = 0x452FB0;   // (event list, map) -> the map's event area

bool EventPacketTaskSafe(const uint8_t* Data, uint32_t* Map, int32_t* Event, uint8_t* Net) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t EvMgr = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x70) : 0;
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t Mp = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x18) : 0;
        if (!EvMgr || !Mp || !*reinterpret_cast<const uintptr_t*>(Gm + 0x22F0)) return false;
        uint32_t Out[2] = {};
        const uint32_t* JoinMap = reinterpret_cast<const uint32_t*(__fastcall*)(uintptr_t, uint32_t*)>(
            ExeBase() + kJoinMapOf)(Mp, Out);
        if (!JoinMap) return false;
        const uintptr_t Area = reinterpret_cast<uintptr_t(__fastcall*)(uintptr_t, uint32_t)>(
            ExeBase() + kAreaForMap)(*reinterpret_cast<const uintptr_t*>(EvMgr + 0x10), *JoinMap);
        if (!Area) return false;
        const uintptr_t Tasks = *reinterpret_cast<const uintptr_t*>(Area + 0x38);
        const uintptr_t Info = *reinterpret_cast<const uintptr_t*>(Area + 0x20);
        const uintptr_t List = Info ? *reinterpret_cast<const uintptr_t*>(Info + 0x10) : 0;
        if (!Tasks || !List || Data[0] >= *reinterpret_cast<const uint16_t*>(List + 10)) return false;
        const uintptr_t Task = *reinterpret_cast<const uintptr_t*>(Tasks + static_cast<uintptr_t>(Data[0]) * 8);
        if (!Task) return false;
        const uintptr_t TaskArea = *reinterpret_cast<const uintptr_t*>(Task + 0x08);
        *Event = *reinterpret_cast<const int32_t*>(Task + 0x28);
        *Map = TaskArea ? *reinterpret_cast<const uint32_t*>(TaskArea + 0x18) : 0;
        *Net = *reinterpret_cast<const uint8_t*>(Task + 0x2F);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// 'E' comes for every task of a map at once when its scripts start again (a rest, a load): 19.09 used up
// the whole probe on those in two rests. So 'E' is only counted, and 'F'/'G' -- a condition met on the
// other side, the host's transition -- are named one by one: event, sequence byte, the state key and mask.
void NoteEventPacket(char Type, const uint8_t* Data, uint32_t Size, bool Dropped) {
    if (Size < 12 || !Data) return;
    if (Type == 'E') {
        if (Dropped) return;
        static std::atomic<uint32_t> s_restarts{ 0 };
        static std::atomic<ULONGLONG> s_loggedAt{ 0 };
        const uint32_t N = s_restarts.fetch_add(1) + 1;
        const ULONGLONG Now = GetTickCount64();
        if (Now - s_loggedAt.load() >= 5000) {
            s_loggedAt.store(Now);
            s_restarts.store(0);
            LOG_INFO("[EVENTNET] %u 'E' from the partner (its event scripts started again)", N);
        }
        return;
    }
    uint32_t Map = 0;
    int32_t Event = 0;
    uint8_t Net = 0;
    const bool Known = EventPacketTaskSafe(Data, &Map, &Event, &Net);
    static uint32_t s_count = 0;
    if (s_count >= 400) return;
    ++s_count;
    uint32_t Key = 0;
    uint16_t Key16 = 0, Mask = 0, Sender = 0;
    memcpy(&Key, Data + 2, sizeof(Key));
    memcpy(&Key16, Data + 6, sizeof(Key16));
    memcpy(&Mask, Data + 8, sizeof(Mask));
    memcpy(&Sender, Data + 10, sizeof(Sender));
    if (Known) {
        LOG_INFO("[EVENTNET] '%c' from the partner (player %u) for event %d of map 0x%08X (task #%u, network byte %u): "
                 "seq %u, key %u/%u, mask 0x%X%s", Type, Sender, Event, Map, Data[0], Net, Data[1], Key, Key16, Mask,
                 Dropped ? " -- DROPPED: we stand in different maps (the lever? see docs 3.50)" : "");
    } else {
        LOG_INFO("[EVENTNET] '%c' from the partner (player %u) for task #%u -- the task was not found here",
                 Type, Sender, Data[0]);
    }
}

void __fastcall EventPacketsDetour(void* Listener, char Type, uint8_t* Data, uint32_t Size) {
    if (PartnerPacketForAnotherMap()) {
        g_mapPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        if (Type == 'F' || Type == 'G') NoteEventPacket(Type, Data, Size, true);
        return;
    }
    if (Type == 'E' || Type == 'F' || Type == 'G') NoteEventPacket(Type, Data, Size, false);
    g_eventPacketsOriginal(Listener, Type, Data, Size);
}

void __fastcall GenPacketsDetour(void* Listener, char Type, uint8_t* Data, uint32_t Size) {
    if (PartnerPacketForAnotherMap()) {
        g_mapPacketsDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_genPacketsOriginal(Listener, Type, Data, Size);
}

void LogDroppedMapPackets(ULONGLONG Now, int32_t MyMap) {
    static ULONGLONG s_at = 0;
    if (Now - s_at < 10000) return;
    s_at = Now;
    const uint32_t Dropped = g_mapPacketsDropped.exchange(0);
    if (!Dropped) return;
    LOG_INFO("[TRAVEL] %u enemy and event packets from the partner dropped in 10 s: it stands in map %u, I in %u, "
             "and they are numbered for its map", Dropped, RawToArea(g_partnerMap.load()), RawToArea(MyMap));
}

// --- the enemy table ------------------------------------------------------------------
bool ResetTableSafe(uintptr_t Mgr) {
    __try {
        reinterpret_cast<ObjFn>(ExeBase() + kEnemyReset)(reinterpret_cast<void*>(Mgr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The host's detach for either side: see 3b above.
bool DetachTableSafe(uintptr_t Mgr) {
    __try {
        *reinterpret_cast<int32_t*>(Mgr + 8) = 1;
        reinterpret_cast<ObjFn>(ExeBase() + kEnemyReset)(reinterpret_cast<void*>(Mgr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ArmTableSafe(uintptr_t Mgr, bool Client, uintptr_t JoinCtrl, int32_t Map) {
    __try {
        if (Client && JoinCtrl) {
            int32_t* Pinned = reinterpret_cast<int32_t*>(JoinCtrl + 0x19C);
            if (LooksLikeRawMap(*Pinned) && *Pinned != Map) *Pinned = Map;
        }
        reinterpret_cast<ObjFn>(ExeBase() + kEnemyArm)(reinterpret_cast<void*>(Mgr));
        if (Client) reinterpret_cast<ObjFn>(ExeBase() + kEnemySnapApplied)(reinterpret_cast<void*>(Mgr));
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void SendMap(int32_t Raw) {
    Network::PlayerMapPacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::PlayerMap;
    Packet.header.size = sizeof(Packet);
    Packet.rawMap = Raw;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

bool HookAt(uint32_t Rva, void* Detour, void** Original, const char* What) {
    if (Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + Rva), Detour, Original)) {
        return true;
    }
    LOG_WARNING("[TRAVEL] could not hook exe+0x%X (%s)", Rva, What);
    return false;
}

// The generator records the game holds right now for a map, as [First, End); both
// 0 when there is no such block or it cannot be read.
void LiveRecordsSafe(int32_t Map, uintptr_t* First, uintptr_t* End) {
    *First = 0;
    *End = 0;
    if (!LooksLikeRawMap(Map)) return;
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t GenMgr = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x40) : 0;
        if (!GenMgr) return;
        const uintptr_t Block = reinterpret_cast<AreaFn>(ExeBase() + kGenAreaFind)(reinterpret_cast<void*>(GenMgr), Map);
        if (!Block) return;
        const uintptr_t Start = *reinterpret_cast<const uintptr_t*>(Block + 0x18);
        const uint32_t Count = *reinterpret_cast<const uint32_t*>(Block + 0x20);
        if (!Start) return;
        *First = Start;
        *End = Start + static_cast<uintptr_t>(Count) * kGenRecordSize;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *First = 0;
        *End = 0;
    }
}

bool ReadTableHeadSafe(uintptr_t Mgr, uintptr_t* Entries, int32_t* Map) {
    __try {
        *Entries = *reinterpret_cast<const uintptr_t*>(Mgr + 0x10);
        *Map = *reinterpret_cast<const int32_t*>(Mgr + 0x18);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Records of [First, End) that say "killed since the last rest" (rec+0x76 & 3); -1 unreadable.
int CountDeadRecordsSafe(uintptr_t First, uintptr_t End) {
    if (!First || End <= First) return -1;
    __try {
        int Dead = 0;
        for (uintptr_t Rec = First; Rec < End; Rec += kGenRecordSize) {
            if (*reinterpret_cast<const uint8_t*>(Rec + 0x76) & 3) ++Dead;
        }
        return Dead;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

int ZeroEntriesOutsideSafe(uintptr_t Entries, uintptr_t First, uintptr_t End) {
    __try {
        int Zeroed = 0;
        for (uint32_t I = 0; I < kEnemyEntries; ++I) {
            uintptr_t* Record = reinterpret_cast<uintptr_t*>(Entries + I * kEnemyEntrySize + 0x10);
            if (!*Record) continue;
            const bool Live = First && *Record >= First && *Record < End && (*Record - First) % kGenRecordSize == 0;
            if (!Live) {
                *Record = 0;
                ++Zeroed;
            }
        }
        return Zeroed;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// --- standing again after a load -----------------------------------------------------
// Game thread only. Whatever puts things back waits kSettleMs from the moment the
// player stands again, even at a bonfire of the same map, where the map id never
// changes; the first tick after that also checks the enemy table for stale entries.
bool      g_wasStanding   = false;
bool      g_loadCheckDue  = false;
ULONGLONG g_standingSince = 0;

bool WatchStanding(ULONGLONG Now, bool* Arrived) {
    const bool Up = Standing();
    *Arrived = Up && !g_wasStanding;
    g_wasStanding = Up;
    if (*Arrived) {
        g_standingSince = Now;
        g_loadCheckDue = true;
    }
    return Up;
}

void NoteArrivalAfterTravel(ULONGLONG Now) {
    const ULONGLONG TravelAt = g_localTravelAt.load();
    if (!TravelAt || Now - TravelAt >= kTravelNoteMs) return;
    LOG_INFO("[TRAVEL] standing again %llu ms after my travel -- the partner and the shared enemies are put back "
             "no sooner than %llu ms from now", static_cast<unsigned long long>(Now - TravelAt),
             static_cast<unsigned long long>(kSettleMs));
}

// 3. A table filled for a map this player has left is emptied, whatever the partner
//    does -- and so is one that, after a load, still points at records that are gone.
//    Entries that point at nothing loaded are zeroed first; the reset runs whatever
//    that finds, because a table for another map must not stay attached. True when
//    the table was reset.
bool EmptyStaleTable(const EnemyTable& T, int32_t MyMap) {
    const bool LoadCheck = g_loadCheckDue && T.Ok;
    if (T.Ok) g_loadCheckDue = false;
    if (!T.Ok || T.State == 0) return false;
    const bool Elsewhere = LooksLikeRawMap(T.Area) && T.Area != MyMap;
    if (!Elsewhere && !LoadCheck) return false;
    const int Stale = ForgetStaleEnemyEntries(T.Mgr);
    if (!Elsewhere && Stale == 0) return false;   // after a load, every entry still points at loaded records
    const bool Done = ResetTableSafe(T.Mgr);
    LOG_INFO("[TRAVEL] the enemy sync table was for map %u and I stand in %u, %d of its entries into freed memory%s "
             "-- %s", RawToArea(T.Area), RawToArea(MyMap), Stale, Stale < 0 ? " (entries unreadable)" : "",
             Done ? "emptied" : "emptying it threw");
    return true;
}

// 3b. An attached table while the partner has stood in another map for kApartMs.
//     True when the table was let go.
bool DetachTableWhenApart(const EnemyTable& T, int32_t MyMap, ULONGLONG Now) {
    static ULONGLONG s_apartSince = 0;
    const ULONGLONG TheirAt = g_partnerMapAt.load();
    const int32_t Theirs = g_partnerMap.load();
    const bool Attached = T.Ok && (T.State == 1 || T.State == 2);
    const bool Apart = TheirAt && Now - TheirAt < kPartnerMapFresh && LooksLikeRawMap(Theirs) && Theirs != MyMap;
    if (!g_detachWhenApart.load() || !Attached || !Apart) {
        s_apartSince = 0;
        return false;
    }
    if (!s_apartSince) s_apartSince = Now;
    if (Now - s_apartSince < kApartMs) return false;
    s_apartSince = 0;
    const int Stale = ForgetStaleEnemyEntries(T.Mgr);
    const bool Done = DetachTableSafe(T.Mgr);
    LOG_INFO("[TRAVEL] the partner has stood in map %u for %llu s and I stand in %u -- the enemy sync table (state %d, "
             "map %u, %d entries into freed memory zeroed) let go, every enemy back under this game's control: %s",
             RawToArea(Theirs), kApartMs / 1000, RawToArea(MyMap), T.State, RawToArea(T.Area), Stale,
             Done ? "done" : "threw");
    return true;
}

// 4. The partner's character, when both share this map and it is missing here. True
//    when there is nothing more to do this tick.
bool RequeuePartnerIfMissing(uintptr_t List, int32_t MyMap, ULONGLONG Now) {
    bool HaveRecord = false;
    bool Present = false;
    {
        std::lock_guard<std::mutex> Lock(g_recordMutex);
        HaveRecord = g_haveRecord;
        if (HaveRecord) Present = PartnerCharacterHereSafe(List);
    }
    if (!HaveRecord) return true;
    if (Present) return false;
    static ULONGLONG s_requeuedAt = 0;
    if (Now - s_requeuedAt < kRetryMs) return true;
    s_requeuedAt = Now;
    uint64_t Queued = 0;
    bool Ran = false;
    {
        std::lock_guard<std::mutex> Lock(g_recordMutex);
        t_queueingOurselves = true;
        Ran = QueueRecordSafe(List, &Queued);
        t_queueingOurselves = false;
    }
    LOG_INFO("[TRAVEL] we share map %u and the partner has no character here -- its record queued again: %s",
             RawToArea(MyMap), !Ran ? "threw" : (Queued & 0xFF) ? "queued" : "the game had no free slot");
    return true;
}

} // namespace

// An entry is live when it points at one of the records of the generator block the
// game holds right now for the table's map (exe+0x517BF0 filled it from exactly that
// block). After a load the block is a new allocation, or none at all for a map left
// behind, so every old entry fails the test -- and so does every entry when the
// block cannot be looked up.
int ForgetStaleEnemyEntries(uintptr_t Mgr) {
    uintptr_t Entries = 0;
    int32_t Map = 0;
    if (!ReadTableHeadSafe(Mgr, &Entries, &Map)) return -1;
    if (!Entries) return 0;
    uintptr_t First = 0, End = 0;
    LiveRecordsSafe(Map, &First, &End);
    return ZeroEntriesOutsideSafe(Entries, First, End);
}
bool InstallTravelSync(bool Enabled, bool DetachWhenApart) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    g_detachWhenApart.store(DetachWhenApart);
    if (!Installed) {
        Installed = true;
        HookAt(kQueuePlayer, reinterpret_cast<void*>(&QueuePlayerDetour), reinterpret_cast<void**>(&g_queueOriginal),
               "the player character queue");
        HookAt(kEventPackets, reinterpret_cast<void*>(&EventPacketsDetour),
               reinterpret_cast<void**>(&g_eventPacketsOriginal), "the event-area packets");
        HookAt(kGenPackets, reinterpret_cast<void*>(&GenPacketsDetour),
               reinterpret_cast<void**>(&g_genPacketsOriginal), "the enemy generator packets");
    }
    LOG_INFO("[TRAVEL] travelling in a session: %s%s", Enabled
        ? "players stay together -- characters and shared enemies are put back when both share a map"
        : "off (travel_resync=false)",
        Enabled && DetachWhenApart ? "; enemies shared only while both stand in one map" : "");
    return g_queueOriginal != nullptr;
}

// Probe (17.09, point 3: after travelling away and back to the partner the bonfire
// travel pose stayed on). Travel start (exe+0x17F8B0) sets [[chr+0xC8]+0xFC] bit
// 0x4000000 and +0xF0 = 0x14; the action controller sends that state only on
// transitions, and a warp destroys the character mid-state, so the partner's copy
// may never hear "idle". For 20 s after either player travels, both characters'
// numbers are written down whenever they change.
namespace {
struct PoseNumbers {
    bool     Ok;
    uint32_t Flags;     // [[chr+0xC8]+0xFC]
    int32_t  Kind;      // [[chr+0xC8]+0xF0]
    uint32_t Bits;      // [[chr+0xB8]+0x4C8] & 0x30
};

PoseNumbers ReadPoseSafe(uintptr_t Chr) {
    PoseNumbers P{};
    if (!Chr) return P;
    __try {
        const uintptr_t Action = *reinterpret_cast<const uintptr_t*>(Chr + 0xC8);
        const uintptr_t Status = *reinterpret_cast<const uintptr_t*>(Chr + 0xB8);
        if (Action) {
            P.Flags = *reinterpret_cast<const uint32_t*>(Action + 0xFC);
            P.Kind = *reinterpret_cast<const int32_t*>(Action + 0xF0);
        }
        if (Status) P.Bits = *reinterpret_cast<const uint32_t*>(Status + 0x4C8) & 0x30;
        P.Ok = Action != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        P.Ok = false;
    }
    return P;
}

std::atomic<ULONGLONG> g_poseWatchUntil{ 0 };
constexpr ULONGLONG kPoseWatchMs = 20000;

// The fix (18.09, point 5: after the guest travelled, the host saw it frozen in the bonfire travel
// pose, and in PvP could not hit it). The copy's action controller keeps kind 0x14 from the travel
// start, and the partner's new character, idle from the start, never sends a transition that would
// end it. So once the partner has travelled and its copy here is still in kind 0x14 eight seconds
// later -- a travel is over in three, and a warp takes the character away -- the copy is put back
// the way the travel start set it: kind 0, the travel bit 0x4000000 of +0xFC cleared (ini
// travel_pose_fix).
std::atomic<bool>      g_poseFix{ true };
std::atomic<ULONGLONG> g_partnerTravelAt{ 0 };
constexpr ULONGLONG    kPoseStuckMs = 8000;
constexpr ULONGLONG    kPoseGiveUpMs = 60000;
constexpr int32_t      kTravelKind = 0x14;

bool ClearTravelPoseSafe(uintptr_t Chr, int32_t* KindBefore) {
    *KindBefore = -1;
    __try {
        const uintptr_t Action = *reinterpret_cast<const uintptr_t*>(Chr + 0xC8);
        if (!Action) return false;
        *KindBefore = *reinterpret_cast<const int32_t*>(Action + 0xF0);
        if (*KindBefore != kTravelKind) return false;
        *reinterpret_cast<int32_t*>(Action + 0xF0) = 0;
        *reinterpret_cast<uint32_t*>(Action + 0xFC) &= ~0x4000000u;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void PoseFixTick() {
    const ULONGLONG At = g_partnerTravelAt.load();
    if (!At || !g_poseFix.load()) return;
    const ULONGLONG Now = GetTickCount64();
    if (Now - At < kPoseStuckMs) return;
    if (Now - At > kPoseGiveUpMs) {
        g_partnerTravelAt.store(0);
        return;
    }
    const uintptr_t Partner = GetPartnerCharacter(1000);
    if (!Partner || !IsSessionPlayer(Partner)) return;
    int32_t Before = -1;
    if (!ClearTravelPoseSafe(Partner, &Before)) {
        if (Before >= 0 && Before != kTravelKind) g_partnerTravelAt.store(0);   // it moved on by itself
        return;
    }
    g_partnerTravelAt.store(0);
    WatchPoses();
    LOG_INFO("[POSE] the partner's copy was still in the bonfire travel pose %llu s after its travel -- put back "
             "to standing", static_cast<unsigned long long>((Now - At) / 1000));
}
} // namespace

void WatchPoses() {
    g_poseWatchUntil.store(GetTickCount64() + kPoseWatchMs);
}

void NotePartnerTravelForPose() {
    g_partnerTravelAt.store(GetTickCount64());
    WatchPoses();
}

void SetTravelPoseFix(bool On) {
    g_poseFix.store(On);
}

void PoseProbeTick() {
    PoseFixTick();
    const ULONGLONG Now = GetTickCount64();
    if (Now > g_poseWatchUntil.load()) return;
    static ULONGLONG s_at = 0;
    if (Now - s_at < 250) return;
    s_at = Now;
    uintptr_t Gm = 0, Local = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Local)) return;
    const uintptr_t Partner = GetPartnerCharacter(1000);
    const PoseNumbers Mine = ReadPoseSafe(Local);
    const PoseNumbers Theirs = ReadPoseSafe(Partner);
    static PoseNumbers s_mine{}, s_theirs{};
    const bool MineChanged = Mine.Ok && (Mine.Flags != s_mine.Flags || Mine.Kind != s_mine.Kind || Mine.Bits != s_mine.Bits);
    const bool TheirsChanged = Theirs.Ok && (Theirs.Flags != s_theirs.Flags || Theirs.Kind != s_theirs.Kind ||
                                             Theirs.Bits != s_theirs.Bits);
    if (!MineChanged && !TheirsChanged) return;
    if (Mine.Ok) s_mine = Mine;
    if (Theirs.Ok) s_theirs = Theirs;
    LOG_INFO("[POSE] me: action flags 0x%08X (travel bit %s), kind %d, status bits 0x%02X | partner's copy: %s "
             "flags 0x%08X (travel bit %s), kind %d, status bits 0x%02X",
             Mine.Flags, (Mine.Flags & 0x4000000) ? "on" : "off", Mine.Kind, Mine.Bits,
             Theirs.Ok ? "seen," : "not seen", Theirs.Flags, (Theirs.Flags & 0x4000000) ? "on" : "off", Theirs.Kind,
             Theirs.Bits);
}

bool PlayersShareMap() {
    int32_t MyMap = 0;
    const ULONGLONG TheirAt = g_partnerMapAt.load();
    return TheirAt && GetTickCount64() - TheirAt < kPartnerMapFresh && ReadLocalRawMap(&MyMap) &&
           g_partnerMap.load() == MyMap;
}

void NoteLocalTravel(int32_t RawMap) {
    g_localTravelAt.store(GetTickCount64());
    WatchPoses();
    LOG_INFO("[TRAVEL] travelling to map %u -- the partner's character goes with the old world; it comes back "
             "once we share a map", RawToArea(RawMap));
}

void NotePartnerRawMap(int32_t RawMap) {
    if (!LooksLikeRawMap(RawMap)) return;
    const int32_t Was = g_partnerMap.exchange(RawMap);
    g_partnerMapAt.store(GetTickCount64());
    if (Was != RawMap) {
        LOG_INFO("[TRAVEL] the partner stands in map %u", RawToArea(RawMap));
        WatchPoses();
    }
}

void TravelResyncTick() {
    if (!g_queueOriginal) return;
    auto& Lobby = Session::SessionManager::GetInstance();
    const ULONGLONG Now = GetTickCount64();
    bool Arrived = false;
    const bool Up = WatchStanding(Now, &Arrived);

    // Whether the partner is in this world at all, before anything else.
    uintptr_t JoinCtrl = 0;
    const int  Join  = JoinState(&JoinCtrl);
    const bool Guest = Join == 7;
    const bool Host  = Lobby.IsActive() && Lobby.IsHost();
    bool AnyAccept = false;
    const bool GuestIn = Host && GuestFullyInMyWorld(&AnyAccept);
    if (!Lobby.IsActive()) {
        ForgetRecord("the lobby is gone");
        g_partnerMap.store(0);
        g_partnerMapAt.store(0);
        return;
    }
    if (Host && !AnyAccept) ForgetRecord("no guest in my world any more");
    if (!Host && (Join < 0 || Join >= 8)) ForgetRecord("I am out of the host's world");
    if (!g_enabled.load()) return;

    if (!Up) return;
    int32_t MyMap = 0;
    if (!ReadLocalRawMap(&MyMap)) return;

    static int32_t   s_myMap = 0;
    static ULONGLONG s_myMapSince = 0, s_sentAt = 0, s_armedAt = 0;
    if (MyMap != s_myMap) {
        s_myMap = MyMap;
        s_myMapSince = Now;
        s_sentAt = 0;
        LOG_INFO("[TRAVEL] I stand in map %u", RawToArea(MyMap));
    }
    if (Now - s_sentAt >= kMapSendMs) {
        s_sentAt = Now;
        SendMap(MyMap);
    }
    if (Arrived) NoteArrivalAfterTravel(Now);
    LogDroppedMapPackets(Now, MyMap);

    // Probe (0.2.2 point 11): what the host's own map holds as killed when a guest is
    // all the way in, to compare with what the guest applies from the snapshot.
    static bool s_guestWasIn = false;
    if (GuestIn && !s_guestWasIn) {
        uintptr_t First = 0, End = 0;
        LiveRecordsSafe(MyMap, &First, &End);
        LOG_INFO("[TRAVEL] a guest is all the way in: %d of %llu enemy records of my map %u killed since my last rest",
                 CountDeadRecordsSafe(First, End), static_cast<unsigned long long>(First ? (End - First) / kGenRecordSize : 0),
                 RawToArea(MyMap));
    }
    s_guestWasIn = GuestIn;

    if (!Guest && !Host) return;

    const EnemyTable T = ReadEnemyTable();
    if (EmptyStaleTable(T, MyMap)) return;
    if (DetachTableWhenApart(T, MyMap, Now)) return;

    // A host puts nobody back while the guest is not fully in its world: the guest's
    // map packets say where it stands, and at home it may well stand in this map.
    if (Host && !GuestIn) return;
    const ULONGLONG TheirAt = g_partnerMapAt.load();
    const bool SameMap = TheirAt && Now - TheirAt < kPartnerMapFresh && g_partnerMap.load() == MyMap;
    if (!SameMap || Now - s_myMapSince < kSettleMs || Now - g_standingSince < kSettleMs) return;

    const uintptr_t List = PlayerList();
    if (!List || RequeuePartnerIfMissing(List, MyMap, Now)) return;

    // 5. Shared enemies again.
    if (T.Ok && T.State == 0 && !T.Armed && Now - s_armedAt >= kRetryMs) {
        s_armedAt = Now;
        if (Guest) EnemyReconcileNow();   // what the host has killed goes first (enemy_reconcile.cpp)
        const bool Done = ArmTableSafe(T.Mgr, Guest, JoinCtrl, MyMap);
        LOG_INFO("[TRAVEL] we share map %u with the partner's character here -- enemy sync armed again as the %s: %s",
                 RawToArea(MyMap), Guest ? "guest" : "host", Done ? "done" : "threw");
    }
}
} // namespace DS2Coop::Sync
