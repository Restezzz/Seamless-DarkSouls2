// Chest lids as the host has them, in every map a guest loads (docs §3.47; 17.09, second report,
// points 2 and 7).
//
// A guest gets the host's object states for the join map only (the snapshot), and an area loaded
// later in the host's world restores nothing: exe+0x1F31A0 returns at once in a world entered by a
// multiplayer warp. So a chest the host had opened long ago stood closed for the guest, and opening
// it asked the host's copy, which is open already -- nothing happened ("can't open old chests, the
// host has them open"). The host hands over which of its chests are open, map by map: the chests of
// its loaded areas as they are now (the lid, exe+0x3F2D30), and for every other map of its save the
// compact save record (exe+0x1F4F90: {u32 object id; u8 state} from +8, count at +4). A guest
// whose own lid is still closed takes the host's state with exe+0x3C1EA0 -- the lid handler
// (exe+0x1D0620) then does what it does for an open chest, and loot_sync.cpp's lid detour decides
// what lies in it by this player's own save: rolled for a chest never opened at home (point 7), the
// leftovers of one opened but not emptied, nothing for one already looted.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/utils.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kJoinCtrlVtable = 0x10D7BD8;
constexpr uint32_t  kAreaByIndex    = 0x3BCE30;   // (mapMgr, index) -> area
constexpr uint32_t  kPrefabGet      = 0x1729A0;   // object of kind 4 -> prefab
constexpr uint32_t  kPrefabSub      = 0x449D00;   // (prefab, 0) -> the object inside
constexpr uint32_t  kChestComp      = 0x1E5C80;   // object -> the treasure box component, or 0
constexpr uint32_t  kChestState     = 0x3F2D30;   // (component) -> the lid's state
constexpr uint32_t  kObjStateSet    = 0x3C1EA0;   // (object, state, 0): set the object's StateAct state
constexpr uint32_t  kSlotFind       = 0x1F4F90;   // (slots, 42, raw map) -> compact save record, or 0
constexpr uint32_t  kCompactSlots   = 42;
constexpr uint32_t  kCompactMax     = 0xC0;
constexpr int32_t   kKillSlots      = 0x2A;       // the kill store's own slots name the save's maps
constexpr uintptr_t kKillSlotSize   = 0xB10;
constexpr uint32_t  kMaxLids        = 200;        // one packet's entries
constexpr ULONGLONG kHostPassMs     = 3000;
constexpr ULONGLONG kGuestPassMs    = 1000;
constexpr int       kSendsPerPass   = 6;
constexpr uint32_t  kPacketMagic    = 0x44533243;

using AreaByIndexFn = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using ObjFn         = uintptr_t(__fastcall*)(uintptr_t);
using PrefabSubFn   = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using CompByteFn    = uint8_t(__fastcall*)(uintptr_t);
using ObjStateSetFn = void(__fastcall*)(uintptr_t, uint8_t, int32_t);
using SlotFindFn    = int32_t*(__fastcall*)(int32_t*, uint32_t, int32_t);

struct Lids {
    int32_t  Map;
    uint16_t Count;
    uint8_t  Source;   // 0 loaded on the host, 1 the host's save record
    Network::ChestLidEntry Entries[kMaxLids];
};

struct GuestStats {
    uint32_t Chests;
    uint32_t Opened;
    uint32_t AlreadyOpen;
    bool     Threw;
};

std::atomic<bool> g_enabled{ true };   // ini chest_lids_reconcile

std::mutex                                            g_cacheMutex;
std::map<int32_t, std::vector<Network::ChestLidEntry>> g_hostLids;   // guest: sorted by id
std::atomic<bool>                                     g_cacheNew{ false };
std::atomic<uint32_t>                                 g_heard{ 0 };
std::map<int32_t, uint32_t>                           g_sent;        // host, game thread

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

template <typename T> T Game(uint32_t Rva) { return reinterpret_cast<T>(ExeBase() + Rva); }

bool ReadPtr(uintptr_t Addr, uintptr_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uintptr_t*>(Addr);
        return *Out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Out = 0;
        return false;
    }
}

