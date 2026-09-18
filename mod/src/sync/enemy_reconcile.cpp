// Enemies as the host has them, in every map a guest loads (docs §3.47).
//
// A guest hears about the host's enemies once: the join snapshot (exe+0x2C2FA0) brings the
// join map's generator live states and that map's kill counters, and nothing in the game
// carries either for another map -- the builders are fixed to the host's current map, the
// apply to join state 4, and the generator packets ('N'..'R', listener exe+0x1F6FD0) are
// routed through the join map. Every map a guest loads later in the host's world is made from
// defaults. Two things are handed over here instead, by the host, every few seconds when they
// change.
//
// Killed records. (u16)[rec+0x68] of every generator record the host has killed and not
// respawned yet: kind [rec+0x76]&3 set, stay bit 1 of [rec+0x7A] set, no-respawn
// [rec+0x76]&0xC clear -- the combination the spawner exe+0x418690 refuses to make a character
// for. From the host's loaded blocks ([genMgr+0x20+slot*8]: raw map +0x24, records
// [+0x18]+i*0xA0, [+0x20] of them) and from the three maps it keeps after leaving them
// ([[GMImp+0x38]+0x200]: map +0x18+k*0x10, record [[+0x20+k*0x10]] = map, u16 count, 0x34-byte
// entries from +0x10 as exe+0x40F770 writes them: id +4, kind +0xD&3, no-respawn +0xD&0xC,
// stay bit 5 of +0xF). A guest applies them the way the game's own 'Q' receiver does,
// exe+0x419460(genMgr, generator, 1, 0): the record ([generator+0x68]) is marked dead and its
// character despawned, and nothing is sent. Dead wins: a record the guest already has as dead
// is left alone, and nothing is ever revived.
//
// Kill counters. A boss is kept dead by its kill counter alone (limit 1, no spawn flag), so a
// boss the host had killed long ago stood in its arena for the guest, behind a fog only the
// guest saw (17.09, second report, point 3). The host's own store slots ([genMgr+0x10]+8+i*0xB10:
// raw map +0, u16 count +4, sorted u16 generator index +0x10, u8 kills +0x210) go over as they
// are. In a multiplayer world the game counts and checks kills in one session slot,
// store+0x1D0A8, and only for the map that slot names (exe+0x1F6F40); a world load clears it
// and names it with the join map (exe+0x1F6870). So a guest's session slot follows its map:
// written from the host's counters with exe+0x1F69E0 (the join snapshot's own copy) when it
// names another map, merged -- the higher count wins -- when it already names this one. Right
// before a map's generators are made, so a killed boss is not put in to begin with.

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
constexpr uint32_t  kAreaBySlot     = 0x3BCE60;   // (map manager, area slot) -> area; its raw map at +8
constexpr uint32_t  kGenOfHandle    = 0x17B7E0;   // (&[rec+0x10]) -> the record's generator, or 0
constexpr uint32_t  kGenKill        = 0x419460;   // (genMgr, generator, markDead, send)
constexpr uint32_t  kSlotCopy       = 0x1F69E0;   // (kill store, 0xB10 bytes): into the session slot
constexpr int32_t   kGenAreas       = 0x2A;
constexpr uint32_t  kGenRecordSize  = 0xA0;
constexpr uint32_t  kMaxRecords     = 256;
constexpr uint32_t  kCacheEntrySize = 0x34;
constexpr int       kCacheSlots     = 3;
constexpr uintptr_t kKillSlotSize   = 0xB10;
constexpr uintptr_t kSessionSlot    = 0x1D0A8;
constexpr uint32_t  kKillEntries    = 0x100;
constexpr ULONGLONG kHostPassMs     = 3000;
constexpr ULONGLONG kGuestPassMs    = 1000;
constexpr int       kSendsPerPass   = 8;
constexpr uint32_t  kPacketMagic    = 0x44533243;

using AreaBySlotFn = uintptr_t(__fastcall*)(uintptr_t, int32_t);
using GenOfFn      = uintptr_t(__fastcall*)(uintptr_t);
using GenKillFn    = void(__fastcall*)(uintptr_t, uintptr_t, uint8_t, uint8_t);
using SlotCopyFn   = void(__fastcall*)(uintptr_t, const uint8_t*);

std::atomic<bool> g_deadRecords{ true };   // ini enemy_dead_reconcile
std::atomic<bool> g_killCounts{ true };    // ini kill_counts_reconcile

