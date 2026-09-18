// A bonfire one player lights is lit for the other, with a notification (0.2.2 point 12).
//
// Kindling ends in exe+0x1CAF50(bonfire component, actor): a bonfire the list already
// calls lit is left alone; otherwise exe+0x17E320(list, id, local) sets bit 0 of the
// byte the list reads (record + [list+0x44] + 2: +0x02 the save's own set, +0x03 the
// session's set a guest in the host's world reads), and the component plays the
// ignition effect (0x1FA9) and turns its flame on (StateAct state 0x1E). `local` is 1
// only when the actor is this player ([GMImp+0xD0]); the partner's kindle replayed in
// the same map arrives with 0. Lighting is no event flag -- flag sync never carried it.
//
// So a local light (the detour on exe+0x17E320) goes out as packet BonfireLit, and the
// partner's is applied here on the game thread:
//   * the bonfire loaded here and not lit: exe+0x1CAF50(component, component) -- any
//     actor that is not the local player takes the game's own non-local path: byte,
//     effect and flame, no packet back, no last-bonfire record;
//   * not loaded: the byte alone, the bonfire is built lit;
//   * lit already but loaded: exe+0x1CADF0(component) sets the flame from the byte.
// A guest in the host's world reads the session byte, which a load empties and the mod
// refills from the host's list; its own save's byte (+0x02), like the host's own save
// byte, is only written with progress sharing on (ini flag_sync).

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
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kLitSet         = 0x17E320;   // (bonfire list, id, local)
constexpr uint32_t  kRecordFind     = 0x17F1C0;   // (bonfire list, id) -> record, or 0
constexpr uint32_t  kKindle         = 0x1CAF50;   // (bonfire component, actor) -> lit now
constexpr uint32_t  kFlameFromByte  = 0x1CADF0;   // (bonfire component)
constexpr uint32_t  kObjRow         = 0x3BA6A0;   // (object) -> param row, +0 the bonfire id
constexpr int       kQueueSize      = 16;
constexpr ULONGLONG kKeepMs         = 10 * 60 * 1000;

using LitSetFn   = void(__fastcall*)(void* list, int32_t id, uint8_t local);
using FindFn     = uintptr_t(__fastcall*)(uintptr_t list, int32_t id);
using KindleFn   = uint64_t(__fastcall*)(uintptr_t component, uintptr_t actor);
using CompFn     = void(__fastcall*)(uintptr_t component);
using RowFn      = const int32_t*(__fastcall*)(uintptr_t object);

LitSetFn g_litSetOriginal = nullptr;

struct Lit {
    int32_t   Id;
    int32_t   Map;
    ULONGLONG At;
    char      From[32];
};

std::mutex g_mutex;
Lit        g_own[kQueueSize];       // lit here, to send
int        g_ownCount = 0;
Lit        g_partner[kQueueSize];   // lit by the partner, to apply
int        g_partnerCount = 0;

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

