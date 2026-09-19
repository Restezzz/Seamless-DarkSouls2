// The map's objects as the host has them, in every map a guest loads (21.09, points 3, 4 and 9 of the
// report: "the shortcut bridge was down at the host and up at me -- a rejoin sorted it out", "a statue
// I turned with a Fragrant Branch stayed for the host until the thing that came out of it died", a
// shortcut gate opened in the host's world that the guest never saw move).
//
// The game tells the other players about a map object's state only when it changes: packets '$' '%'
// '&' ''' (MapStateActPacketReceiver, docs §3.52). What the host had already done before the guest
// came -- a lowered bridge, an opened gate, a statue already turned -- is in the snapshot the guest
// gets for the join map alone, and exe+0x1F31A0 restores nothing in a world entered by a multiplayer
// warp, so every other map stood as it loads from the guest's own save. This hands the states over the
// same way chest_lids.cpp hands over open chests, and for the same reason.
//
// What is handed over: every object of a loaded area that has a state machine (StateActCtrl, vtable
// exe+0x10CF668) -- gates, bridges, lifts, statues, doors -- except the ones an event marked as taking
// no state from the network (flag 0x80: Majula's lever gate and about twenty like it, which each
// player keeps for itself, §3.53). For a map the host does not have loaded, its save's compact record
// (exe+0x1F4F90) carries the same {id, state} pairs. A guest sets a state that differs from its own
// with exe+0x3C1EA0, the call the game itself uses to put an object where a snapshot says it is.

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
constexpr uint32_t  kStateActCtrlVt = 0x10CF668;   // StateActCtrl::vftable
constexpr uint32_t  kAreaByIndex    = 0x3BCE30;    // (mapMgr, index) -> area
constexpr uint32_t  kPrefabGet      = 0x1729A0;    // object of kind 4 -> prefab
constexpr uint32_t  kPrefabSub      = 0x449D00;    // (prefab, 0) -> the object inside
constexpr uint32_t  kStateComp      = 0x1CA790;    // (object + 0xB8, object) -> its state-act component
constexpr uint32_t  kObjStateSet    = 0x3C1EA0;    // (object, state, 0): put the object in that state
constexpr uint32_t  kSlotFind       = 0x1F4F90;    // (slots, 42, raw map) -> the save's compact record
constexpr uint32_t  kCompactSlots   = 42;
constexpr uint32_t  kCompactMax     = 0xC0;
constexpr int32_t   kKillSlots      = 0x2A;
constexpr uintptr_t kKillSlotSize   = 0xB10;
constexpr uint32_t  kNoNetworkFlag  = 0x80;
constexpr uint32_t  kMaxStates      = 250;         // one packet's entries
constexpr ULONGLONG kHostPassMs     = 3000;
constexpr ULONGLONG kGuestPassMs    = 5000;
constexpr int       kSendsPerPass   = 4;
constexpr uint32_t  kPacketMagic    = 0x44533243;

using AreaByIndexFn = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using ObjFn         = uintptr_t(__fastcall*)(uintptr_t);
using PrefabSubFn   = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using CompFn        = uintptr_t(__fastcall*)(uintptr_t, uintptr_t);
using ObjStateSetFn = void(__fastcall*)(uintptr_t, uint8_t, int32_t);
using SlotFindFn    = int32_t*(__fastcall*)(int32_t*, uint32_t, int32_t);

struct States {
    int32_t  Map;
    uint16_t Count;
    uint8_t  Source;   // 0 loaded on the host, 1 the host's save record
    Network::MapObjectState Entries[kMaxStates];
};

std::atomic<bool> g_enabled{ true };   // ini map_object_states

std::mutex                                              g_cacheMutex;
std::map<int32_t, std::vector<Network::MapObjectState>> g_hostStates;   // guest: sorted by id
std::atomic<bool>                                       g_cacheNew{ false };
std::atomic<uint32_t>                                   g_heard{ 0 };
std::map<int32_t, uint32_t>                             g_sent;         // host, game thread

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

// The object's state machine, or 0: only a real StateActCtrl, and only one that takes state from the
// network (an event marked the others as each player's own, §3.53).
uintptr_t SyncedCtrl(uintptr_t Obj) {
    const uintptr_t Comp = Game<CompFn>(kStateComp)(Obj + 0xB8, Obj);
    if (!Comp) return 0;
    const uintptr_t Ctrl = *reinterpret_cast<uintptr_t*>(Comp + 0x48);
    if (!Ctrl || *reinterpret_cast<uintptr_t*>(Ctrl) != ExeBase() + kStateActCtrlVt) return 0;
    if (*reinterpret_cast<uint32_t*>(Ctrl + 0x20) & kNoNetworkFlag) return 0;
    return Ctrl;
}