// Guest: the host's latest per raw map (network thread writes, game thread reads).
struct HostKills {
    std::vector<uint16_t> Index;
    std::vector<uint8_t>  Kills;
};
std::mutex                               g_cacheMutex;
std::map<int32_t, std::vector<uint16_t>> g_deadByMap;
std::map<int32_t, HostKills>             g_killsByMap;
std::atomic<bool>                        g_cacheNew{ false };
std::atomic<uint32_t>                    g_deadHeard{ 0 };
std::atomic<uint32_t>                    g_killsHeard{ 0 };

// Host: what went out to the guest now in the world (game thread only).
std::map<int32_t, uint32_t> g_sentDead;
std::map<int32_t, uint32_t> g_sentKills;

struct DeadList {
    int32_t  Map;
    uint16_t Count;
    uint16_t Records;
    uint8_t  Source;
    uint16_t Ids[kMaxRecords];
};

struct KillList {
    int32_t  Map;
    uint16_t Count;
    uint16_t Index[kKillEntries];
    uint8_t  Kills[kKillEntries];
};

struct ApplyStats {
    uint32_t Found;
    uint32_t AlreadyDead;
    uint32_t Killed;
    uint32_t NoGenerator;
    bool     Threw;
};

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool ReadPtr(uintptr_t Addr, uintptr_t* Out) {
    __try {
        *Out = *reinterpret_cast<const uintptr_t*>(Addr);
        return *Out != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Out = 0;
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

bool LooksLikeRawMap(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) >= 10 && ((R >> 24) & 0xFF) <= 60 && ((R >> 16) & 0xFF) < 100 &&
           ((R >> 8) & 0xFF) < 100 && (R & 0xFF) < 100;
}

uint32_t MapNumber(int32_t Raw) {
    const uint32_t R = static_cast<uint32_t>(Raw);
    return ((R >> 24) & 0xFF) * 1000000u + ((R >> 16) & 0xFF) * 10000u + ((R >> 8) & 0xFF) * 100u + (R & 0xFF);
}

// The join controller's state; -1 with none.
int JoinState() {
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return -1;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return -1;
    int32_t State = -1;
    return ReadI32(Ctrl + 0xF8, &State) ? State : -1;
}

bool GuestInHostWorld() {
    auto& Lobby = Session::SessionManager::GetInstance();
    return Lobby.IsActive() && !Lobby.IsHost() && JoinState() == 7;
}

// The host's side: an accept controller that has taken the guest all the way in (0x10).
bool GuestFullyInMyWorldSafe() {
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

bool HostWithGuestIn() {
    auto& Lobby = Session::SessionManager::GetInstance();
    return Lobby.IsActive() && Lobby.IsHost() && GuestFullyInMyWorldSafe();
}

// The map this player stands in, the game's own value ([[[netRoot+0x20]+0x5B8]+0xC]).
bool ReadLocalMap(int32_t* Out) {
    uintptr_t Root = 0, List = 0, Local = 0;
    return ReadPtr(ExeBase() + kNetRoot, &Root) && ReadPtr(Root + 0x20, &List) && ReadPtr(List + 0x5B8, &Local) &&
           ReadI32(Local + 0xC, Out) && *Out != 0;
}

uintptr_t GeneratorManager() {
    uintptr_t Gm = 0, GenMgr = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x40, &GenMgr) ? GenMgr : 0;
}

uintptr_t MapManager() {
    uintptr_t Gm = 0, MapMgr = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x38, &MapMgr) ? MapMgr : 0;
}

uint32_t Fnv1a(const void* Data, size_t Size, uint32_t Hash) {
    const auto* P = static_cast<const uint8_t*>(Data);
    for (size_t I = 0; I < Size; ++I) {
        Hash ^= P[I];
        Hash *= 16777619u;
    }
    return Hash;
}