bool ReadLocalRawMapSafe(int32_t* Out) {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        const uintptr_t List = Root ? *reinterpret_cast<const uintptr_t*>(Root + 0x20) : 0;
        const uintptr_t Local = List ? *reinterpret_cast<const uintptr_t*>(List + 0x5B8) : 0;
        if (!Local) return false;
        *Out = *reinterpret_cast<const int32_t*>(Local + 0xC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall LitSetDetour(void* List, int32_t Id, uint8_t Local) {
    g_litSetOriginal(List, Id, Local);
    if (!Local || !Session::SessionManager::GetInstance().IsActive()) return;
    int32_t Map = 0;
    ReadLocalRawMapSafe(&Map);
    std::lock_guard<std::mutex> Lock(g_mutex);
    if (g_ownCount < kQueueSize) g_own[g_ownCount++] = Lit{ Id, Map, GetTickCount64(), {} };
}

struct ApplyResult {
    bool Known;       // the id is a bonfire of this save's list
    bool WasLit;      // the byte the list reads was lit already
    bool Loaded;      // the bonfire's object is in a loaded map here
    bool Kindled;     // exe+0x1CAF50 lit it
    bool OwnSaved;    // the save's own byte newly set
    int  View;
};

// The bonfire's component among the loaded ones ([list+0x08], next at +0x60).
uintptr_t FindLoadedBonfire(uintptr_t List, int32_t Id) {
    uintptr_t Comp = *reinterpret_cast<const uintptr_t*>(List + 0x08);
    for (int Guard = 0; Comp && Guard < 512; ++Guard) {
        const uintptr_t Object = *reinterpret_cast<const uintptr_t*>(Comp + 8);
        const int32_t* Row = Object ? reinterpret_cast<RowFn>(ExeBase() + kObjRow)(Object) : nullptr;
        if (Row && *Row == Id) return Comp;
        Comp = *reinterpret_cast<const uintptr_t*>(Comp + 0x60);
    }
    return 0;
}

bool ApplyPartnerLitSafe(int32_t Id, bool Share, ApplyResult* R) {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        const uintptr_t Events = Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0x70) : 0;
        const uintptr_t List = Events ? *reinterpret_cast<const uintptr_t*>(Events + 0x58) : 0;
        if (!List) return false;
        const uintptr_t Record = reinterpret_cast<FindFn>(ExeBase() + kRecordFind)(List, Id);
        if (!Record) return true;
        R->Known = true;
        R->View = *reinterpret_cast<const int32_t*>(List + 0x44);
        uint8_t* Read = reinterpret_cast<uint8_t*>(Record + 2 + (R->View == 1 ? 1 : 0));
        uint8_t* Own = reinterpret_cast<uint8_t*>(Record + 2);
        R->WasLit = (*Read & 1) != 0;
        const uintptr_t Comp = FindLoadedBonfire(List, Id);
        R->Loaded = Comp != 0;
        if (R->View == 0 && !Share) return true;   // the read byte is this save's own: only with sharing
        if (!R->WasLit && Comp) {
            R->Kindled = (reinterpret_cast<KindleFn>(ExeBase() + kKindle)(Comp, Comp) & 0xFF) != 0;
        } else if (!R->WasLit) {
            *Read |= 1;
        } else if (Comp) {
            reinterpret_cast<CompFn>(ExeBase() + kFlameFromByte)(Comp);
        }
        if (Share && !(*Own & 1)) {
            *Own |= 1;
            R->OwnSaved = true;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string BonfireLabel(int32_t Id) {
    const std::string Name = GameBonfireName(Id);
    char Buffer[128];
    if (!Name.empty()) {
        std::snprintf(Buffer, sizeof(Buffer), "\xC2\xAB%s\xC2\xBB", Name.c_str());
    } else {
        std::snprintf(Buffer, sizeof(Buffer), "%d", Id);
    }
    return Buffer;
}

void SendLit(const Lit& L) {
    Network::BonfireLitPacket Packet{};
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::BonfireLit;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.id = L.Id;
    Packet.map = L.Map;
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

} // namespace

bool InstallBonfireLit() {
    static bool Installed = false;
    if (Installed) return g_litSetOriginal != nullptr;
    Installed = true;
    const bool Ok = Hooks::HookManager::GetInstance().InstallHook(
        reinterpret_cast<void*>(ExeBase() + kLitSet), reinterpret_cast<void*>(&LitSetDetour),
        reinterpret_cast<void**>(&g_litSetOriginal));
    if (!Ok) g_litSetOriginal = nullptr;
    LOG_INFO("[BONFIRE] lighting exe+0x%X %s", kLitSet,
             Ok ? "hooked: a bonfire one player lights is lit for the other" : "NOT hooked");
    return Ok;
}

void NotePartnerBonfireLit(int32_t Id, int32_t Map, const std::string& From) {
    std::lock_guard<std::mutex> Lock(g_mutex);
    if (g_partnerCount >= kQueueSize) return;
    Lit L{ Id, Map, GetTickCount64(), {} };
    strncpy_s(L.From, sizeof(L.From), From.c_str(), _TRUNCATE);
    g_partner[g_partnerCount++] = L;
}

// Game thread.
void BonfireLitGameTick() {
    Lit Own[kQueueSize], Partner[kQueueSize];
    int OwnCount = 0, PartnerCount = 0;
    {
        std::lock_guard<std::mutex> Lock(g_mutex);
        OwnCount = g_ownCount;
        for (int I = 0; I < OwnCount; ++I) Own[I] = g_own[I];
        g_ownCount = 0;
        PartnerCount = g_partnerCount;
        for (int I = 0; I < PartnerCount; ++I) Partner[I] = g_partner[I];
        g_partnerCount = 0;
    }
    for (int I = 0; I < OwnCount; ++I) {
        SendLit(Own[I]);
        LOG_INFO("[BONFIRE] I lit bonfire %d -- telling the other player", Own[I].Id);
    }
    if (!PartnerCount) return;

    const bool Share = IsProgressSharingOn();
    const ULONGLONG Now = GetTickCount64();
    Lit Keep[kQueueSize];
    int KeepCount = 0;
    for (int I = 0; I < PartnerCount; ++I) {
        const Lit& L = Partner[I];
        ApplyResult R{};
        if (!ApplyPartnerLitSafe(L.Id, Share, &R)) {
            // Not standing in a world yet: try again later, for a while.
            if (Now - L.At < kKeepMs && KeepCount < kQueueSize) Keep[KeepCount++] = L;
            continue;
        }
        LOG_INFO("[BONFIRE] %s lit bonfire %d: %s (view %d, %s, %s%s)", L.From, L.Id,
                 !R.Known ? "not in my list" : R.WasLit ? "lit here already" : R.Kindled ? "kindled here" : "lit here",
                 R.View, R.Loaded ? "loaded" : "not loaded", R.OwnSaved ? "my save too" : "not my save",
                 (!R.Known || (R.View == 0 && !Share)) ? "; sharing off or unknown -- only told" : "");
        if (!R.Known) continue;
        NotePartnerLitForRespawn(L.Id, L.Map);   // death_sync.cpp: my respawn follows it where we both are
        const std::string Label = BonfireLabel(L.Id);
        UI::Overlay::GetInstance().ShowNotification(
            UI::Format(UI::Tr("%s lit the bonfire %s", "%s зажёг костёр %s"), L.From, Label.c_str()),
            5.0f, UI::NotifyKind::Player);
    }
    if (KeepCount) {
        std::lock_guard<std::mutex> Lock(g_mutex);
        for (int I = 0; I < KeepCount && g_partnerCount < kQueueSize; ++I) g_partner[g_partnerCount++] = Keep[I];
    }
}

} // namespace DS2Coop::Sync
