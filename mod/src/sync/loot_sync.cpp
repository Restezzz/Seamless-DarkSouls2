// World items for every player from their own save -- also in someone else's world.
//
// A world item is a map object with a MapObjItemDropComponent. When its area
// loads, the component builds an item lot and asks the drop manager
// ([[GMImp+0x38]+0x1C8]) for a drop; only a live drop is drawn and can be picked
// up. Two multiplayer gates keep a player who loads an area inside a session
// from ever getting one:
//
//   exe+0x1E26F0  lot from the map object: returns an EMPTY lot while
//                 multiplayer is active (exe+0x5135F0), so no drop is made
//   exe+0x1F31A0  area load step 12: put back the saved object states of the
//                 area -- which items were already taken -- skipped entirely in
//                 multiplayer
//
// The host loaded its areas before the session began and keeps its items; a
// guest loads the host's world inside the session and sees none.
//
// What was taken lives in each object's drop handle (component +0x40): lot id,
// the lot slots ever rolled (+0xC) and the ones still lying there (+0xE). The
// handles are saved per area in two stores, both part of the save file:
//
//   [mapMgr+0x200]       full records of the three latest areas (0x6008 bytes)
//   [mapMgr+0x1F8]+0x24  42 compact records (0xC08 bytes each)
//
// and put back by exe+0x1F3A10(stateMgr, obj, &lotId, &packed). In multiplayer
// neither store is read nor written, so the player's own world stays as it was.
//
// With loot sync on, for world items only:
//   1. item-drop init gets the lot the game would build alone (chests and enemy
//      drops keep the game's rules);
//   2. after the area's own restore -- skipped in multiplayer -- each item's
//      handle is put back from THIS player's records, and nothing else in the
//      area is touched, so doors and levers stay as the host has them;
//   3. an item picked up in multiplayer is remembered in ds2_coop_loot.txt and
//      applied whenever that area loads again; in the player's own world the
//      game then saves it by itself, exactly as if it had been picked up there.
//
// The other player's copy is untouched: world-item drops are local to each
// client (drop type 0), only enemy drops travel between players.

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
#include <cstdio>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

// --- game functions and data (RVA) -------------------------------------------
constexpr uint32_t kGameManagerImp  = 0x16148F0;  // *(exe+...) = GameManagerImp
constexpr uint32_t kLotFromMapObj   = 0x1E26F0;   // (int out[2], row, index, flag): empty in multiplayer
constexpr uint32_t kLotCompute      = 0x1E2B40;   // (int out[2], kind, base lot, index)
constexpr uint32_t kItemDropInit    = 0x1D2E70;   // MapObjItemDropComponent: roll the lot, make the drop
constexpr uint32_t kItemDropInitEnd = 0x1D2E70 + 377;
constexpr uint32_t kAreaRestore     = 0x1F31A0;   // (stateMgr, areaId): saved object states, area load step 12
constexpr uint32_t kPickup          = 0x1E6C60;   // (uint64* drop handle): take a drop
constexpr uint32_t kMpActive        = 0x5135F0;   // multiplayer active (through the mod's own hook)
constexpr uint32_t kAreaById        = 0x3BCE40;   // (mapMgr, areaId) -> area
constexpr uint32_t kAreaByIndex     = 0x3BCE30;   // (mapMgr, index) -> area
constexpr uint32_t kRecordFind      = 0x1E7450;   // (store, areaId) -> full record
constexpr uint32_t kSlotFind        = 0x1F4F90;   // (slots, 42, areaId) -> compact record
constexpr uint32_t kSlotIndex       = 0x1F5030;   // (compact record, object id) -> entry or -1
constexpr uint32_t kGetItemDrop     = 0x1E5C10;   // object -> MapObjItemDropComponent
constexpr uint32_t kPrefabGet       = 0x1729A0;   // object of kind 4 -> prefab
constexpr uint32_t kPrefabSub       = 0x449D00;   // (prefab, 0) -> the object inside
constexpr uint32_t kHandleRestore   = 0x1F3A10;   // (stateMgr, obj, &lotId, &packed)
constexpr uint32_t kHandlePack      = 0x1DE750;   // (handle, &packed)
constexpr uint32_t kHandleLot       = 0x1DE7F0;   // (handle, &lotId)
constexpr uint32_t kDropMgrGet      = 0x1E6550;   // -> drop manager
constexpr uint32_t kDropMgrHas      = 0x1E41F0;   // (mgr, handle*) -> drop is live
constexpr uint32_t kObjRefMake      = 0x17B2C0;   // (out, obj) -> object reference
constexpr uint32_t kUnregister      = 0x1E6410;   // (object reference*) -> drop removed
constexpr uint32_t kItemDropSet     = 0x1D33E0;   // (component, enable): register or remove the drop
constexpr uint32_t kShouldExist     = 0x1D32E0;   // (component) -> the model says the item is there

// --- layout --------------------------------------------------------------------
constexpr uint32_t kHandleOff     = 0x40;   // drop handle inside the component
constexpr uint32_t kRolledMaskOff = 0x4C;   // handle +0xC: lot slots ever rolled
constexpr uint32_t kCompactSlots  = 42;
constexpr uint32_t kCompactMax    = 0xC0;   // entries in one compact record
constexpr int32_t  kFullMax       = 1024;   // entries in one full record
constexpr char     kPendingFile[] = "ds2_coop_loot.txt";