uint8_t StateOf(uintptr_t Ctrl) {
    return *reinterpret_cast<uint8_t*>(Ctrl + 0x1C);
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
void CollectAreaImpl(uintptr_t Area, States* Out) {
    Out->Map = *reinterpret_cast<int32_t*>(Area + 8);
    Out->Source = 0;
    Out->Count = 0;
    const uintptr_t* Objects = nullptr;
    uint32_t Count = 0;
    if (!AreaObjects(Area, &Objects, &Count)) return;
    for (uint32_t I = 0; I < Count && Out->Count < kMaxStates; ++I) {
        const uintptr_t Obj = ResolveObject(Objects[I]);
        if (!Obj) continue;
        const uintptr_t Ctrl = SyncedCtrl(Obj);
        if (!Ctrl) continue;
        const uint32_t Id = ObjectId(Obj);
        if (!Id) continue;
        Out->Entries[Out->Count].id = Id;
        Out->Entries[Out->Count].state = StateOf(Ctrl);
        ++Out->Count;
    }
}

int CollectLoadedSafe(States* Out, int Max, int32_t* Covered) {
    int N = 0;
    __try {
        const uintptr_t MapMgr = MapManager();
        const uintptr_t Areas = MapMgr ? *reinterpret_cast<uintptr_t*>(MapMgr + 8) : 0;
        if (!Areas) return 0;
        const int32_t AreaCount = *reinterpret_cast<int16_t*>(Areas + 0x1B6);
        for (int32_t A = 0; A < AreaCount && A < 128 && N < Max; ++A) {
            const uintptr_t Area = Game<AreaByIndexFn>(kAreaByIndex)(MapMgr, A);
            if (!Area || *reinterpret_cast<int8_t*>(Area + 0x1E0) <= 11) continue;
            CollectAreaImpl(Area, &Out[N]);
            if (!LooksLikeRawMap(Out[N].Map) || !Out[N].Count) continue;
            Covered[N] = Out[N].Map;
            ++N;
        }
        return N;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

// A map the host does not have loaded: its save's own record of that map's objects.
bool CollectSavedSafe(int32_t Map, States* Out) {
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
        for (uint32_t K = 0; K < N && Out->Count < kMaxStates; ++K) {
            const uint32_t Id = static_cast<uint32_t>(Compact[2 + 2 * K]) & 0x7FFFFFF;
            const uint8_t State = *(reinterpret_cast<const uint8_t*>(Compact) + 8 + K * 8 + 4);
            if (!Id) continue;
            Out->Entries[Out->Count].id = Id;
            Out->Entries[Out->Count].state = State;
            ++Out->Count;
        }
        return Out->Count != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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

uint32_t StatesCrc(const States& S) {
    uint32_t Hash = 2166136261u;
    const auto* P = reinterpret_cast<const uint8_t*>(S.Entries);
    const size_t Size = S.Count * sizeof(Network::MapObjectState);
    for (size_t I = 0; I < Size; ++I) {
        Hash ^= P[I];
        Hash *= 16777619u;
    }
    return Hash ^ S.Source ^ (static_cast<uint32_t>(S.Count) << 8);
}

void SendStates(const States& S) {
    Network::MapObjectStatesPacket Packet{};
    Packet.header.magic = kPacketMagic;
    Packet.header.type = Network::PacketType::MapObjectStates;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = S.Map;
    Packet.count = S.Count;
    Packet.source = S.Source;
    std::memcpy(Packet.entries, S.Entries, S.Count * sizeof(Network::MapObjectState));
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

    static States s_loaded[16];
    int32_t Covered[16] = {};
    const int Loaded = CollectLoadedSafe(s_loaded, 16, Covered);
    if (Loaded < 0) {
        LOG_WARNING("[MAPOBJ] reading my loaded areas' objects threw -- not handed over this time");
        return;
    }
    int Sends = 0;
    auto SendIfChanged = [&](const States& S) {
        if (Sends >= kSendsPerPass) return;
        const uint32_t Crc = StatesCrc(S);
        const auto It = g_sent.find(S.Map);
        if (It != g_sent.end() && It->second == Crc) return;
        SendStates(S);
        g_sent[S.Map] = Crc;
        ++Sends;
        LOG_INFO("[MAPOBJ] map %u: %u object states handed to the guest (%s)", MapNumber(S.Map), S.Count,
                 S.Source ? "from my save" : "loaded here");
    };
    for (int I = 0; I < Loaded; ++I) SendIfChanged(s_loaded[I]);

    int32_t Maps[kKillSlots] = {};
    const int MapCount = SaveMapsSafe(Maps, kKillSlots);
    static States s_saved;
    for (int I = 0; I < MapCount && Sends < kSendsPerPass; ++I) {
        if (std::find(Covered, Covered + Loaded, Maps[I]) != Covered + Loaded) continue;
        if (CollectSavedSafe(Maps[I], &s_saved)) SendIfChanged(s_saved);
    }
}

// --- the guest ------------------------------------------------------------------------------
struct GuestStats {
    uint32_t Seen;
    uint32_t Set;
    bool     Threw;
};

void ApplyAreaImpl(uintptr_t Area, const Network::MapObjectState* Host, uint32_t HostCount, GuestStats* S) {
    const uintptr_t* Objects = nullptr;
    uint32_t Count = 0;
    if (!AreaObjects(Area, &Objects, &Count)) return;
    for (uint32_t I = 0; I < Count; ++I) {
        const uintptr_t Obj = ResolveObject(Objects[I]);
        if (!Obj) continue;
        const uintptr_t Ctrl = SyncedCtrl(Obj);
        if (!Ctrl) continue;
        const uint32_t Id = ObjectId(Obj);
        if (!Id) continue;
        const auto* End = Host + HostCount;
        const auto* At = std::lower_bound(Host, End, Id, [](const Network::MapObjectState& E, uint32_t Want) {
            return E.id < Want;
        });
        if (At == End || At->id != Id) continue;
        S->Seen++;
        if (StateOf(Ctrl) == At->state) continue;
        Game<ObjStateSetFn>(kObjStateSet)(Obj, At->state, 0);
        S->Set++;
    }
}

bool ApplyAreaSafe(uintptr_t Area, const Network::MapObjectState* Host, uint32_t HostCount, GuestStats* S) {
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

void ApplyToArea(uintptr_t Area, int32_t Map, const char* When) {
    std::vector<Network::MapObjectState> Host;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        const auto It = g_hostStates.find(Map);
        if (It == g_hostStates.end() || It->second.empty()) return;
        Host = It->second;
    }
    GuestStats S{};
    ApplyAreaSafe(Area, Host.data(), static_cast<uint32_t>(Host.size()), &S);
    if (S.Threw) {
        LOG_ERROR("[MAPOBJ] map %u: putting the host's objects where it has them threw (%u done before)",
                  MapNumber(Map), S.Set);
        return;
    }
    if (S.Set) {
        LOG_INFO("[MAPOBJ] map %u %s: %u object(s) put where the host has them (%u of its %zu found here)",
                 MapNumber(Map), When, S.Set, S.Seen, Host.size());
    }
}

void GuestPass(ULONGLONG Now) {
    static ULONGLONG s_at = 0;
    const bool Fresh = g_cacheNew.exchange(false);
    if (!Fresh && Now - s_at < kGuestPassMs) return;
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

void Forget(const char* Why) {
    size_t Maps = 0;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        Maps = g_hostStates.size();
        g_hostStates.clear();
    }
    if (Maps) LOG_INFO("[MAPOBJ] the host's object states of %zu maps forgotten: %s", Maps, Why);
}

} // namespace

void SetMapObjectStates(bool On) {
    g_enabled.store(On);
    LOG_INFO("[MAPOBJ] the map's objects as the host has them: %s", On
             ? "in every map a guest loads (gates, bridges, lifts, statues)"
             : "only from the join (the game's way)");
}

void MapObjectsTick() {
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

void NoteHostMapObjects(int32_t Map, uint8_t Source, const void* Raw, uint16_t Count) {
    const auto* Entries = static_cast<const Network::MapObjectState*>(Raw);
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!g_enabled.load() || !Lobby.IsActive() || Lobby.IsHost() || !LooksLikeRawMap(Map) || Count > kMaxStates) {
        return;
    }
    std::vector<Network::MapObjectState> Sorted(Entries, Entries + Count);
    std::sort(Sorted.begin(), Sorted.end(),
              [](const Network::MapObjectState& A, const Network::MapObjectState& B) { return A.id < B.id; });
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        g_hostStates[Map] = std::move(Sorted);
    }
    g_cacheNew.store(true);
    const uint32_t N = g_heard.fetch_add(1) + 1;
    if (N <= 40 || N % 100 == 0) {
        LOG_INFO("[MAPOBJ] the host's map %u: %u object states (%s)", MapNumber(Map), Count,
                 Source ? "from its save" : "loaded there");
    }
}

} // namespace DS2Coop::Sync