bool LooksLikeRawMap(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) >= 10 && ((R >> 24) & 0xFF) <= 60 && ((R >> 16) & 0xFF) < 100 &&
           ((R >> 8) & 0xFF) < 100 && (R & 0xFF) < 100;
}

uint32_t MapNumber(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
}

// The lid states worth handing over: open, settled open.
bool LidOpenState(uint32_t State) {
    const uint32_t Tens = State / 10 * 10;
    return Tens == 60 || Tens == 80 || Tens == 90 || Tens == 120;
}

bool LidClosedState(uint32_t State) {
    return State / 10 * 10 <= 10;
}

int JoinStateSafe() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -1;
    __try {
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool GuestFullyInSafe() {
    uintptr_t Root = 0, Mp = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return false;
    __try {
        uintptr_t It = *reinterpret_cast<const uintptr_t*>(Mp + 0x48);
        const uintptr_t End = *reinterpret_cast<const uintptr_t*>(Mp + 0x50);
        for (int Guard = 0; It && It < End && Guard < 16; It += 8, ++Guard) {
            const uintptr_t Ctrl = *reinterpret_cast<const uintptr_t*>(It);
            if (Ctrl && *reinterpret_cast<const int32_t*>(Ctrl + 0x150) == 0x10) return true;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return false;
}

uintptr_t MapManager() {
    uintptr_t Gm = 0, MapMgr = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x38, &MapMgr) ? MapMgr : 0;
}

uintptr_t ResolveObject(uintptr_t Obj) {
    if (!Obj) return 0;
    if (*reinterpret_cast<uint8_t*>(Obj + 0xA2) != 4) return Obj;
    const uintptr_t Prefab = Game<ObjFn>(kPrefabGet)(Obj);
    return Prefab ? Game<PrefabSubFn>(kPrefabSub)(Prefab, 0) : 0;
}

uint32_t ObjectId(uintptr_t Obj) {
    const uintptr_t Info = *reinterpret_cast<uintptr_t*>(Obj + 0x30);
    if (!Info || *reinterpret_cast<uint8_t*>(Info + 8) != 1) return 0;
    const uint32_t* Id = *reinterpret_cast<uint32_t* const*>(Info + 0x70);
    return Id ? (*Id & 0x7FFFFFF) : 0;
}

// The area's object list; false while the area is not loaded far enough.
bool AreaObjects(uintptr_t Area, const uintptr_t** Objects, uint32_t* Count) {
    if (!Area || *reinterpret_cast<int8_t*>(Area + 0x1E0) <= 11) return false;
    const uintptr_t List = *reinterpret_cast<uintptr_t*>(Area + 0x160);
    if (!List) return false;
    *Objects = *reinterpret_cast<uintptr_t* const*>(List + 0x10);
    *Count = *reinterpret_cast<uint32_t*>(List + 0x18);
    return *Objects != nullptr;
}

// --- the host -------------------------------------------------------------------------------
void CollectAreaLidsImpl(uintptr_t Area, Lids* Out) {
    Out->Map = *reinterpret_cast<int32_t*>(Area + 8);
    Out->Source = 0;
    Out->Count = 0;
    const uintptr_t* Objects = nullptr;
    uint32_t Count = 0;
    if (!AreaObjects(Area, &Objects, &Count)) return;
    for (uint32_t I = 0; I < Count && Out->Count < kMaxLids; ++I) {
        const uintptr_t Obj = ResolveObject(Objects[I]);
        if (!Obj) continue;
        const uintptr_t Box = Game<ObjFn>(kChestComp)(Obj);
        if (!Box) continue;
        const uint8_t State = Game<CompByteFn>(kChestState)(Box);
        const uint32_t Id = ObjectId(Obj);
        if (!Id || !LidOpenState(State)) continue;
        Out->Entries[Out->Count].id = Id;
        Out->Entries[Out->Count].state = State;
        ++Out->Count;
    }
}

// Every loaded area of the host's world. -1 on a fault.
int CollectLoadedSafe(Lids* Out, int Max, int32_t* Covered) {
    int N = 0;
    __try {
        const uintptr_t MapMgr = MapManager();
        const uintptr_t Areas = MapMgr ? *reinterpret_cast<uintptr_t*>(MapMgr + 8) : 0;
        if (!Areas) return 0;
        const int32_t AreaCount = *reinterpret_cast<int16_t*>(Areas + 0x1B6);
        for (int32_t A = 0; A < AreaCount && A < 128 && N < Max; ++A) {
            const uintptr_t Area = Game<AreaByIndexFn>(kAreaByIndex)(MapMgr, A);
            if (!Area || *reinterpret_cast<int8_t*>(Area + 0x1E0) <= 11) continue;
            CollectAreaLidsImpl(Area, &Out[N]);
            if (!LooksLikeRawMap(Out[N].Map)) continue;
            Covered[N] = Out[N].Map;
            ++N;
        }
        return N;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// The host's compact save record of one map: its open chests (and anything else in an open
// state -- the guest only ever touches chests).
bool CollectSavedSafe(int32_t Map, Lids* Out) {
    __try {
        const uintptr_t MapMgr = MapManager();
        const uintptr_t StateMgr = MapMgr ? *reinterpret_cast<uintptr_t*>(MapMgr + 0x1F8) : 0;
        if (!StateMgr) return false;
        const int32_t* Compact = Game<SlotFindFn>(kSlotFind)(reinterpret_cast<int32_t*>(StateMgr + 0x24),
                                                           kCompactSlots, Map);
        if (!Compact || Compact[0] != Map) return false;
        uint32_t N = static_cast<uint32_t>(Compact[1]);
        if (N > kCompactMax) N = kCompactMax;
        Out->Map = Map;
        Out->Source = 1;
        Out->Count = 0;
        for (uint32_t K = 0; K < N && Out->Count < kMaxLids; ++K) {
            const uint32_t Id = static_cast<uint32_t>(Compact[2 + 2 * K]) & 0x7FFFFFF;
            const uint8_t State = *(reinterpret_cast<const uint8_t*>(Compact) + 8 + K * 8 + 4);
            if (!Id || !LidOpenState(State)) continue;
            Out->Entries[Out->Count].id = Id;
            Out->Entries[Out->Count].state = State;
            ++Out->Count;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The maps of the host's save: the raw map ids its kill store's own slots name.
int SaveMapsSafe(int32_t* Out, int Max) {
    int N = 0;
    uintptr_t Gm = 0, GenMgr = 0, Store = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0x40, &GenMgr) || !ReadPtr(GenMgr + 0x10, &Store)) {
        return 0;
    }
    __try {
        for (int32_t I = 0; I < kKillSlots && N < Max; ++I) {
            const int32_t Map = *reinterpret_cast<const int32_t*>(Store + 8 + I * kKillSlotSize);
            if (LooksLikeRawMap(Map)) Out[N++] = Map;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return N;
}

uint32_t LidsCrc(const Lids& L) {
    uint32_t Hash = 2166136261u;
    const auto* P = reinterpret_cast<const uint8_t*>(L.Entries);
    const size_t Size = L.Count * sizeof(Network::ChestLidEntry);
    for (size_t I = 0; I < Size; ++I) {
        Hash ^= P[I];
        Hash *= 16777619u;
    }
    return Hash ^ L.Source ^ (static_cast<uint32_t>(L.Count) << 8);
}

void SendLids(const Lids& L) {
    Network::ChestLidsPacket Packet{};
    Packet.header.magic = kPacketMagic;
    Packet.header.type = Network::PacketType::ChestLids;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = L.Map;
    Packet.count = L.Count;
    Packet.source = L.Source;
    std::memcpy(Packet.entries, L.Entries, L.Count * sizeof(Network::ChestLidEntry));
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

void HostPass(ULONGLONG Now) {
    static bool s_guestWasIn = false;
    static ULONGLONG s_at = 0;
    auto& Lobby = Session::SessionManager::GetInstance();
    const bool GuestIn = Lobby.IsActive() && Lobby.IsHost() && GuestFullyInSafe();
    if (!GuestIn) {
        if (s_guestWasIn) g_sent.clear();
        s_guestWasIn = false;
        return;
    }
    if (!s_guestWasIn) {
        s_guestWasIn = true;
        g_sent.clear();
        s_at = 0;
    }
    if (Now - s_at < kHostPassMs) return;
    s_at = Now;

    static Lids s_loaded[16];
    int32_t Covered[16] = {};
    const int Loaded = CollectLoadedSafe(s_loaded, 16, Covered);
    if (Loaded < 0) {
        LOG_WARNING("[LOOT] reading my loaded areas' chests threw -- not handed over this time");
        return;
    }
    int Sends = 0;
    auto SendIfChanged = [&](const Lids& L) {
        if (Sends >= kSendsPerPass) return;
        const uint32_t Crc = LidsCrc(L);
        const auto It = g_sent.find(L.Map);
        if (It != g_sent.end() && It->second == Crc) return;
        SendLids(L);
        g_sent[L.Map] = Crc;
        ++Sends;
        LOG_INFO("[LOOT] map %u: %u open chests handed to the guest (%s)", MapNumber(L.Map), L.Count,
                 L.Source ? "from my save" : "loaded here");
    };
    for (int I = 0; I < Loaded; ++I) SendIfChanged(s_loaded[I]);

    int32_t Maps[kKillSlots] = {};
    const int MapCount = SaveMapsSafe(Maps, kKillSlots);
    static Lids s_saved;
    for (int I = 0; I < MapCount && Sends < kSendsPerPass; ++I) {
        if (std::find(Covered, Covered + Loaded, Maps[I]) != Covered + Loaded) continue;
        if (CollectSavedSafe(Maps[I], &s_saved)) SendIfChanged(s_saved);
    }
}

// --- the guest ------------------------------------------------------------------------------
void ApplyAreaImpl(uintptr_t Area, const Network::ChestLidEntry* Host, uint32_t HostCount, GuestStats* S) {
    const uintptr_t* Objects = nullptr;
    uint32_t Count = 0;
    if (!AreaObjects(Area, &Objects, &Count)) return;
    for (uint32_t I = 0; I < Count; ++I) {
        const uintptr_t Obj = ResolveObject(Objects[I]);
        if (!Obj) continue;
        const uintptr_t Box = Game<ObjFn>(kChestComp)(Obj);
        if (!Box) continue;
        const uint32_t Id = ObjectId(Obj);
        if (!Id) continue;
        const auto* End = Host + HostCount;
        const auto* At = std::lower_bound(Host, End, Id, [](const Network::ChestLidEntry& E, uint32_t Want) {
            return E.id < Want;
        });
        if (At == End || At->id != Id) continue;
        S->Chests++;
        const uint8_t Mine = Game<CompByteFn>(kChestState)(Box);
        if (!LidClosedState(Mine)) {
            S->AlreadyOpen++;
            continue;
        }
        Game<ObjStateSetFn>(kObjStateSet)(Obj, At->state, 0);
        S->Opened++;
    }
}

bool ApplyAreaSafe(uintptr_t Area, const Network::ChestLidEntry* Host, uint32_t HostCount, GuestStats* S) {
    __try {
        ApplyAreaImpl(Area, Host, HostCount, S);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S->Threw = true;
        return false;
    }
}

bool AreaMapSafe(uintptr_t MapMgr, int32_t Index, uintptr_t* Area, int32_t* Map) {
    __try {
        *Area = Game<AreaByIndexFn>(kAreaByIndex)(MapMgr, Index);
        if (!*Area || *reinterpret_cast<int8_t*>(*Area + 0x1E0) <= 11) return false;
        *Map = *reinterpret_cast<int32_t*>(*Area + 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

int AreaCountSafe(uintptr_t MapMgr) {
    __try {
        const uintptr_t Areas = *reinterpret_cast<uintptr_t*>(MapMgr + 8);
        return Areas ? *reinterpret_cast<int16_t*>(Areas + 0x1B6) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The host's open chests applied to one loaded area of this guest's copy of the host's world.
void ApplyToArea(uintptr_t Area, int32_t Map, const char* When) {
    std::vector<Network::ChestLidEntry> Host;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        const auto It = g_hostLids.find(Map);
        if (It == g_hostLids.end() || It->second.empty()) return;
        Host = It->second;
    }
    GuestStats S{};
    ApplyAreaSafe(Area, Host.data(), static_cast<uint32_t>(Host.size()), &S);
    if (S.Threw) {
        LOG_ERROR("[LOOT] map %u: opening the chests the host has open threw (%u done before)", MapNumber(Map),
                  S.Opened);
        return;
    }
    if (S.Opened) {
        LOG_INFO("[LOOT] map %u %s: %u chests the host has open were closed here -- opened as the host has them "
                 "(%u of the host's %zu found, %u already open); what lies in them follows your own save",
                 MapNumber(Map), When, S.Opened, S.Chests, Host.size(), S.AlreadyOpen);
    }
}

void GuestPass(ULONGLONG Now) {
    static ULONGLONG s_at = 0;
    const bool Fresh = g_cacheNew.exchange(false);
    if (!Fresh && Now - s_at < kGuestPassMs * 5) return;
    s_at = Now;
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return;
    const int Count = AreaCountSafe(MapMgr);
    for (int32_t A = 0; A < Count && A < 128; ++A) {
        uintptr_t Area = 0;
        int32_t Map = 0;
        if (AreaMapSafe(MapMgr, A, &Area, &Map)) ApplyToArea(Area, Map, "(update)");
    }
}

bool GuestInHostWorld() {
    auto& Lobby = Session::SessionManager::GetInstance();
    return Lobby.IsActive() && !Lobby.IsHost() && JoinStateSafe() == 7;
}

void Forget(const char* Why) {
    size_t Maps = 0;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        Maps = g_hostLids.size();
        g_hostLids.clear();
    }
    if (Maps) LOG_INFO("[LOOT] the host's open chests of %zu maps forgotten: %s", Maps, Why);
}

} // namespace

void SetChestLidsReconcile(bool On) {
    g_enabled.store(On);
    LOG_INFO("[LOOT] chests the host has open: %s", On ? "opened for a guest in every map it loads"
                                                        : "only from the join (the game's way)");
}

void ChestLidsTick() {
    if (!g_enabled.load()) return;
    const ULONGLONG Now = GetTickCount64();
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) {
        Forget("the lobby is gone");
        return;
    }
    if (Lobby.IsHost()) {
        HostPass(Now);
        return;
    }
    const int Join = JoinStateSafe();
    if (Join < 3 || Join > 7) {
        Forget("out of the host's world");
        return;
    }
    if (Join == 7) GuestPass(Now);
}

// Right after an area's objects were restored (exe+0x1F31A0): the host's open chests there.
void ApplyHostChestLids(uint32_t AreaId) {
    if (!g_enabled.load() || !GuestInHostWorld()) return;
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return;
    const int Count = AreaCountSafe(MapMgr);
    for (int32_t A = 0; A < Count && A < 128; ++A) {
        uintptr_t Area = 0;
        int32_t Map = 0;
        if (AreaMapSafe(MapMgr, A, &Area, &Map) && static_cast<uint32_t>(Map) == AreaId) {
            ApplyToArea(Area, Map, "as it loaded");
            return;
        }
    }
}

void NoteHostChestLids(int32_t Map, uint8_t Source, const void* Raw, uint16_t Count) {
    const auto* Entries = static_cast<const Network::ChestLidEntry*>(Raw);
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!g_enabled.load() || !Lobby.IsActive() || Lobby.IsHost() || !LooksLikeRawMap(Map) || Count > kMaxLids) {
        return;
    }
    std::vector<Network::ChestLidEntry> Sorted(Entries, Entries + Count);
    std::sort(Sorted.begin(), Sorted.end(),
              [](const Network::ChestLidEntry& A, const Network::ChestLidEntry& B) { return A.id < B.id; });
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        g_hostLids[Map] = std::move(Sorted);
    }
    g_cacheNew.store(true);
    const uint32_t N = g_heard.fetch_add(1) + 1;
    if (N <= 40 || N % 100 == 0) {
        LOG_INFO("[LOOT] the host's map %u: %u open chests (%s)", MapNumber(Map), Count,
                 Source ? "from its save" : "loaded there");
    }
}

} // namespace DS2Coop::Sync