using LotFromMapObjFn = void(__fastcall*)(int32_t* out, uintptr_t row, int32_t index, char flag);
using LotComputeFn    = void(__fastcall*)(int32_t* out, int32_t kind, int32_t baseLot, int32_t index);
using AreaRestoreFn   = void(__fastcall*)(uintptr_t stateMgr, uint64_t areaId);
using PickupFn        = uint64_t(__fastcall*)(uint64_t* dropHandle);
using MpActiveFn      = uint64_t(__fastcall*)(uintptr_t session);
using AreaByIdFn      = uintptr_t(__fastcall*)(uintptr_t mapMgr, uint32_t areaId);
using AreaByIndexFn   = uintptr_t(__fastcall*)(uintptr_t mapMgr, int32_t index);
using RecordFindFn    = const int32_t*(__fastcall*)(uintptr_t store, int32_t areaId);
using SlotFindFn      = int32_t*(__fastcall*)(int32_t* slots, uint32_t count, int32_t areaId);
using SlotIndexFn     = uint32_t(__fastcall*)(int32_t* slot, int32_t objectId);
using ObjFn           = uintptr_t(__fastcall*)(uintptr_t obj);
using PrefabSubFn     = uintptr_t(__fastcall*)(uintptr_t prefab, int32_t index);
using HandleRestoreFn = void(__fastcall*)(uintptr_t stateMgr, uintptr_t obj, uint32_t* lotId, uint32_t* packed);
using HandleWordFn    = void(__fastcall*)(uintptr_t handle, uint32_t* out);
using DropMgrGetFn    = uintptr_t(__fastcall*)();
using DropMgrHasFn    = bool(__fastcall*)(uintptr_t mgr, uint64_t* handle);
using ObjRefMakeFn    = uint32_t*(__fastcall*)(uint32_t* out, uintptr_t obj);
using ObjRefFn        = void(__fastcall*)(uint32_t* ref);
using ItemDropSetFn   = void(__fastcall*)(uintptr_t comp, char enable);
using CompByteFn      = uint8_t(__fastcall*)(uintptr_t comp);
using CompFn          = void(__fastcall*)(uintptr_t comp);

enum Request : int { kNone = 0, kShow = 1, kHide = 2 };

uintptr_t g_base = 0;
LotFromMapObjFn g_lotFromMapObj = nullptr;
AreaRestoreFn   g_areaRestore   = nullptr;
PickupFn        g_pickup        = nullptr;

std::atomic<bool>      g_installed{ false };
std::atomic<bool>      g_ok{ false };        // all three hooks are in
std::atomic<bool>      g_enabled{ true };
std::atomic<bool>      g_broken{ false };    // touching the world threw once: stay out this run
std::atomic<int>       g_request{ kNone };   // F4 work for the game thread
std::atomic<uint32_t>  g_lotsBuilt{ 0 };
std::atomic<ULONGLONG> g_lastAreaToast{ 0 };

// An item picked up in multiplayer, until that area loads in the owner's world.
struct Pending {
    std::string Owner;
    uint32_t Area = 0;
    uint32_t Object = 0;
    uint32_t Lot = 0;
    uint32_t Packed = 0;
};

// The same, flat, for the code that runs under SEH.
struct PendingPod {
    uint32_t Area;
    uint32_t Object;
    uint32_t Lot;
    uint32_t Packed;
    uint32_t Applied;
};

struct AreaStats {
    uint32_t Items;        // world items in the area
    uint32_t FromFull;     // put back from the full record
    uint32_t FromCompact;  // put back from the compact record
    uint32_t Remembered;   // put back from ds2_coop_loot.txt
    uint32_t WasLive;      // already had a drop when put back
    uint32_t Applied;
    uint32_t Shown;
    uint32_t Hidden;
};

struct PickupProbe {
    uintptr_t Object;
    uintptr_t Comp;
    uint32_t  Area;
    uint32_t  Id;
    uint32_t  LotBefore;
    uint32_t  PackedBefore;
    uint32_t  LotAfter;
    uint32_t  PackedAfter;
    uint8_t   Found;
    uint8_t   StillLive;
};

std::mutex           g_pendingMutex;
std::vector<Pending> g_pending;

template <typename T> T Game(uint32_t Rva) { return reinterpret_cast<T>(g_base + Rva); }

// --- raw world access (no C++ objects: these run under SEH) --------------------

uintptr_t GameManager() { return *reinterpret_cast<uintptr_t*>(g_base + kGameManagerImp); }

uintptr_t MapManager() {
    const uintptr_t Gm = GameManager();
    return Gm ? *reinterpret_cast<uintptr_t*>(Gm + 0x38) : 0;
}

// The same test the game makes before skipping the restore: a session exists
// and the multiplayer predicate says yes.
bool MultiplayerActive() {
    const uintptr_t Gm = GameManager();
    if (!Gm) return false;
    const uintptr_t Session = *reinterpret_cast<uintptr_t*>(Gm + 0x22F0);
    if (!Session) return false;
    return (Game<MpActiveFn>(kMpActive)(Session) & 0xFF) != 0;
}

uintptr_t ResolveObject(uintptr_t Obj) {
    if (!Obj) return 0;
    if (*reinterpret_cast<uint8_t*>(Obj + 0xA2) != 4) return Obj;
    const uintptr_t Prefab = Game<ObjFn>(kPrefabGet)(Obj);
    return Prefab ? Game<PrefabSubFn>(kPrefabSub)(Prefab, 0) : 0;
}