// --- reading the host's state (SEH, plain data only) ---------------------------------------
bool CollectBlockSafe(uintptr_t Block, DeadList* Out) {
    __try {
        Out->Map = *reinterpret_cast<const int32_t*>(Block + 0x24);
        const uintptr_t First = *reinterpret_cast<const uintptr_t*>(Block + 0x18);
        uint32_t N = *reinterpret_cast<const uint32_t*>(Block + 0x20);
        if (!First || !LooksLikeRawMap(Out->Map)) return false;
        if (N > kMaxRecords) N = kMaxRecords;
        Out->Records = static_cast<uint16_t>(N);
        Out->Source = 0;
        Out->Count = 0;
        for (uint32_t I = 0; I < N; ++I) {
            const uintptr_t Rec = First + I * kGenRecordSize;
            const int32_t Id = *reinterpret_cast<const int32_t*>(Rec + 0x68);
            const uint8_t Kind = *reinterpret_cast<const uint8_t*>(Rec + 0x76);
            const uint8_t Stay = *reinterpret_cast<const uint8_t*>(Rec + 0x7A);
            if (Id <= 0 || Id > 0xFFFF) continue;
            if ((Kind & 3) && (Stay & 2) && !(Kind & 0xC)) Out->Ids[Out->Count++] = static_cast<uint16_t>(Id);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// One of the three maps the host keeps after leaving them. -1: that slot is empty; 0: read;
// 1: unreadable or its record names another map.
int CollectCacheSafe(uintptr_t Cache, int K, DeadList* Out) {
    __try {
        const int32_t Map = *reinterpret_cast<const int32_t*>(Cache + 0x18 + K * 0x10);
        if (Map == -1 || Map == 0) return -1;
        const uintptr_t Holder = *reinterpret_cast<const uintptr_t*>(Cache + 0x20 + K * 0x10);
        const uintptr_t Rec = Holder ? *reinterpret_cast<const uintptr_t*>(Holder) : 0;
        if (!Rec) return -1;
        if (!LooksLikeRawMap(Map) || *reinterpret_cast<const int32_t*>(Rec) != Map) return 1;
        uint32_t N = *reinterpret_cast<const uint16_t*>(Rec + 4);
        if (N > kMaxRecords) N = kMaxRecords;
        Out->Map = Map;
        Out->Records = static_cast<uint16_t>(N);
        Out->Source = 1;
        Out->Count = 0;
        for (uint32_t I = 0; I < N; ++I) {
            const uintptr_t E = Rec + 0x10 + I * kCacheEntrySize;
            const uint16_t Id = *reinterpret_cast<const uint16_t*>(E + 4);
            const uint8_t Flags = *reinterpret_cast<const uint8_t*>(E + 0xD);
            const uint8_t More = *reinterpret_cast<const uint8_t*>(E + 0xF);
            if (!Id) continue;
            if ((Flags & 3) && ((More >> 5) & 1) && !(Flags & 0xC)) Out->Ids[Out->Count++] = Id;
        }
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 1;
    }
}

bool CollectKillSlotSafe(uintptr_t Slot, KillList* Out) {
    __try {
        Out->Map = *reinterpret_cast<const int32_t*>(Slot);
        const uint32_t N = *reinterpret_cast<const uint16_t*>(Slot + 4);
        if (!LooksLikeRawMap(Out->Map) || N == 0 || N > kKillEntries) return false;
        Out->Count = static_cast<uint16_t>(N);
        std::memcpy(Out->Index, reinterpret_cast<const void*>(Slot + 0x10), N * sizeof(uint16_t));
        std::memcpy(Out->Kills, reinterpret_cast<const void*>(Slot + 0x210), N);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- the host -------------------------------------------------------------------------------
void SendDead(const DeadList& L) {
    Network::EnemyDeadListPacket Packet{};
    Packet.header.magic = kPacketMagic;
    Packet.header.type = Network::PacketType::EnemyDeadList;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = L.Map;
    Packet.count = L.Count;
    Packet.source = L.Source;
    std::memcpy(Packet.ids, L.Ids, L.Count * sizeof(uint16_t));
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

void SendKills(const KillList& K) {
    Network::KillCountsPacket Packet{};
    Packet.header.magic = kPacketMagic;
    Packet.header.type = Network::PacketType::KillCounts;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.map = K.Map;
    Packet.count = K.Count;
    std::memcpy(Packet.index, K.Index, K.Count * sizeof(uint16_t));
    std::memcpy(Packet.kills, K.Kills, K.Count);
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

uint32_t DeadCrc(const DeadList& L) {
    uint32_t Hash = Fnv1a(&L.Source, 1, 2166136261u);
    Hash = Fnv1a(&L.Count, sizeof(L.Count), Hash);
    return Fnv1a(L.Ids, L.Count * sizeof(uint16_t), Hash);
}

uint32_t KillCrc(const KillList& K) {
    uint32_t Hash = Fnv1a(&K.Count, sizeof(K.Count), 2166136261u);
    Hash = Fnv1a(K.Index, K.Count * sizeof(uint16_t), Hash);
    return Fnv1a(K.Kills, K.Count, Hash);
}

void CollectHostDead(std::vector<DeadList>* Lists) {
    const uintptr_t GenMgr = GeneratorManager();
    if (!GenMgr) return;
    for (int32_t Slot = 0; Slot < kGenAreas; ++Slot) {
        uintptr_t Block = 0;
        if (!ReadPtr(GenMgr + 0x20 + static_cast<uintptr_t>(Slot) * 8, &Block)) continue;
        DeadList L{};
        if (CollectBlockSafe(Block, &L)) Lists->push_back(L);
    }
    const uintptr_t MapMgr = MapManager();
    uintptr_t Cache = 0;
    if (!MapMgr || !ReadPtr(MapMgr + 0x200, &Cache)) return;
    for (int K = 0; K < kCacheSlots; ++K) {
        DeadList L{};
        const int Got = CollectCacheSafe(Cache, K, &L);
        if (Got == 1) {
            static std::atomic<uint32_t> s_odd{ 0 };
            if (s_odd.fetch_add(1) < 3) {
                LOG_WARNING("[ENEMIES] the host's kept map %d is not laid out as expected -- not handed over", K);
            }
            continue;
        }
        if (Got != 0) continue;
        const bool Loaded = std::any_of(Lists->begin(), Lists->end(),
                                        [&](const DeadList& Have) { return Have.Map == L.Map && Have.Source == 0; });
        if (!Loaded) Lists->push_back(L);
    }
}

void CollectHostKills(std::vector<KillList>* Lists) {
    const uintptr_t GenMgr = GeneratorManager();
    uintptr_t Store = 0;
    if (!GenMgr || !ReadPtr(GenMgr + 0x10, &Store)) return;
    for (int32_t I = 0; I < kGenAreas; ++I) {
        KillList K{};
        if (CollectKillSlotSafe(Store + 8 + static_cast<uintptr_t>(I) * kKillSlotSize, &K)) Lists->push_back(K);
    }
}

void HostPass(ULONGLONG Now) {
    static bool s_guestWasIn = false;
    static ULONGLONG s_at = 0;
    const bool GuestIn = HostWithGuestIn();
    if (!GuestIn) {
        if (s_guestWasIn) {
            g_sentDead.clear();
            g_sentKills.clear();
        }
        s_guestWasIn = false;
        return;
    }
    if (!s_guestWasIn) {
        s_guestWasIn = true;
        g_sentDead.clear();
        g_sentKills.clear();
        s_at = 0;
    }
    if (Now - s_at < kHostPassMs) return;
    s_at = Now;

    int Sends = 0;
    uint32_t DeadMaps = 0, DeadIds = 0, KillMaps = 0;
    if (g_deadRecords.load()) {
        std::vector<DeadList> Lists;
        Lists.reserve(kGenAreas + kCacheSlots);
        CollectHostDead(&Lists);
        for (const DeadList& L : Lists) {
            if (Sends >= kSendsPerPass) break;
            const uint32_t Crc = DeadCrc(L);
            const auto It = g_sentDead.find(L.Map);
            if (It != g_sentDead.end() && It->second == Crc) continue;
            SendDead(L);
            g_sentDead[L.Map] = Crc;
            ++Sends;
            ++DeadMaps;
            DeadIds += L.Count;
            LOG_INFO("[ENEMIES] map %u (%s): %u of %u enemy records killed and not respawned -- handed to the guest",
                     MapNumber(L.Map), L.Source ? "a map I left, kept by the game" : "loaded", L.Count, L.Records);
        }
    }
    if (g_killCounts.load() && Sends < kSendsPerPass) {
        std::vector<KillList> Lists;
        Lists.reserve(kGenAreas);
        CollectHostKills(&Lists);
        for (const KillList& K : Lists) {
            if (Sends >= kSendsPerPass) break;
            const uint32_t Crc = KillCrc(K);
            const auto It = g_sentKills.find(K.Map);
            if (It != g_sentKills.end() && It->second == Crc) continue;
            SendKills(K);
            g_sentKills[K.Map] = Crc;
            ++Sends;
            ++KillMaps;
            uint32_t Killed = 0;
            for (uint32_t I = 0; I < K.Count; ++I) Killed += K.Kills[I] ? 1 : 0;
            LOG_INFO("[ENEMIES] map %u: kill counters of %u generators (%u killed at least once) -- handed to the "
                     "guest", MapNumber(K.Map), K.Count, Killed);
        }
    }
    (void)DeadMaps;
    (void)DeadIds;
    (void)KillMaps;
}

// --- the guest ------------------------------------------------------------------------------
void ApplyDeadToBlockSafe(uintptr_t GenMgr, uintptr_t Block, const uint16_t* Ids, uint32_t IdCount,
                          ApplyStats* S) {
    __try {
        const uintptr_t First = *reinterpret_cast<const uintptr_t*>(Block + 0x18);
        uint32_t N = *reinterpret_cast<const uint32_t*>(Block + 0x20);
        if (!First) return;
        if (N > kMaxRecords) N = kMaxRecords;
        for (uint32_t I = 0; I < N; ++I) {
            const uintptr_t Rec = First + I * kGenRecordSize;
            const int32_t Id = *reinterpret_cast<const int32_t*>(Rec + 0x68);
            if (Id <= 0 || Id > 0xFFFF || !std::binary_search(Ids, Ids + IdCount, static_cast<uint16_t>(Id))) continue;
            S->Found++;
            const uint8_t Kind = *reinterpret_cast<const uint8_t*>(Rec + 0x76);
            const uint8_t Stay = *reinterpret_cast<const uint8_t*>(Rec + 0x7A);
            if ((Kind & 3) && (Stay & 2)) {
                S->AlreadyDead++;
                continue;
            }
            const uintptr_t Gen = reinterpret_cast<GenOfFn>(ExeBase() + kGenOfHandle)(Rec + 0x10);
            if (!Gen || *reinterpret_cast<const uintptr_t*>(Gen + 0x68) != Rec) {
                S->NoGenerator++;
                continue;
            }
            reinterpret_cast<GenKillFn>(ExeBase() + kGenKill)(GenMgr, Gen, 1, 0);
            S->Killed++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S->Threw = true;
    }
}

bool BlockMapSafe(uintptr_t Block, int32_t* Map) {
    __try {
        *Map = *reinterpret_cast<const int32_t*>(Block + 0x24);
        return LooksLikeRawMap(*Map);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The host's killed records applied to one area slot's block. Logs what it changed.
void ApplyDeadToSlot(uintptr_t GenMgr, int32_t Slot, const char* When) {
    uintptr_t Block = 0;
    int32_t Map = 0;
    if (!ReadPtr(GenMgr + 0x20 + static_cast<uintptr_t>(Slot) * 8, &Block) || !BlockMapSafe(Block, &Map)) return;
    std::vector<uint16_t> Ids;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        const auto It = g_deadByMap.find(Map);
        if (It == g_deadByMap.end() || It->second.empty()) return;
        Ids = It->second;
    }
    ApplyStats S{};
    ApplyDeadToBlockSafe(GenMgr, Block, Ids.data(), static_cast<uint32_t>(Ids.size()), &S);
    if (S.Threw) {
        LOG_ERROR("[ENEMIES] map %u: marking the host's killed enemies dead threw (%u done before)", MapNumber(Map),
                  S.Killed);
        return;
    }
    if (S.Killed || S.NoGenerator) {
        LOG_INFO("[ENEMIES] map %u %s: %u enemies the host has killed marked dead here and taken away (%u of the "
                 "host's %zu found, %u already dead, %u without a generator)", MapNumber(Map), When, S.Killed,
                 S.Found, Ids.size(), S.AlreadyDead, S.NoGenerator);
    }
}

enum class SlotResult { Unreadable, Written, Merged, Unchanged };

SlotResult ApplyKillsSafe(uintptr_t Store, int32_t Map, const uint16_t* Index, const uint8_t* Kills, uint32_t Count,
                          uint8_t* Buffer, uint32_t* Raised, int32_t* WasMap, uint32_t* WasCount) {
    *Raised = 0;
    __try {
        const uintptr_t Slot = Store + kSessionSlot;
        *WasMap = *reinterpret_cast<const int32_t*>(Slot);
        const uint32_t N = *reinterpret_cast<const uint16_t*>(Slot + 4);
        *WasCount = N;
        if (*WasMap == Map && N > 0 && N <= kKillEntries) {
            const auto* SlotIndex = reinterpret_cast<const uint16_t*>(Slot + 0x10);
            auto* SlotKills = reinterpret_cast<uint8_t*>(Slot + 0x210);
            for (uint32_t I = 0; I < Count; ++I) {
                const uint16_t* At = std::lower_bound(SlotIndex, SlotIndex + N, Index[I]);
                if (At == SlotIndex + N || *At != Index[I]) continue;
                const size_t Pos = static_cast<size_t>(At - SlotIndex);
                if (SlotKills[Pos] < Kills[I]) {
                    SlotKills[Pos] = Kills[I];
                    ++*Raised;
                }
            }
            return *Raised ? SlotResult::Merged : SlotResult::Unchanged;
        }
        std::memset(Buffer, 0, kKillSlotSize);
        std::memcpy(Buffer, &Map, sizeof(Map));
        const uint16_t Count16 = static_cast<uint16_t>(Count);
        std::memcpy(Buffer + 4, &Count16, sizeof(Count16));
        std::memcpy(Buffer + 0x10, Index, Count * sizeof(uint16_t));
        std::memcpy(Buffer + 0x210, Kills, Count);
        reinterpret_cast<SlotCopyFn>(ExeBase() + kSlotCopy)(Store, Buffer);
        return SlotResult::Written;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return SlotResult::Unreadable;
    }
}

// The host's kill counters of `Map` into the session slot. False if the host sent none.
bool ApplyKillsForMap(int32_t Map, const char* When) {
    HostKills Kills;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        const auto It = g_killsByMap.find(Map);
        if (It == g_killsByMap.end()) return false;
        Kills = It->second;
    }
    const uintptr_t GenMgr = GeneratorManager();
    uintptr_t Store = 0;
    if (!GenMgr || !ReadPtr(GenMgr + 0x10, &Store)) return false;
    alignas(16) static uint8_t s_buffer[kKillSlotSize];
    uint32_t Raised = 0, WasCount = 0;
    int32_t WasMap = 0;
    const SlotResult R = ApplyKillsSafe(Store, Map, Kills.Index.data(), Kills.Kills.data(),
                                        static_cast<uint32_t>(Kills.Index.size()), s_buffer, &Raised, &WasMap,
                                        &WasCount);
    uint32_t Killed = 0;
    for (uint8_t K : Kills.Kills) Killed += K ? 1 : 0;
    switch (R) {
        case SlotResult::Written:
            LOG_INFO("[ENEMIES] map %u %s: the session's kill counters now the host's (%zu generators, %u killed at "
                     "least once); they named map %u with %u entries", MapNumber(Map), When, Kills.Index.size(),
                     Killed, MapNumber(WasMap), WasCount);
            break;
        case SlotResult::Merged:
            LOG_INFO("[ENEMIES] map %u %s: %u kill counters raised to the host's", MapNumber(Map), When, Raised);
            break;
        case SlotResult::Unreadable:
            LOG_ERROR("[ENEMIES] map %u %s: the session's kill counters could not be written", MapNumber(Map), When);
            break;
        case SlotResult::Unchanged:
            break;
    }
    return R != SlotResult::Unreadable;
}

bool SessionSlotMapSafe(int32_t* Map, uint32_t* Count) {
    const uintptr_t GenMgr = GeneratorManager();
    uintptr_t Store = 0;
    if (!GenMgr || !ReadPtr(GenMgr + 0x10, &Store)) return false;
    __try {
        *Map = *reinterpret_cast<const int32_t*>(Store + kSessionSlot);
        *Count = *reinterpret_cast<const uint16_t*>(Store + kSessionSlot + 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MapLoaded(uintptr_t GenMgr, int32_t Map) {
    for (int32_t Slot = 0; Slot < kGenAreas; ++Slot) {
        uintptr_t Block = 0;
        int32_t Have = 0;
        if (ReadPtr(GenMgr + 0x20 + static_cast<uintptr_t>(Slot) * 8, &Block) && BlockMapSafe(Block, &Have) &&
            Have == Map) {
            return true;
        }
    }
    return false;
}

bool AreaMapSafe(uintptr_t MapMgr, int32_t Slot, int32_t* Map) {
    __try {
        const uintptr_t Area = reinterpret_cast<AreaBySlotFn>(ExeBase() + kAreaBySlot)(MapMgr, Slot);
        if (!Area) return false;
        *Map = *reinterpret_cast<const int32_t*>(Area + 8);
        return LooksLikeRawMap(*Map);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SessionSlotSummarySafe(uintptr_t Store, int32_t* Map, uint32_t* Count, uint32_t* Kills) {
    __try {
        const uintptr_t Slot = Store + kSessionSlot;
        *Map = *reinterpret_cast<const int32_t*>(Slot);
        uint32_t N = *reinterpret_cast<const uint16_t*>(Slot + 4);
        if (N > kKillEntries) N = kKillEntries;
        *Count = N;
        *Kills = 0;
        for (uint32_t I = 0; I < N; ++I) *Kills += *reinterpret_cast<const uint8_t*>(Slot + 0x210 + I);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Probe: the session's kill counters whenever they change -- whether a guest's own kill is
// counted at all (exe+0x40FE7E) shows here as the sum going up right after one.
void LogSessionSlotChange(uintptr_t GenMgr) {
    static int32_t s_map = 0;
    static uint32_t s_count = 0, s_kills = 0;
    uintptr_t Store = 0;
    int32_t Map = 0;
    uint32_t Count = 0, Kills = 0;
    if (!ReadPtr(GenMgr + 0x10, &Store) || !SessionSlotSummarySafe(Store, &Map, &Count, &Kills)) return;
    if (Map == s_map && Count == s_count && Kills == s_kills) return;
    LOG_INFO("[ENEMIES] the session's kill counters: map %u, %u generators, %u kills in all (were map %u, %u, %u)",
             MapNumber(Map), Count, Kills, MapNumber(s_map), s_count, s_kills);
    s_map = Map;
    s_count = Count;
    s_kills = Kills;
}

void GuestPass(ULONGLONG Now) {
    static ULONGLONG s_at = 0;
    const bool Fresh = g_cacheNew.exchange(false);
    if (!Fresh && Now - s_at < kGuestPassMs) return;
    s_at = Now;
    const uintptr_t GenMgr = GeneratorManager();
    if (!GenMgr) return;
    LogSessionSlotChange(GenMgr);
    if (g_deadRecords.load()) {
        for (int32_t Slot = 0; Slot < kGenAreas; ++Slot) ApplyDeadToSlot(GenMgr, Slot, "(update)");
    }
    if (g_killCounts.load()) {
        int32_t Local = 0, SlotMap = 0;
        uint32_t SlotCount = 0;
        if (ReadLocalMap(&Local) && SessionSlotMapSafe(&SlotMap, &SlotCount) && (SlotMap != Local || Fresh)) {
            ApplyKillsForMap(Local, "(the map I stand in)");
        }
    }
}

} // namespace

bool InstallEnemyReconcile(bool DeadRecords, bool KillCounts) {
    g_deadRecords.store(DeadRecords);
    g_killCounts.store(KillCounts);
    LOG_INFO("[ENEMIES] a guest in the host's world: enemies the host has killed %s; kill counters (bosses) %s",
             DeadRecords ? "handed over for every map the guest loads" : "only from the join (the game's way)",
             KillCounts ? "handed over and followed map by map" : "only from the join (the game's way)");
    return true;
}

void EnemyReconcileTick() {
    if (!g_deadRecords.load() && !g_killCounts.load()) return;
    const ULONGLONG Now = GetTickCount64();
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) {
        ForgetHostEnemyStates("the lobby is gone");
        return;
    }
    if (Lobby.IsHost()) {
        HostPass(Now);
        return;
    }
    const int Join = JoinState();
    if (Join < 3 || Join > 7) {
        ForgetHostEnemyStates("out of the host's world");
        return;
    }
    if (Join == 7) GuestPass(Now);
}

void NoteHostEnemyDeadList(int32_t Map, uint8_t Source, const uint16_t* Ids, uint16_t Count) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!g_deadRecords.load() || !Lobby.IsActive() || Lobby.IsHost() || !LooksLikeRawMap(Map) ||
        Count > kMaxRecords) {
        return;
    }
    std::vector<uint16_t> Sorted(Ids, Ids + Count);
    std::sort(Sorted.begin(), Sorted.end());
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        g_deadByMap[Map] = std::move(Sorted);
    }
    g_cacheNew.store(true);
    const uint32_t N = g_deadHeard.fetch_add(1) + 1;
    if (N <= 30 || N % 100 == 0) {
        LOG_INFO("[ENEMIES] the host's map %u: %u enemies killed (%s)", MapNumber(Map), Count,
                 Source ? "a map it left" : "loaded there");
    }
}

void NoteHostKillCounts(int32_t Map, const uint16_t* Index, const uint8_t* Kills, uint16_t Count) {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!g_killCounts.load() || !Lobby.IsActive() || Lobby.IsHost() || !LooksLikeRawMap(Map) || Count == 0 ||
        Count > kKillEntries) {
        return;
    }
    for (uint16_t I = 1; I < Count; ++I) {
        if (Index[I] <= Index[I - 1]) {
            LOG_WARNING("[ENEMIES] the host's kill counters of map %u are not in order -- ignored", MapNumber(Map));
            return;
        }
    }
    HostKills K;
    K.Index.assign(Index, Index + Count);
    K.Kills.assign(Kills, Kills + Count);
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        g_killsByMap[Map] = std::move(K);
    }
    g_cacheNew.store(true);
    const uint32_t N = g_killsHeard.fetch_add(1) + 1;
    if (N <= 50 || N % 100 == 0) {
        LOG_INFO("[ENEMIES] the host's kill counters of map %u: %u generators", MapNumber(Map), Count);
    }
}

void ForgetHostEnemyStates(const char* Why) {
    g_sentDead.clear();   // the host: a rest changed the lists, everything goes out again
    size_t Maps = 0;
    {
        std::lock_guard<std::mutex> Lock(g_cacheMutex);
        Maps = g_deadByMap.size();
        g_deadByMap.clear();
    }
    if (Maps) LOG_INFO("[ENEMIES] the host's killed enemies of %zu maps forgotten: %s", Maps, Why);
}

// Right before an area's generators are made: the session's kill counters for its map.
void EnemyReconcileBeforeArea(void* Mgr, int32_t AreaIndex) {
    if (!g_killCounts.load() || !Mgr || AreaIndex < 0 || AreaIndex >= kGenAreas || !GuestInHostWorld()) return;
    const uintptr_t MapMgr = MapManager();
    int32_t Map = 0, Local = 0, SlotMap = 0;
    uint32_t SlotCount = 0;
    if (!MapMgr || !AreaMapSafe(MapMgr, AreaIndex, &Map) || !SessionSlotMapSafe(&SlotMap, &SlotCount)) return;
    const bool HaveLocal = ReadLocalMap(&Local);
    // The slot names one map. Taken for this one unless it names the map I stand in and
    // that one is still here -- a neighbour loading must not take it from under me.
    const bool Keep = SlotMap != Map && SlotCount > 0 && HaveLocal && SlotMap == Local && Map != Local &&
                      MapLoaded(reinterpret_cast<uintptr_t>(Mgr), SlotMap);
    if (Keep) {
        static std::atomic<uint32_t> s_kept{ 0 };
        if (s_kept.fetch_add(1) < 20) {
            LOG_INFO("[ENEMIES] map %u's generators made next to the map I stand in (%u) -- the session's kill "
                     "counters stay with that one", MapNumber(Map), MapNumber(Local));
        }
        return;
    }
    if (!ApplyKillsForMap(Map, "as its generators are made")) {
        static std::atomic<uint32_t> s_none{ 0 };
        if (s_none.fetch_add(1) < 20) {
            LOG_INFO("[ENEMIES] map %u's generators made with no kill counters from the host (the session's name "
                     "map %u)", MapNumber(Map), MapNumber(SlotMap));
        }
    }
}

// Right after: the host's killed records, before anything in the block has spawned.
void EnemyReconcileAfterArea(void* Mgr, int32_t AreaIndex) {
    if (!g_deadRecords.load() || !Mgr || AreaIndex < 0 || AreaIndex >= kGenAreas || !GuestInHostWorld()) return;
    const uintptr_t MapMgr = MapManager();
    uintptr_t Block = 0;
    int32_t AreaMap = 0, BlockMap = 0;
    if (MapMgr && AreaMapSafe(MapMgr, AreaIndex, &AreaMap) &&
        ReadPtr(reinterpret_cast<uintptr_t>(Mgr) + 0x20 + static_cast<uintptr_t>(AreaIndex) * 8, &Block) &&
        BlockMapSafe(Block, &BlockMap) && AreaMap != BlockMap) {
        static std::atomic<uint32_t> s_odd{ 0 };
        if (s_odd.fetch_add(1) < 5) {
            LOG_WARNING("[ENEMIES] area slot %d: the map table says map %u, its generators say map %u",
                        AreaIndex, MapNumber(AreaMap), MapNumber(BlockMap));
        }
    }
    ApplyDeadToSlot(reinterpret_cast<uintptr_t>(Mgr), AreaIndex, "as its generators were made");
}

// Game thread, before the enemy sync is armed again: what the host has killed first.
void EnemyReconcileNow() {
    if (!g_deadRecords.load() || !GuestInHostWorld()) return;
    const uintptr_t GenMgr = GeneratorManager();
    if (!GenMgr) return;
    for (int32_t Slot = 0; Slot < kGenAreas; ++Slot) ApplyDeadToSlot(GenMgr, Slot, "(before the enemy sync)");
}

} // namespace DS2Coop::Sync