// The id the game files the object's state under (0 = not saved).
uint32_t ObjectId(uintptr_t Obj) {
    const uintptr_t Info = *reinterpret_cast<uintptr_t*>(Obj + 0x30);
    if (!Info || *reinterpret_cast<uint8_t*>(Info + 8) != 1) return 0;
    const uint32_t* Id = *reinterpret_cast<uint32_t* const*>(Info + 0x70);
    return Id ? (*Id & 0x7FFFFFF) : 0;
}

// Full record: entries of 16 bytes from +8, the object id in bits 2..28 of their
// last word; lot ids from int 0x1002, packed handles from int 0x1402.
int32_t FullRecordIndex(const int32_t* Full, uint32_t Id) {
    int32_t Count = Full[1];
    if (Count > kFullMax) Count = kFullMax;
    for (int32_t K = 0; K < Count; ++K) {
        const uint32_t Word = static_cast<uint32_t>(Full[2 + 4 * K + 3]);
        if (((Word >> 2) & 0x7FFFFFF) == Id) return K;
    }
    return -1;
}

bool DropIsLive(uintptr_t Comp) {
    const uintptr_t Mgr = Game<DropMgrGetFn>(kDropMgrGet)();
    return Mgr && Game<DropMgrHasFn>(kDropMgrHas)(Mgr, reinterpret_cast<uint64_t*>(Comp + kHandleOff));
}

void RemoveDrop(uintptr_t Obj) {
    uint64_t Ref = 0;
    Game<ObjRefMakeFn>(kObjRefMake)(reinterpret_cast<uint32_t*>(&Ref), Obj);
    Game<ObjRefFn>(kUnregister)(reinterpret_cast<uint32_t*>(&Ref));
}

// Register or remove the drop exactly as the model says -- the component's own
// enable path, so the loaded-area and DLC-map checks still apply.
void Reevaluate(uintptr_t Comp) {
    Game<ItemDropSetFn>(kItemDropSet)(Comp, static_cast<char>(Game<CompByteFn>(kShouldExist)(Comp)));
}

void ApplyItemState(uintptr_t StateMgr, uintptr_t Obj, uintptr_t Comp,
                    uint32_t Lot, uint32_t Packed, AreaStats* S) {
    // A drop made before the state arrived is rebuilt from the state, so an
    // item this player already took does not stay on the ground.
    const bool Live = DropIsLive(Comp);
    if (Live) {
        RemoveDrop(Obj);
        S->WasLive++;
    }
    Game<HandleRestoreFn>(kHandleRestore)(StateMgr, Obj, &Lot, &Packed);
    if (Live) Reevaluate(Comp);
    S->Applied++;
}

void RestoreAreaImpl(uintptr_t StateMgr, uint32_t AreaId, bool Own,
                     PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return;
    const uintptr_t Area = Game<AreaByIdFn>(kAreaById)(MapMgr, AreaId);
    if (!Area) return;
    const uintptr_t List = *reinterpret_cast<uintptr_t*>(Area + 0x160);
    if (!List) return;
    const uintptr_t* Objects = *reinterpret_cast<uintptr_t* const*>(List + 0x10);
    const uint32_t Count = *reinterpret_cast<uint32_t*>(List + 0x18);
    if (!Objects) return;

    const int32_t* Full = nullptr;
    int32_t* Compact = nullptr;
    if (Own) {
        const uintptr_t Store = *reinterpret_cast<uintptr_t*>(MapMgr + 0x200);
        if (Store) {
            Full = Game<RecordFindFn>(kRecordFind)(Store, static_cast<int32_t>(AreaId));
            if (Full && static_cast<uint32_t>(Full[0]) != AreaId) Full = nullptr;
        }
        if (StateMgr) {
            Compact = Game<SlotFindFn>(kSlotFind)(reinterpret_cast<int32_t*>(StateMgr + 0x24),
                                                  kCompactSlots, static_cast<int32_t>(AreaId));
        }
    }

    for (uint32_t I = 0; I < Count; ++I) {
        const uintptr_t Obj = ResolveObject(Objects[I]);
        if (!Obj) continue;
        const uintptr_t Comp = Game<ObjFn>(kGetItemDrop)(Obj);
        if (!Comp) continue;
        S->Items++;
        const uint32_t Id = ObjectId(Obj);
        if (!Id) continue;

        uint32_t Lot = 0, Packed = 0;
        bool Have = false;
        PendingPod* Remembered = nullptr;
        for (int32_t P = 0; P < PendCount; ++P) {
            if (Pend[P].Area != AreaId || Pend[P].Object != Id) continue;
            // A line that never rolled anything says nothing: use the records.
            if (Pend[P].Lot == 0 && (Pend[P].Packed & 0x3FF) == 0) break;
            Lot = Pend[P].Lot;
            Packed = Pend[P].Packed;
            Remembered = &Pend[P];
            Have = true;
            break;
        }
        if (!Have && Full) {
            const int32_t K = FullRecordIndex(Full, Id);
            if (K >= 0) {
                Lot = static_cast<uint32_t>(Full[0x1002 + K]);
                Packed = static_cast<uint32_t>(Full[0x1402 + K]);
                Have = true;
                S->FromFull++;
            }
        }
        if (!Have && Compact) {
            const uint32_t K = Game<SlotIndexFn>(kSlotIndex)(Compact, static_cast<int32_t>(Id));
            if (K < kCompactMax) {
                Lot = static_cast<uint32_t>(Compact[0x182 + K]);
                Packed = static_cast<uint32_t>(Compact[0x242 + K]);
                Have = true;
                S->FromCompact++;
            }
        }
        // A record of an item that never rolled says nothing: leave it be.
        if (Have && (Lot != 0 || (Packed & 0x3FF) != 0)) {
            ApplyItemState(StateMgr, Obj, Comp, Lot, Packed, S);
            if (Remembered) {
                Remembered->Applied = 1;   // only now may the line be forgotten
                S->Remembered++;
            }
        }
    }
}

// F4: every world item in the loaded areas, shown from this player's records or
// hidden again.
void ShowHideImpl(bool Show, PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return;
    const uintptr_t Areas = *reinterpret_cast<uintptr_t*>(MapMgr + 8);
    if (!Areas) return;
    const int32_t AreaCount = *reinterpret_cast<int16_t*>(Areas + 0x1B6);
    const uintptr_t StateMgr = *reinterpret_cast<uintptr_t*>(MapMgr + 0x1F8);

    for (int32_t A = 0; A < AreaCount && A < 128; ++A) {
        const uintptr_t Area = Game<AreaByIndexFn>(kAreaByIndex)(MapMgr, A);
        if (!Area || *reinterpret_cast<int8_t*>(Area + 0x1E0) <= 11) continue;   // not loaded yet
        const uint32_t AreaId = *reinterpret_cast<uint32_t*>(Area + 8);
        if (Show) RestoreAreaImpl(StateMgr, AreaId, true, Pend, PendCount, S);

        const uintptr_t List = *reinterpret_cast<uintptr_t*>(Area + 0x160);
        if (!List) continue;
        const uintptr_t* Objects = *reinterpret_cast<uintptr_t* const*>(List + 0x10);
        const uint32_t Count = *reinterpret_cast<uint32_t*>(List + 0x18);
        if (!Objects) continue;
        for (uint32_t I = 0; I < Count; ++I) {
            const uintptr_t Obj = ResolveObject(Objects[I]);
            if (!Obj) continue;
            const uintptr_t Comp = Game<ObjFn>(kGetItemDrop)(Obj);
            if (!Comp) continue;
            if (Show) {
                // Never rolled: roll it now, as the model's load would have.
                if (*reinterpret_cast<uint16_t*>(Comp + kRolledMaskOff) == 0) {
                    Game<CompFn>(kItemDropInit)(Comp);
                }
                Reevaluate(Comp);
                if (DropIsLive(Comp)) S->Shown++;
            } else if (DropIsLive(Comp)) {
                // Straight to the drop manager: on DLC maps the component's
                // enable path would ask the model instead of taking the "off".
                RemoveDrop(Obj);
                *reinterpret_cast<uint8_t*>(Comp + 0x58) &= static_cast<uint8_t>(~8u);   // "registered"
                S->Hidden++;
            }
        }
    }
}

void FindDropImpl(uint64_t Handle, PickupProbe* P) {
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return;
    const uintptr_t Areas = *reinterpret_cast<uintptr_t*>(MapMgr + 8);
    if (!Areas) return;
    const int32_t AreaCount = *reinterpret_cast<int16_t*>(Areas + 0x1B6);

    for (int32_t A = 0; A < AreaCount && A < 128; ++A) {
        const uintptr_t Area = Game<AreaByIndexFn>(kAreaByIndex)(MapMgr, A);
        if (!Area || *reinterpret_cast<int8_t*>(Area + 0x1E0) <= 11) continue;
        const uintptr_t List = *reinterpret_cast<uintptr_t*>(Area + 0x160);
        if (!List) continue;
        const uintptr_t* Objects = *reinterpret_cast<uintptr_t* const*>(List + 0x10);
        const uint32_t Count = *reinterpret_cast<uint32_t*>(List + 0x18);
        if (!Objects) continue;
        for (uint32_t I = 0; I < Count; ++I) {
            const uintptr_t Obj = ResolveObject(Objects[I]);
            if (!Obj) continue;
            const uintptr_t Comp = Game<ObjFn>(kGetItemDrop)(Obj);
            if (!Comp || *reinterpret_cast<uint64_t*>(Comp + kHandleOff) != Handle) continue;
            P->Object = Obj;
            P->Comp = Comp;
            P->Area = *reinterpret_cast<uint32_t*>(Area + 8);
            P->Id = ObjectId(Obj);
            Game<HandleWordFn>(kHandleLot)(Comp + kHandleOff, &P->LotBefore);
            P->PackedBefore = 0;
            Game<HandleWordFn>(kHandlePack)(Comp + kHandleOff, &P->PackedBefore);
            P->Found = 1;
            return;
        }
    }
}

void SnapshotAfterImpl(PickupProbe* P) {
    Game<HandleWordFn>(kHandleLot)(P->Comp + kHandleOff, &P->LotAfter);
    P->PackedAfter = 0;
    Game<HandleWordFn>(kHandlePack)(P->Comp + kHandleOff, &P->PackedAfter);
    P->StillLive = DropIsLive(P->Comp) ? 1 : 0;
}

bool RestoreAreaSafe(uintptr_t StateMgr, uint32_t AreaId, bool Own,
                     PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    __try {
        RestoreAreaImpl(StateMgr, AreaId, Own, Pend, PendCount, S);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ShowHideSafe(bool Show, PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    __try {
        ShowHideImpl(Show, Pend, PendCount, S);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool FindDropSafe(uint64_t Handle, PickupProbe* P) {
    __try {
        FindDropImpl(Handle, P);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SnapshotAfterSafe(PickupProbe* P) {
    __try {
        SnapshotAfterImpl(P);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool MultiplayerActiveSafe() {
    __try {
        return MultiplayerActive();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// --- remembered pickups ----------------------------------------------------------

std::vector<std::string> SplitTabs(const std::string& Line) {
    std::vector<std::string> Parts(1);
    for (const char C : Line) {
        if (C == '\t') Parts.emplace_back();
        else if (C != '\r' && C != '\n') Parts.back() += C;
    }
    return Parts;
}

void SavePendingLocked() {
    std::ofstream Out(kPendingFile, std::ios::trunc);
    if (!Out.is_open()) {
        LOG_WARNING("[LOOT] cannot write %s -- pickups made in multiplayer will not be kept", kPendingFile);
        return;
    }
    Out << "# World items picked up in someone else's world (DS2 Seamless Co-op).\n"
           "# Each line is applied the next time that area loads in the owner's own world,\n"
           "# where the game then saves it like any other pickup, and the line goes away.\n"
           "# owner\tarea\tobject\tlot\tstate\n";
    char Numbers[64];
    for (const Pending& E : g_pending) {
        std::snprintf(Numbers, sizeof(Numbers), "\t%08X\t%08X\t%08X\t%08X\n",
                      E.Area, E.Object, E.Lot, E.Packed);
        Out << E.Owner << Numbers;
    }
}

void LoadPending() {
    std::ifstream In(kPendingFile);
    if (!In.is_open()) return;
    std::lock_guard<std::mutex> Lock(g_pendingMutex);
    std::string Line;
    while (std::getline(In, Line)) {
        if (Line.empty() || Line[0] == '#') continue;
        const std::vector<std::string> Parts = SplitTabs(Line);
        if (Parts.size() != 5 || Parts[0].empty()) continue;
        try {
            Pending E;
            E.Owner = Parts[0];
            E.Area = static_cast<uint32_t>(std::stoul(Parts[1], nullptr, 16));
            E.Object = static_cast<uint32_t>(std::stoul(Parts[2], nullptr, 16));
            E.Lot = static_cast<uint32_t>(std::stoul(Parts[3], nullptr, 16));
            E.Packed = static_cast<uint32_t>(std::stoul(Parts[4], nullptr, 16));
            g_pending.push_back(E);
        } catch (...) {
            LOG_WARNING("[LOOT] skipping an unreadable line in %s", kPendingFile);
        }
    }
}

std::vector<PendingPod> OwnerPods(const std::string& Owner, bool AllAreas, uint32_t AreaId) {
    std::vector<PendingPod> Pods;
    if (Owner.empty()) return Pods;
    std::lock_guard<std::mutex> Lock(g_pendingMutex);
    for (const Pending& E : g_pending) {
        if (E.Owner != Owner || (!AllAreas && E.Area != AreaId)) continue;
        Pods.push_back(PendingPod{ E.Area, E.Object, E.Lot, E.Packed, 0 });
    }
    return Pods;
}

// Applied in the owner's own world: from here on the game keeps it itself.
uint32_t ForgetApplied(const std::string& Owner, const std::vector<PendingPod>& Pods) {
    std::lock_guard<std::mutex> Lock(g_pendingMutex);
    uint32_t Removed = 0;
    for (const PendingPod& Pod : Pods) {
        if (!Pod.Applied) continue;
        for (size_t I = 0; I < g_pending.size(); ++I) {
            const Pending& E = g_pending[I];
            if (E.Owner == Owner && E.Area == Pod.Area && E.Object == Pod.Object) {
                g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(I));
                ++Removed;
                break;
            }
        }
    }
    if (Removed) SavePendingLocked();
    return Removed;
}

// What the handle has to say once the item is gone: every slot it ever rolled
// stays rolled -- so the lot is never rolled again -- and nothing lies there.
uint32_t TakenState(const PickupProbe& P) {
    const uint32_t Base = P.LotAfter ? P.PackedAfter : P.PackedBefore;
    uint32_t Rolled = (P.PackedBefore & 0x3FF) | ((P.PackedBefore >> 10) & 0x3FF)
                    | (P.PackedAfter & 0x3FF) | ((P.PackedAfter >> 10) & 0x3FF);
    if (!Rolled) Rolled = 1;
    return (Base & ~0xFFFFFu) | Rolled;
}

void RememberPickup(const PickupProbe& P) {
    if (P.StillLive) {
        LOG_INFO("[LOOT] object %u was not taken after all -- nothing to remember", P.Id);
        return;
    }
    if (!P.Id) {
        LOG_WARNING("[LOOT] picked up an item whose object has no saved id -- it cannot be remembered");
        return;
    }
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    if (Owner.empty()) {
        LOG_WARNING("[LOOT] picked up object %u but the character name is unreadable -- not remembered", P.Id);
        return;
    }
    const uint32_t Lot = P.LotAfter ? P.LotAfter : P.LotBefore;
    const uint32_t Packed = TakenState(P);
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        bool Replaced = false;
        for (Pending& E : g_pending) {
            if (E.Owner == Owner && E.Area == P.Area && E.Object == P.Id) {
                E.Lot = Lot;
                E.Packed = Packed;
                Replaced = true;
                break;
            }
        }
        if (!Replaced) {
            Pending E;
            E.Owner = Owner;
            E.Area = P.Area;
            E.Object = P.Id;
            E.Lot = Lot;
            E.Packed = Packed;
            g_pending.push_back(E);
        }
        SavePendingLocked();
    }
    LOG_INFO("[LOOT] picked up in multiplayer: area %u object %u lot %u (handle %08X -> %08X, kept as %08X)"
             " -- %s's own world will not have it either",
             P.Area, P.Id, Lot, P.PackedBefore, P.PackedAfter, Packed, Owner.c_str());
    UI::Overlay::GetInstance().ShowNotification(
        UI::Tr("Picked up \xE2\x80\x94 it is gone from your own world too",
               "Подобрано \xE2\x80\x94 в твоём мире этого предмета тоже не будет"),
        2.5f, UI::NotifyKind::Success);
}

// --- detours ---------------------------------------------------------------------

// The call the lot builder makes when alone, under SEH like every other walk.
bool ComputeLotSafe(int32_t* Out, uintptr_t Row, int32_t Index, char Flag) {
    __try {
        Game<LotComputeFn>(kLotCompute)(Out, (Flag != 0) + 1, *reinterpret_cast<int32_t*>(Row + 0x24),
                                        Index ? Index : 1);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The lot builder answers "no lot" in multiplayer. For item-drop init -- and only
// there -- build the lot the way the game does alone.
void __fastcall LotFromMapObjDetour(int32_t* Out, uintptr_t Row, int32_t Index, char Flag) {
    g_lotFromMapObj(Out, Row, Index, Flag);
    if (!Out || Out[0] != 0 || !Row) return;
    if (!g_ok.load() || !g_enabled.load() || g_broken.load()) return;
    // Item-drop init, and treasure-chest init (exe+0x1CF930, 439 bytes), whose
    // chest the guest can now open (the lid settles locally, see MpActiveHook) --
    // an empty lot would have given it nothing inside.
    constexpr uint32_t kChestInit    = 0x1CF930;
    constexpr uint32_t kChestInitEnd = 0x1CF930 + 439;
    const uintptr_t Caller = reinterpret_cast<uintptr_t>(_ReturnAddress()) - g_base;
    const bool FromItemDrop = Caller >= kItemDropInit && Caller < kItemDropInitEnd;
    const bool FromChest    = Caller >= kChestInit && Caller < kChestInitEnd;
    if (!FromItemDrop && !FromChest) return;
    if (!MultiplayerActiveSafe()) return;   // empty for a reason of its own
    if (!ComputeLotSafe(Out, Row, Index, Flag)) {
        Out[0] = 0;   // no lot, as the game would have had it
        Out[1] = 0;
        g_broken.store(true);
        LOG_ERROR("[LOOT] building an item lot threw -- world item sync is off for this run");
        return;
    }
    const uint32_t Built = g_lotsBuilt.fetch_add(1) + 1;
    if (Built <= 3 || Built % 100 == 0) {
        LOG_INFO("[LOOT] item lot built in multiplayer (#%u): lot %d", Built, Out[0]);
    }
}

bool HavePending() {
    std::lock_guard<std::mutex> Lock(g_pendingMutex);
    return !g_pending.empty();
}

void __fastcall AreaRestoreDetour(uintptr_t StateMgr, uint64_t AreaArg) {
    g_areaRestore(StateMgr, AreaArg);
    if (!g_ok.load() || !g_enabled.load() || g_broken.load()) return;

    const uint32_t AreaId = static_cast<uint32_t>(AreaArg);
    const bool Mp = MultiplayerActiveSafe();
    if (!Mp && !HavePending()) return;   // alone, nothing remembered: the common case costs nothing
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    std::vector<PendingPod> Pods = OwnerPods(Owner, false, AreaId);
    if (!Mp && Pods.empty()) return;   // own world, nothing remembered: the game has done it all

    AreaStats S{};
    if (!RestoreAreaSafe(StateMgr, AreaId, Mp, Pods.data(), static_cast<int32_t>(Pods.size()), &S)) {
        g_broken.store(true);
        LOG_ERROR("[LOOT] putting back world items in area %u threw -- world item sync is off for this run", AreaId);
        return;
    }
    const uint32_t Forgotten = Mp ? 0 : ForgetApplied(Owner, Pods);

    if (Mp) {
        LOG_INFO("[LOOT] area %u loaded in multiplayer: %u world items, %u put back from your save"
                 " (%u full + %u compact), %u remembered pickups, %u rebuilt",
                 AreaId, S.Items, S.FromFull + S.FromCompact, S.FromFull, S.FromCompact,
                 S.Remembered, S.WasLive);
        const ULONGLONG Now = GetTickCount64();
        if (S.Items && Now - g_lastAreaToast.load() > 120000) {
            g_lastAreaToast.store(Now);
            UI::Overlay::GetInstance().ShowNotification(
                UI::Tr("World items here follow your own save",
                       "Предметы мира здесь \xE2\x80\x94 по твоему сейву"),
                3.5f, UI::NotifyKind::Info);
        }
    } else {
        LOG_INFO("[LOOT] area %u in your own world: %u pickups made in multiplayer applied (%u now kept by the game)",
                 AreaId, S.Remembered, Forgotten);
    }
}

// What a drop holds, and what the inventory made of it -- logged for every
// pickup, so "picked up but nothing in the bag" names its own reason. The
// inventory ([[[GMImp+0xA8]+0x10]+0x10]) keeps the verdict of its last add at
// +0x10138: bit 31 = refused, with the reason in bits 30..16 (exe+0x1A94F0).
struct DropContents {
    uint32_t Count;
    uint32_t Item[8];
    uint16_t Amount[8];
};

using DropCopyFn = bool(__fastcall*)(uintptr_t mgr, uint64_t* handle, void* desc);
constexpr uint32_t kDropCopy = 0x1E3D80;   // (mgr, &handle, out 0x84 bytes) -> the drop exists

bool ReadDropSafe(uint64_t Handle, DropContents* C) {
    __try {
        const uintptr_t Mgr = Game<DropMgrGetFn>(kDropMgrGet)();
        if (!Mgr) return false;
        alignas(16) uint8_t Desc[0x100] = {};
        uint64_t H = Handle;
        if (!Game<DropCopyFn>(kDropCopy)(Mgr, &H, Desc)) return false;
        C->Count = Desc[0x80];
        for (uint32_t I = 0; I < C->Count && I < 8; ++I) {
            C->Item[I] = *reinterpret_cast<uint32_t*>(Desc + I * 0x10 + 4);
            C->Amount[I] = *reinterpret_cast<uint16_t*>(Desc + I * 0x10 + 0xC);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadInventoryVerdictSafe(uint32_t* Out) {
    __try {
        const uintptr_t Gm = GameManager();
        if (!Gm) return false;
        const uintptr_t Data = *reinterpret_cast<uintptr_t*>(Gm + 0xA8);
        if (!Data) return false;
        const uintptr_t Bag = *reinterpret_cast<uintptr_t*>(Data + 0x10);
        if (!Bag) return false;
        const uintptr_t Inventory = *reinterpret_cast<uintptr_t*>(Bag + 0x10);
        if (!Inventory) return false;
        *Out = *reinterpret_cast<uint32_t*>(Inventory + 0x10138);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void LogPickup(uint64_t Handle, const PickupProbe& P, bool HaveContents, const DropContents& C,
               bool HaveVerdict, uint32_t Verdict, bool StillThere) {
    std::string Items;
    char Part[64];
    if (!HaveContents) {
        Items = "contents unreadable";
    } else {
        std::snprintf(Part, sizeof(Part), "%u item(s)", C.Count);
        Items = Part;
        for (uint32_t I = 0; I < C.Count && I < 8; ++I) {
            std::snprintf(Part, sizeof(Part), "%s %u x%u", I ? "," : ":", C.Item[I], C.Amount[I]);
            Items += Part;
        }
    }
    char Where[64] = "not a world item";
    if (P.Found) std::snprintf(Where, sizeof(Where), "area %u object %u", P.Area, P.Id);
    LOG_INFO("[LOOT] pickup of drop %016llX (%s): %s -> inventory %s (%08X), drop %s",
             static_cast<unsigned long long>(Handle), Where, Items.c_str(),
             !HaveVerdict ? "unreadable" : (Verdict & 0x80000000u) ? "REFUSED" : "took it",
             Verdict, StillThere ? "still there" : "gone");
}

uint64_t __fastcall PickupDetour(uint64_t* DropHandle) {
    if (!DropHandle) return g_pickup(DropHandle);
    const uint64_t Handle = *DropHandle;

    // Read-only diagnostics for every pickup, in any world.
    DropContents Contents{};
    const bool HaveContents = ReadDropSafe(Handle, &Contents);
    PickupProbe P{};
    const bool Searched = FindDropSafe(Handle, &P);

    const uint64_t Result = g_pickup(DropHandle);

    uint32_t Verdict = 0;
    const bool HaveVerdict = ReadInventoryVerdictSafe(&Verdict);
    DropContents Left{};
    const bool StillThere = ReadDropSafe(Handle, &Left);
    LogPickup(Handle, P, HaveContents, Contents, HaveVerdict, Verdict, StillThere);

    // Remembering is for multiplayer only, and only with loot sync on.
    if (!g_ok.load() || !g_enabled.load() || g_broken.load() || !MultiplayerActiveSafe()) return Result;
    if (Searched && P.Found) {
        // The inventory said no (8000C000 for ee at 22:26:16): the item is not in
        // the bag, so it must not vanish from this player's own world either.
        if (HaveVerdict && (Verdict & 0x80000000u)) {
            LOG_INFO("[LOOT] the inventory refused it -- not remembered, your own world keeps it");
        } else if (SnapshotAfterSafe(&P)) {
            RememberPickup(P);
        }
    } else if (Searched) {
        LOG_INFO("[LOOT] picked up a drop that is not a world item (enemy drop) -- nothing to remember");
    }
    return Result;
}

bool Hook(uint32_t Rva, void* Detour, void** Original, const char* What) {
    void* Target = reinterpret_cast<void*>(g_base + Rva);
    if (Hooks::HookManager::GetInstance().InstallHook(Target, Detour, Original)) return true;
    LOG_WARNING("[LOOT] could not hook %s at exe+0x%X", What, Rva);
    return false;
}

// --- only the local player's own pickups ------------------------------------------
// A pickup is an action, and the action executor (exe+0x451E50 -> exe+0x4528F0
// for actions 0x1B/0x1E/0x1F, called with the acting character) also runs for
// the other player's replayed actions. The character-kind gate at exe+0x452916
// kept a phantom's replayed pickup out of the host's bag; the mod opens that
// gate so a guest can pick anything up at all, and on 12.09 ee's lifegem
// (22:20:33) went into Restez's bag as well. Now only an action of this
// player's own character ([GMImp+0xD0]) takes anything.
constexpr uint32_t kPickupExec = 0x4528F0;   // (target, actor) -> taken
using PickupExecFn = uint64_t(__fastcall*)(void*, void*);
PickupExecFn g_pickupExec = nullptr;
std::atomic<uint32_t> g_foreignPickups{ 0 };

uintptr_t LocalCharacterSafe() {
    __try {
        const uintptr_t Gm = GameManager();
        return Gm ? *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t __fastcall PickupExecDetour(void* Target, void* Actor) {
    const uintptr_t Local = LocalCharacterSafe();
    if (Local && Actor && reinterpret_cast<uintptr_t>(Actor) != Local) {
        const uint32_t N = g_foreignPickups.fetch_add(1) + 1;
        if (N <= 5 || N % 50 == 0) {
            LOG_INFO("[LOOT] the other player's pickup was replayed here -- not taken (#%u)", N);
        }
        return 0;
    }
    return g_pickupExec(Target, Actor);
}

} // namespace

bool InstallLootSync() {
    if (g_installed.exchange(true)) return g_ok.load();
    g_base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    LoadPending();

    // The restore and the pickup go in before the lot: a lot without its saved
    // state would hand out again what this player has already taken.
    const bool Restore = Hook(kAreaRestore, reinterpret_cast<void*>(&AreaRestoreDetour),
                              reinterpret_cast<void**>(&g_areaRestore), "the area object-state restore");
    const bool Pickup = Restore && Hook(kPickup, reinterpret_cast<void*>(&PickupDetour),
                                        reinterpret_cast<void**>(&g_pickup), "the item pickup");
    const bool Lot = Pickup && Hook(kLotFromMapObj, reinterpret_cast<void*>(&LotFromMapObjDetour),
                                    reinterpret_cast<void**>(&g_lotFromMapObj), "the item lot builder");
    g_ok.store(Restore && Pickup && Lot);

    // Independent of the rest: without it a replayed pickup fills the wrong bag.
    Hook(kPickupExec, reinterpret_cast<void*>(&PickupExecDetour),
         reinterpret_cast<void**>(&g_pickupExec), "the pickup action (own pickups only)");

    size_t Remembered = 0;
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        Remembered = g_pending.size();
    }
    LOG_INFO("[LOOT] world item sync %s (restore exe+0x1F31A0, pickup exe+0x1E6C60, lot exe+0x1E26F0),"
             " %zu pickup(s) waiting in %s",
             g_ok.load() ? "hooked" : "unavailable", Remembered, kPendingFile);
    return g_ok.load();
}

void SetLootSyncEnabled(bool enabled) {
    g_enabled.store(enabled);
    LOG_INFO("[LOOT] world item sync %s", enabled
        ? "ON: in someone else's world you get the items your own save has not picked up"
        : "OFF: the game's own rules (no world items for whoever joins)");
}

bool IsLootSyncEnabled() {
    return g_enabled.load();
}

void ToggleLootSyncNow() {
    const bool On = !g_enabled.load();
    SetLootSyncEnabled(On);
    g_request.store(On ? kShow : kHide);
    UI::Overlay::GetInstance().ShowNotification(
        On ? UI::Tr("World items: ON \xE2\x80\x94 everything your own save has not picked up",
                    "Предметы мира: ВКЛ \xE2\x80\x94 всё, что ты ещё не подобрал у себя")
           : UI::Tr("World items: OFF \xE2\x80\x94 as the game has them",
                    "Предметы мира: ВЫКЛ \xE2\x80\x94 как в обычной игре"),
        3.0f, UI::NotifyKind::Info);
}

void LootSyncGameTick() {
    const int Req = g_request.exchange(kNone);
    if (Req == kNone || !g_ok.load() || g_broken.load()) return;
    if (!MultiplayerActiveSafe()) {
        LOG_INFO("[LOOT] F4: not in multiplayer right now -- the world items are the game's own");
        return;
    }
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    std::vector<PendingPod> Pods = OwnerPods(Owner, true, 0);
    AreaStats S{};
    if (!ShowHideSafe(Req == kShow, Pods.data(), static_cast<int32_t>(Pods.size()), &S)) {
        g_broken.store(true);
        LOG_ERROR("[LOOT] F4 threw while walking the loaded areas -- world item sync is off for this run");
        return;
    }
    if (Req == kShow) {
        LOG_INFO("[LOOT] F4 on: %u world items in the loaded areas, %u on the ground now"
                 " (%u put back from your save, %u remembered pickups)",
                 S.Items, S.Shown, S.FromFull + S.FromCompact, S.Remembered);
    } else {
        LOG_INFO("[LOOT] F4 off: %u world items taken off the ground", S.Hidden);
    }
}

} // namespace DS2Coop::Sync
