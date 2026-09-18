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

// Treasure chests (docs §3.36). The lid is the object's StateAct state; what was
// taken is the drop handle at component +0x68 (lot +0x70, rolled +0x74, lying
// +0x76, taken +0x7A).
constexpr uint32_t kChestComp       = 0x1E5C80;   // object -> the treasure box component, or 0
constexpr uint32_t kChestState      = 0x3F2D30;   // (component) -> the lid's state
constexpr uint32_t kChestKind2      = 0x3F2A10;   // (component) -> a kind-2 box (its lot swapped for 0x2FAF468)
constexpr uint32_t kChestRegister   = 0x1CFF10;   // (component, enable): register or remove the chest's drop
constexpr uint32_t kChestInit       = 0x1CF930;   // (component, flag): roll the chest's lot, make its drop
constexpr uint32_t kObjStateSet     = 0x3C1EA0;   // (object, state, 0): set the object's StateAct state
constexpr uint32_t kObjLotIndex     = 0x3BA640;   // (object) -> lot index for the current cycle
constexpr uint32_t kObjRow          = 0x3BA6A0;   // (object) -> its param row
constexpr uint32_t kSvrEventVtable  = 0x10E9FB8;  // the server-event chest: it reports takes by itself
constexpr uint32_t kChestHandleOff  = 0x68;
constexpr uint32_t kChestLotOff     = 0x70;
constexpr uint32_t kChestTakenOff   = 0x7A;

// Walls and other objects broken in someone else's world (17.09, point 17: a wall an
// enemy blew up at the host's stayed shut in the guest's own world, while a shortcut
// opened by hand -- a flag -- was open). Broken is a StateAct state, not a flag: the
// host's break reaches the guest as state packet '$' (exe+0x1F4CD0 -> StateAct vt[0xA8])
// and nothing records it in multiplayer (exe+0x1F3A80 skips the store). The guest
// remembers every saved object that packet leaves broken (exe+0x3C1800 -> 2), and at
// home puts the same state on it once the area's saved states are in (exe+0x3C1EA0,
// which swaps in the broken model); the line goes once a home load reads it broken.
constexpr uint32_t kStatePacket     = 0x1F4CD0;   // listener (self, data, size 0xC): the host's object state
constexpr uint32_t kObjFromRef      = 0x17BD90;   // (object reference*) -> object
constexpr uint32_t kObjBrokenState  = 0x3C1800;   // (object) -> 0 no StateAct, 1 whole, 2 broken

// A chest this player opened at home without taking what was inside (17.09, point 13:
// in the host's world it was empty). The lid state dispatcher exe+0x1D0620(box, state)
// rolls a chest only on the opening (tens 50) and the open states 90/120 (exe+0x1D0520,
// when nothing was ever taken or left inside); the settled open lids 60/80 roll
// nothing. A host who had opened that chest sends its lid as 60/80, so the guest's
// copy stayed empty whatever the guest's own save held. So when a lid settles open on
// an empty, never-rolled chest in someone else's world, and this player's own record
// says the chest still holds something, that record is put in -- the leftovers of
// this player's own opening, which a pickup then remembers as usual.
constexpr uint32_t kChestLidState   = 0x1D0620;   // (box component, state)
constexpr uint32_t kKind2Lot        = 0x2FAF468;
constexpr uint32_t kPackedTaken     = 1u << 24;

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
using StatePacketFn   = void(__fastcall*)(void* listener, uint32_t* data, int64_t size);
using ObjFromRefFn    = uintptr_t(__fastcall*)(const uint32_t* ref);
using ObjStateFn      = uint8_t(__fastcall*)(uintptr_t obj);
using ChestLidFn      = void(__fastcall*)(uintptr_t box, uint8_t state);

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
// A chest line also carries the lid's state and the NG cycle it was taken in.
struct Pending {
    std::string Owner;
    uint32_t Area = 0;
    uint32_t Object = 0;
    uint32_t Lot = 0;
    uint32_t Packed = 0;
    bool     Chest = false;
    bool     Wall = false;     // an object broken there: State is the StateAct state to put on it
    bool     Kill = false;     // a once-only enemy killed there: Area its raw map, Object its generator id
    uint32_t State = 0;
    uint32_t Cycle = 0;
};

// The same, flat, for the code that runs under SEH.
struct PendingPod {
    uint32_t Area;
    uint32_t Object;
    uint32_t Lot;
    uint32_t Packed;
    uint32_t Applied;
    uint32_t Chest;
    uint32_t State;
    uint32_t Cycle;
    uint32_t Wall;
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
    uint32_t Chests;         // chests in the area
    uint32_t ChestsTaken;    // chests emptied from this player's own records / remembered lines
    uint32_t ChestLidsSet;   // lids put open at home
    uint32_t WallsBroken;    // objects broken at home the way they were broken elsewhere
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
    uint8_t   Chest;       // the drop was a treasure chest's
    uint8_t   State;       // ... its lid state
    uint8_t   Kind2;
    uint8_t   SvrEvent;
    uint32_t  Cycle;
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

// --- treasure chests (docs §3.36) --------------------------------------------------
// A chest's lid and its "taken" were never written anywhere when a guest emptied it
// in the host's world (the multiplayer paths skip every store), so at home the
// record still said "closed, no lot" and opening it rolled the item again -- and the
// other way round, a chest emptied at home could be looted again in the host's world.

using ObjU32Fn      = uint32_t(__fastcall*)(uintptr_t obj);
using ObjStateSetFn = void(__fastcall*)(uintptr_t obj, uint8_t state, int32_t flag);
using ChestRegFn    = void(__fastcall*)(uintptr_t comp, char enable);

bool ChestDropIsLive(uintptr_t Box) {
    const uintptr_t Mgr = Game<DropMgrGetFn>(kDropMgrGet)();
    return Mgr && Game<DropMgrHasFn>(kDropMgrHas)(Mgr, reinterpret_cast<uint64_t*>(Box + kChestHandleOff));
}

// The NG cycle this save is in ([[[GMImp+0xA8]+0xC0]+0x68]); 0 if unreadable.
uint32_t CurrentCycle() {
    const uintptr_t Gm = GameManager();
    if (!Gm) return 0;
    const uintptr_t Data = *reinterpret_cast<uintptr_t*>(Gm + 0xA8);
    if (!Data) return 0;
    const uintptr_t Play = *reinterpret_cast<uintptr_t*>(Data + 0xC0);
    return Play ? *reinterpret_cast<uint32_t*>(Play + 0x68) : 0;
}

// Lid states by tens: 90 and 120 are open.
bool LidOpen(uint32_t State) {
    const uint32_t Tens = State / 10 * 10;
    return Tens == 90 || Tens == 120;
}

bool IsSvrEventChest(uintptr_t Box) {
    return *reinterpret_cast<uintptr_t*>(Box) == g_base + kSvrEventVtable;
}

// Puts a "taken" handle into the chest: its live drop (if one was made) goes, the
// handle is restored the way the area load does it, and the chest registers again.
void ApplyChestTaken(uintptr_t StateMgr, uintptr_t Obj, uintptr_t Box, uint32_t Lot, uint32_t Packed,
                     AreaStats* S) {
    const bool Live = ChestDropIsLive(Box);
    if (Live) {
        RemoveDrop(Obj);
        S->WasLive++;
    }
    Game<HandleRestoreFn>(kHandleRestore)(StateMgr, Obj, &Lot, &Packed);
    if (Live) {
        const uintptr_t Ctrl  = *reinterpret_cast<uintptr_t*>(Obj + 0xB8);
        const uintptr_t Model = Ctrl ? *reinterpret_cast<uintptr_t*>(Ctrl + 8) : 0;
        const bool Enable = Model && ((*reinterpret_cast<uint8_t*>(Model + 0xE8) >> 1) & 1);
        Game<ChestRegFn>(kChestRegister)(Box, Enable ? 1 : 0);
    }
    S->ChestsTaken++;
}

void RestoreChest(uintptr_t StateMgr, uintptr_t Obj, uintptr_t Box, uint32_t AreaId, bool Own,
                  const int32_t* Full, int32_t* Compact, PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    S->Chests++;
    if (IsSvrEventChest(Box)) return;                        // reports its own takes (exe+0x1CEDB0)
    if (Game<CompByteFn>(kChestKind2)(Box)) return;          // kind 2: left to the game
    const uint32_t Id = ObjectId(Obj);
    if (!Id) return;

    // This player's own record of it.
    uint32_t OwnLot = 0, OwnPacked = 0;
    bool HaveOwn = false;
    if (Full) {
        const int32_t K = FullRecordIndex(Full, Id);
        if (K >= 0) {
            OwnLot = static_cast<uint32_t>(Full[0x1002 + K]);
            OwnPacked = static_cast<uint32_t>(Full[0x1402 + K]);
            HaveOwn = true;
        }
    }
    if (!HaveOwn && Compact) {
        const uint32_t K = Game<SlotIndexFn>(kSlotIndex)(Compact, static_cast<int32_t>(Id));
        if (K < kCompactMax) {
            OwnLot = static_cast<uint32_t>(Compact[0x182 + K]);
            OwnPacked = static_cast<uint32_t>(Compact[0x242 + K]);
            HaveOwn = true;
        }
    }
    const bool OwnTaken = HaveOwn && (OwnPacked & kPackedTaken) && ((OwnPacked >> 10) & 0x3FF) == 0;

    PendingPod* Line = nullptr;
    for (int32_t P = 0; P < PendCount; ++P) {
        if (Pend[P].Chest && Pend[P].Area == AreaId && Pend[P].Object == Id) {
            Line = &Pend[P];
            break;
        }
    }

    if (Own) {
        // Someone else's world: empty for this player when its own save, or a
        // chest it emptied elsewhere, says so. The lid stays as the host has it.
        uint32_t Lot = 0, Packed = 0;
        if (OwnTaken) {
            Lot = OwnLot;
            Packed = OwnPacked;
        } else if (Line) {
            Lot = Line->Lot;
            Packed = (Line->Packed & 0x3003FF) | kPackedTaken;
        } else {
            return;
        }
        if (*reinterpret_cast<uint8_t*>(Box + kChestTakenOff)) return;   // already empty here
        ApplyChestTaken(StateMgr, Obj, Box, Lot, Packed, S);
        return;
    }

    // Own world: only chests emptied in someone else's world.
    if (!Line) return;
    if (OwnTaken) {
        Line->Applied = 1;   // the game keeps it itself now
        return;
    }
    const uint32_t Cycle = CurrentCycle();
    if (Line->Cycle && Cycle && Line->Cycle != Cycle) {
        Line->Applied = 1;   // another NG cycle: the records were reset
        return;
    }
    // The lot as this world rolls it: in multiplayer the lot index follows the
    // session's cycle, which need not be this save's.
    int32_t Out[2] = { 0, 0 };
    const uintptr_t Row = Game<ObjFn>(kObjRow)(Obj);
    if (Row) {
        Game<LotComputeFn>(kLotCompute)(Out, 1, *reinterpret_cast<int32_t*>(Row + 0x24),
                                        static_cast<int32_t>(Game<ObjU32Fn>(kObjLotIndex)(Obj)));
    }
    const uint32_t Lot = Out[0] ? static_cast<uint32_t>(Out[0]) : Line->Lot;
    const uint32_t Packed = (Line->Packed & 0x3003FF) | kPackedTaken;
    ApplyChestTaken(StateMgr, Obj, Box, Lot, Packed, S);
    // The lid after the handle, so the open-state handler finds it taken.
    if (LidOpen(Line->State) && !LidOpen(Game<CompByteFn>(kChestState)(Box))) {
        Game<ObjStateSetFn>(kObjStateSet)(Obj, static_cast<uint8_t>(Line->State), 0);
        S->ChestLidsSet++;
    }
    S->Remembered++;
    // The line stays until a later load finds the game's own record saying taken.
}

// Own world: an object remembered broken elsewhere. Forgotten once this load finds it
// broken by the game's own saved state.
void RestoreWall(uintptr_t Obj, uint32_t AreaId, PendingPod* Pend, int32_t PendCount, AreaStats* S) {
    uint32_t Id = 0;
    for (int32_t P = 0; P < PendCount; ++P) {
        if (!Pend[P].Wall || Pend[P].Area != AreaId) continue;
        if (!Id) Id = ObjectId(Obj);
        if (!Id) return;
        if (Pend[P].Object != Id) continue;
        const uint8_t Now = Game<ObjStateFn>(kObjBrokenState)(Obj);
        if (Now == 2) {
            Pend[P].Applied = 1;   // the game keeps it itself now
        } else if (Now == 1) {
            Game<ObjStateSetFn>(kObjStateSet)(Obj, static_cast<uint8_t>(Pend[P].State), 0);
            S->WallsBroken++;
        }
        return;
    }
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
    // At home the records are read only for remembered chests: a chest line is
    // forgotten once the game's own record says taken.
    bool ChestLines = false;
    bool WallLines = false;
    for (int32_t P = 0; P < PendCount && !ChestLines; ++P) ChestLines = Pend[P].Chest && Pend[P].Area == AreaId;
    for (int32_t P = 0; P < PendCount && !WallLines; ++P) WallLines = Pend[P].Wall && Pend[P].Area == AreaId;
    if (Own || ChestLines) {
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
        if (!Own && WallLines) RestoreWall(Obj, AreaId, Pend, PendCount, S);
        const uintptr_t Comp = Game<ObjFn>(kGetItemDrop)(Obj);
        if (!Comp) {
            const uintptr_t Box = Game<ObjFn>(kChestComp)(Obj);
            if (Box) RestoreChest(StateMgr, Obj, Box, AreaId, Own, Full, Compact, Pend, PendCount, S);
            continue;
        }
        if (!Own) {
            // Own world: world items only from remembered lines -- the records are
            // the game's and it has already put them back.
            bool Any = false;
            for (int32_t P = 0; P < PendCount && !Any; ++P) Any = !Pend[P].Chest && Pend[P].Area == AreaId;
            if (!Any) continue;
        }
        S->Items++;
        const uint32_t Id = ObjectId(Obj);
        if (!Id) continue;

        uint32_t Lot = 0, Packed = 0;
        bool Have = false;
        PendingPod* Remembered = nullptr;
        for (int32_t P = 0; P < PendCount; ++P) {
            if (Pend[P].Chest || Pend[P].Area != AreaId || Pend[P].Object != Id) continue;
            // A line that never rolled anything says nothing: use the records.
            if (Pend[P].Lot == 0 && (Pend[P].Packed & 0x3FF) == 0) break;
            Lot = Pend[P].Lot;
            Packed = Pend[P].Packed;
            Remembered = &Pend[P];
            Have = true;
            break;
        }
        if (!Have && Full && Own) {
            const int32_t K = FullRecordIndex(Full, Id);
            if (K >= 0) {
                Lot = static_cast<uint32_t>(Full[0x1002 + K]);
                Packed = static_cast<uint32_t>(Full[0x1402 + K]);
                Have = true;
                S->FromFull++;
            }
        }
        if (!Have && Compact && Own) {
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
            if (Comp && *reinterpret_cast<uint64_t*>(Comp + kHandleOff) == Handle) {
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
            // A chest's contents are its own drop, at +0x68 of its component: the
            // pickup of a chest used to be logged as "not a world item".
            if (Comp) continue;
            const uintptr_t Box = Game<ObjFn>(kChestComp)(Obj);
            if (!Box || *reinterpret_cast<uint64_t*>(Box + kChestHandleOff) != Handle) continue;
            P->Object = Obj;
            P->Comp = Box;
            P->Area = *reinterpret_cast<uint32_t*>(Area + 8);
            P->Id = ObjectId(Obj);
            Game<HandleWordFn>(kHandleLot)(Box + kChestHandleOff, &P->LotBefore);
            P->PackedBefore = 0;
            Game<HandleWordFn>(kHandlePack)(Box + kChestHandleOff, &P->PackedBefore);
            P->Chest = 1;
            P->State = Game<CompByteFn>(kChestState)(Box);
            P->Kind2 = Game<CompByteFn>(kChestKind2)(Box) ? 1 : 0;
            P->SvrEvent = IsSvrEventChest(Box) ? 1 : 0;
            P->Cycle = CurrentCycle();
            P->Found = 1;
            return;
        }
    }
}

void SnapshotAfterImpl(PickupProbe* P) {
    const uintptr_t HandleAt = P->Comp + (P->Chest ? kChestHandleOff : kHandleOff);
    Game<HandleWordFn>(kHandleLot)(HandleAt, &P->LotAfter);
    P->PackedAfter = 0;
    Game<HandleWordFn>(kHandlePack)(HandleAt, &P->PackedAfter);
    P->StillLive = (P->Chest ? ChestDropIsLive(P->Comp) : DropIsLive(P->Comp)) ? 1 : 0;
    if (P->Chest) P->State = Game<CompByteFn>(kChestState)(P->Comp);
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
           "# owner\tarea\tobject\tlot\tstate\n"
           "# A chest emptied there: owner\tarea\tobject\tlot\tstate\tC\tlid\tcycle -- kept until\n"
           "# the owner's own save says the chest is empty.\n"
           "# An object broken there (a wall): owner\tarea\tobject\t0\t0\tW\tstate\t0 -- kept until\n"
           "# the owner's own world loads it broken.\n"
           "# A once-only enemy killed there whose drop was taken: owner\tmap\tgenerator\t0\t0\tK\t0\tcycle\n"
           "# -- counted as killed in the owner's own world the next time that map loads.\n";
    char Numbers[96];
    for (const Pending& E : g_pending) {
        if (E.Kill) {
            std::snprintf(Numbers, sizeof(Numbers), "\t%08X\t%08X\t00000000\t00000000\tK\t0\t%u\n",
                          E.Area, E.Object, E.Cycle);
        } else if (E.Wall) {
            std::snprintf(Numbers, sizeof(Numbers), "\t%08X\t%08X\t00000000\t00000000\tW\t%u\t0\n",
                          E.Area, E.Object, E.State);
        } else if (E.Chest) {
            std::snprintf(Numbers, sizeof(Numbers), "\t%08X\t%08X\t%08X\t%08X\tC\t%u\t%u\n",
                          E.Area, E.Object, E.Lot, E.Packed, E.State, E.Cycle);
        } else {
            std::snprintf(Numbers, sizeof(Numbers), "\t%08X\t%08X\t%08X\t%08X\n",
                          E.Area, E.Object, E.Lot, E.Packed);
        }
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
        const bool ChestLine = Parts.size() == 8 && Parts[5] == "C";
        const bool WallLine = Parts.size() == 8 && Parts[5] == "W";
        const bool KillLine = Parts.size() == 8 && Parts[5] == "K";
        if ((Parts.size() != 5 && !ChestLine && !WallLine && !KillLine) || Parts[0].empty()) continue;
        try {
            Pending E;
            E.Owner = Parts[0];
            E.Area = static_cast<uint32_t>(std::stoul(Parts[1], nullptr, 16));
            E.Object = static_cast<uint32_t>(std::stoul(Parts[2], nullptr, 16));
            E.Lot = static_cast<uint32_t>(std::stoul(Parts[3], nullptr, 16));
            E.Packed = static_cast<uint32_t>(std::stoul(Parts[4], nullptr, 16));
            if (ChestLine) {
                E.Chest = true;
                E.State = static_cast<uint32_t>(std::stoul(Parts[6]));
                E.Cycle = static_cast<uint32_t>(std::stoul(Parts[7]));
            } else if (WallLine) {
                E.Wall = true;
                E.State = static_cast<uint32_t>(std::stoul(Parts[6]));
            } else if (KillLine) {
                E.Kill = true;
                E.Cycle = static_cast<uint32_t>(std::stoul(Parts[7]));
            }
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
        if (E.Kill || E.Owner != Owner || (!AllAreas && E.Area != AreaId)) continue;
        Pods.push_back(PendingPod{ E.Area, E.Object, E.Lot, E.Packed, 0, E.Chest ? 1u : 0u, E.State, E.Cycle,
                                   E.Wall ? 1u : 0u });
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
            if (!E.Kill && E.Owner == Owner && E.Area == Pod.Area && E.Object == Pod.Object &&
                E.Chest == (Pod.Chest != 0) && E.Wall == (Pod.Wall != 0)) {
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

// A chest emptied in someone else's world: remembered with its lid and cycle, so
// the owner's own copy of that chest is open and empty too.
void RememberChest(const PickupProbe& P, const std::string& Owner) {
    const uint32_t Lot = P.LotAfter ? P.LotAfter : P.LotBefore;
    if (P.Kind2 || P.SvrEvent || Lot == kKind2Lot) {
        LOG_INFO("[LOOT] chest %u in area %u is of a kind the game keeps by itself -- not remembered", P.Id, P.Area);
        return;
    }
    if (!(P.PackedAfter & kPackedTaken) || ((P.PackedAfter >> 10) & 0x3FF)) {
        LOG_INFO("[LOOT] chest %u in area %u still holds something (handle %08X) -- not remembered yet",
                 P.Id, P.Area, P.PackedAfter);
        return;
    }
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        bool Replaced = false;
        for (Pending& E : g_pending) {
            if (E.Owner == Owner && E.Area == P.Area && E.Object == P.Id && E.Chest && !E.Wall) {
                E.Lot = Lot;
                E.Packed = P.PackedAfter;
                E.State = P.State;
                E.Cycle = P.Cycle;
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
            E.Packed = P.PackedAfter;
            E.Chest = true;
            E.State = P.State;
            E.Cycle = P.Cycle;
            g_pending.push_back(E);
        }
        SavePendingLocked();
    }
    LOG_INFO("[LOOT] chest emptied in multiplayer: area %u object %u lot %u (handle %08X -> %08X, lid %u, cycle %u)"
             " -- %s's own chest will be open and empty as well",
             P.Area, P.Id, Lot, P.PackedBefore, P.PackedAfter, P.State, P.Cycle, Owner.c_str());
    UI::Overlay::GetInstance().ShowNotification(
        UI::Tr("Chest emptied \xE2\x80\x94 it will be empty in your own world too",
               "Сундук опустошён \xE2\x80\x94 в твоём мире он тоже будет пустым"),
        2.5f, UI::NotifyKind::Success);
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
    if (P.Chest) {
        RememberChest(P, Owner);
        return;
    }
    const uint32_t Lot = P.LotAfter ? P.LotAfter : P.LotBefore;
    const uint32_t Packed = TakenState(P);
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        bool Replaced = false;
        for (Pending& E : g_pending) {
            if (E.Owner == Owner && E.Area == P.Area && E.Object == P.Id && !E.Chest && !E.Wall && !E.Kill) {
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
                 " (%u full + %u compact), %u remembered pickups, %u rebuilt; %u chests, %u empty for you",
                 AreaId, S.Items, S.FromFull + S.FromCompact, S.FromFull, S.FromCompact,
                 S.Remembered, S.WasLive, S.Chests, S.ChestsTaken);
        ApplyHostChestLids(AreaId);   // chest_lids.cpp: the host's open chests, after this player's own records
        const ULONGLONG Now = GetTickCount64();
        if (S.Items && Now - g_lastAreaToast.load() > 120000) {
            g_lastAreaToast.store(Now);
            UI::Overlay::GetInstance().ShowNotification(
                UI::Tr("World items here follow your own save",
                       "Предметы мира здесь \xE2\x80\x94 по твоему сейву"),
                3.5f, UI::NotifyKind::Info);
        }
    } else {
        LOG_INFO("[LOOT] area %u in your own world: %u pickups made in multiplayer applied (%u now kept by the game);"
                 " %u chests emptied, %u lids opened, %u objects broken as they were elsewhere",
                 AreaId, S.Remembered, Forgotten, S.ChestsTaken, S.ChestLidsSet, S.WallsBroken);
    }
}

// A chest that is already empty must not roll its lot again. The open-state
// handler (exe+0x1D0520) checks for that before it asks; the opening animation's
// call (exe+0x1D06C0, state 50) and the drop rebuild (exe+0x3B9FD0) do not, and a
// chest emptied elsewhere but closed here would hand its item out a second time.
// exe+0x1CF930 returns nothing and starts with PUSH RBP / PUSH RDI / PUSH R14 --
// whole instructions, no RIP use.
using ChestInitFn = void(__fastcall*)(uintptr_t comp, char flag);
ChestInitFn g_chestInit = nullptr;
std::atomic<uint32_t> g_chestRerollsRefused{ 0 };

bool ChestAlreadyEmptySafe(uintptr_t Box, uint32_t* Lot) {
    __try {
        if (IsSvrEventChest(Box)) return false;   // reports its own takes
        *Lot = *reinterpret_cast<uint32_t*>(Box + kChestLotOff);
        return *Lot != 0 && *reinterpret_cast<uint8_t*>(Box + kChestTakenOff) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall ChestInitDetour(uintptr_t Box, char Flag) {
    if (Box && g_ok.load() && g_enabled.load() && !g_broken.load()) {
        uint32_t Lot = 0;
        if (ChestAlreadyEmptySafe(Box, &Lot)) {
            const uint32_t N = g_chestRerollsRefused.fetch_add(1) + 1;
            if (N <= 10 || N % 100 == 0) {
                LOG_INFO("[LOOT] an emptied chest (lot %u) was about to roll its contents again -- left empty (%u so far)",
                         Lot, N);
            }
            return;
        }
    }
    g_chestInit(Box, Flag);
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
    if (P.Found) std::snprintf(Where, sizeof(Where), "area %u %s %u", P.Area, P.Chest ? "chest" : "object", P.Id);
    LOG_INFO("[LOOT] pickup of drop %016llX (%s): %s -> inventory %s (%08X), drop %s",
             static_cast<unsigned long long>(Handle), Where, Items.c_str(),
             !HaveVerdict ? "unreadable" : (Verdict & 0x80000000u) ? "REFUSED" : "took it",
             Verdict, StillThere ? "still there" : "gone");
}

// --- a once-only enemy killed in the host's world (17.09, second report point 6) -------------
// An enemy that is killed once and for all (a crystal lizard: its generator's limit is 1 and no
// death flag) is kept dead by its kill counter alone. In the host's world a guest's kill is never
// counted in its own save -- exe+0x40FDB0 skips the count in a world entered by a multiplayer warp,
// and a count there would land in the session slot, thrown away on leaving -- so at home it spawned
// again and dropped its item a second time ("the drop also fell in my world, from the same snake,
// which should be dead"). When the guest takes such an enemy's drop, the kill is remembered; the
// next time that map loads in the guest's own world it is counted there (exe+0x1F63E0), before the
// map's generators are made. Only taking the drop counts: a drop left lying stays at home.
constexpr uint32_t kGenOfHandle   = 0x17B7E0;   // (&[rec+0x10]) -> the record's generator
constexpr uint32_t kKillCountGet  = 0x1F6760;   // (kill store, area slot, generator id) -> AL: times killed
constexpr uint32_t kKillCountUp   = 0x1F63E0;   // (kill store, area slot, generator id): once more
constexpr uint32_t kAreaBySlot    = 0x3BCE60;   // (mapMgr, area slot) -> area; its raw map at +8
constexpr uintptr_t kKillSlotSize = 0xB10;
using KillCountGetFn = uint64_t(__fastcall*)(uintptr_t, uint32_t, uint32_t);
using KillCountUpFn  = void(__fastcall*)(uintptr_t, uint32_t, uint32_t);

struct GeneratorDrop {
    uintptr_t Rec;
    int32_t   Map;
    uint16_t  GenId;
};

struct KillFacts {
    bool    Taken;
    bool    Lying;
    uint8_t Limit;
    int32_t DeathFlag;
    uint8_t RowBits;
};

// The generator record whose drop this handle is ([rec+0x88]), in any loaded block.
bool FindGeneratorDropSafe(uint64_t Handle, GeneratorDrop* Out) {
    __try {
        const uintptr_t Gm = GameManager();
        const uintptr_t GenMgr = Gm ? *reinterpret_cast<uintptr_t*>(Gm + 0x40) : 0;
        if (!GenMgr) return false;
        for (int32_t Slot = 0; Slot < 0x2A; ++Slot) {
            const uintptr_t Block = *reinterpret_cast<uintptr_t*>(GenMgr + 0x20 + static_cast<uintptr_t>(Slot) * 8);
            if (!Block) continue;
            const uintptr_t First = *reinterpret_cast<uintptr_t*>(Block + 0x18);
            uint32_t N = *reinterpret_cast<uint32_t*>(Block + 0x20);
            if (!First) continue;
            if (N > 4096) N = 4096;
            for (uint32_t K = 0; K < N; ++K) {
                const uintptr_t Rec = First + K * 0xA0;
                if (*reinterpret_cast<uint64_t*>(Rec + 0x88) != Handle) continue;
                Out->Rec = Rec;
                Out->Map = *reinterpret_cast<int32_t*>(Block + 0x24);
                Out->GenId = *reinterpret_cast<uint16_t*>(Rec + 0x68);
                return true;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadKillFactsSafe(uintptr_t Rec, KillFacts* F) {
    __try {
        F->Taken = *reinterpret_cast<uint8_t*>(Rec + 0x9A) != 0;
        F->Lying = *reinterpret_cast<uint16_t*>(Rec + 0x96) != 0;
        const uintptr_t Gen = Game<ObjFn>(kGenOfHandle)(Rec + 0x10);
        if (!Gen) return false;
        F->Limit = *reinterpret_cast<uint8_t*>(Gen + 0x8D);
        const uintptr_t Row = *reinterpret_cast<uintptr_t*>(Gen + 0x58);
        if (!Row) return false;
        F->DeathFlag = *reinterpret_cast<int32_t*>(Row + 4);
        F->RowBits = *reinterpret_cast<uint8_t*>(Row + 0x48);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RememberOnceOnlyKill(const GeneratorDrop& D) {
    KillFacts F{};
    if (!ReadKillFactsSafe(D.Rec, &F)) {
        LOG_INFO("[LOOT] an enemy's drop taken (generator %u of map %08X) -- its generator is unreadable", D.GenId,
                 static_cast<uint32_t>(D.Map));
        return;
    }
    const bool OnceOnly = F.Limit == 1 && F.DeathFlag == 0 && !(F.RowBits & 0x40);
    if (!F.Taken || F.Lying || !OnceOnly) {
        LOG_INFO("[LOOT] an enemy's drop taken (generator %u of map %08X: limit %u, death flag %d, row bits %02X, "
                 "taken %d, lying %d) -- %s", D.GenId, static_cast<uint32_t>(D.Map), F.Limit, F.DeathFlag,
                 F.RowBits, F.Taken ? 1 : 0, F.Lying ? 1 : 0,
                 OnceOnly ? "not all of it is taken yet" : "it comes back anyway: nothing to remember");
        return;
    }
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    if (Owner.empty()) return;
    const uint32_t Cycle = CurrentCycle();
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        for (const Pending& E : g_pending) {
            if (E.Kill && E.Owner == Owner && E.Area == static_cast<uint32_t>(D.Map) && E.Object == D.GenId) return;
        }
        Pending E;
        E.Owner = Owner;
        E.Area = static_cast<uint32_t>(D.Map);
        E.Object = D.GenId;
        E.Kill = true;
        E.Cycle = Cycle;
        g_pending.push_back(E);
        SavePendingLocked();
    }
    LOG_INFO("[LOOT] a once-only enemy (generator %u of map %08X) killed here and its drop taken -- %s's own "
             "world will have it dead too", D.GenId, static_cast<uint32_t>(D.Map), Owner.c_str());
}

bool AreaMapBySlotSafe(uintptr_t MapMgr, int32_t Slot, int32_t* Map) {
    __try {
        const uintptr_t Area = Game<AreaByIndexFn>(kAreaBySlot)(MapMgr, Slot);
        if (!Area) return false;
        *Map = *reinterpret_cast<int32_t*>(Area + 8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Counts one kill of `GenId` in this player's own slot for the area; false if that slot names
// another map or anything faulted. Before/After are the kill counts read around it.
bool CountHomeKillSafe(uintptr_t Store, int32_t Slot, int32_t Map, uint16_t GenId, uint32_t* Before, uint32_t* After) {
    __try {
        if (*reinterpret_cast<int32_t*>(Store + 8 + static_cast<uintptr_t>(Slot) * kKillSlotSize) != Map) return false;
        *Before = static_cast<uint32_t>(Game<KillCountGetFn>(kKillCountGet)(Store, Slot, GenId) & 0xFF);
        if (*Before == 0) Game<KillCountUpFn>(kKillCountUp)(Store, Slot, GenId);
        *After = static_cast<uint32_t>(Game<KillCountGetFn>(kKillCountGet)(Store, Slot, GenId) & 0xFF);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t __fastcall PickupDetour(uint64_t* DropHandle) {
    if (!DropHandle) return g_pickup(DropHandle);
    const uint64_t Handle = *DropHandle;

    // Read-only diagnostics for every pickup, in any world.
    DropContents Contents{};
    const bool HaveContents = ReadDropSafe(Handle, &Contents);
    PickupProbe P{};
    const bool Searched = FindDropSafe(Handle, &P);
    GeneratorDrop EnemyDrop{};
    const bool FromEnemy = Searched && !P.Found && g_ok.load() && g_enabled.load() && MultiplayerActiveSafe() &&
                           FindGeneratorDropSafe(Handle, &EnemyDrop);

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
    } else if (FromEnemy) {
        RememberOnceOnlyKill(EnemyDrop);
    } else if (Searched) {
        LOG_INFO("[LOOT] picked up a drop that is not a world item (enemy drop) -- nothing to remember");
    }
    return Result;
}

}  // namespace (reopened below)

// Right before a map's generators are made in this player's own world: the once-only enemies
// killed in someone else's world, counted here (guest_world.cpp calls this).
void ApplyHomeKillsBeforeArea(void* GenMgr, int32_t AreaIndex) {
    if (!GenMgr || AreaIndex < 0 || AreaIndex >= 0x2A || !g_ok.load() || !g_enabled.load() || g_broken.load()) return;
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        bool Any = false;
        for (const Pending& E : g_pending) Any = Any || E.Kill;
        if (!Any) return;
    }
    if (MultiplayerActiveSafe()) return;   // someone else's world: that count goes to the session slot
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    const uintptr_t MapMgr = MapManager();
    int32_t Map = 0;
    if (Owner.empty() || !MapMgr || !AreaMapBySlotSafe(MapMgr, AreaIndex, &Map)) return;
    const uintptr_t Store = *reinterpret_cast<uintptr_t*>(reinterpret_cast<uintptr_t>(GenMgr) + 0x10);
    if (!Store) return;
    const uint32_t Cycle = CurrentCycle();
    std::lock_guard<std::mutex> Lock(g_pendingMutex);
    bool Changed = false;
    for (size_t I = 0; I < g_pending.size();) {
        const Pending E = g_pending[I];
        if (!E.Kill || E.Owner != Owner || E.Area != static_cast<uint32_t>(Map)) {
            ++I;
            continue;
        }
        if (E.Cycle && Cycle && E.Cycle != Cycle) {
            LOG_INFO("[LOOT] once-only enemy %u of map %08X was killed in another NG cycle -- forgotten", E.Object,
                     E.Area);
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(I));
            Changed = true;
            continue;
        }
        uint32_t Before = 0, After = 0;
        if (!CountHomeKillSafe(Store, AreaIndex, Map, static_cast<uint16_t>(E.Object), &Before, &After)) {
            LOG_WARNING("[LOOT] once-only enemy %u of map %08X: its kill could not be counted here (slot %d)",
                        E.Object, E.Area, AreaIndex);
            ++I;
            continue;
        }
        LOG_INFO("[LOOT] once-only enemy %u of map %08X, killed in someone else's world: kill count here %u -> %u%s",
                 E.Object, E.Area, Before, After, After ? " -- it stays dead, and its drop does not fall again" : "");
        if (After) {
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(I));
            Changed = true;
            continue;
        }
        ++I;
    }
    if (Changed) SavePendingLocked();
}

namespace {

// --- a chest opened at home and left full -------------------------------------------
ChestLidFn g_chestLid = nullptr;
std::atomic<uint32_t> g_chestLeftoversPut{ 0 };

struct LeftoverFacts {
    uint32_t Area;
    uint32_t Id;
    uint32_t Lot;
    uint32_t Packed;
    bool     Put;
    bool     Untouched;   // this player's own save never opened the chest
};

// The area a loaded object is in (0 when not found).
uint32_t AreaOfObject(uintptr_t MapMgr, uintptr_t Obj) {
    const uintptr_t Areas = *reinterpret_cast<uintptr_t*>(MapMgr + 8);
    if (!Areas) return 0;
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
            if (ResolveObject(Objects[I]) == Obj) return *reinterpret_cast<uint32_t*>(Area + 8);
        }
    }
    return 0;
}

void PutChestLeftoversImpl(uintptr_t Box, PendingPod* Pend, int32_t PendCount, LeftoverFacts* F) {
    if (IsSvrEventChest(Box) || Game<CompByteFn>(kChestKind2)(Box)) return;
    if (*reinterpret_cast<uint32_t*>(Box + kChestLotOff) != 0 || *reinterpret_cast<uint8_t*>(Box + kChestTakenOff)) return;
    if (ChestDropIsLive(Box)) return;
    const uintptr_t Obj = *reinterpret_cast<uintptr_t*>(Box + 8);
    const uintptr_t MapMgr = MapManager();
    if (!Obj || !MapMgr) return;
    F->Id = ObjectId(Obj);
    if (!F->Id) return;
    F->Area = AreaOfObject(MapMgr, Obj);
    if (!F->Area) return;
    for (int32_t P = 0; P < PendCount; ++P) {
        if (Pend[P].Chest && Pend[P].Area == F->Area && Pend[P].Object == F->Id) return;   // emptied elsewhere
    }
    const uintptr_t StateMgr = *reinterpret_cast<uintptr_t*>(MapMgr + 0x1F8);
    bool Have = false;
    const uintptr_t Store = *reinterpret_cast<uintptr_t*>(MapMgr + 0x200);
    if (Store) {
        const int32_t* Full = Game<RecordFindFn>(kRecordFind)(Store, static_cast<int32_t>(F->Area));
        if (Full && static_cast<uint32_t>(Full[0]) == F->Area) {
            const int32_t K = FullRecordIndex(Full, F->Id);
            if (K >= 0) {
                F->Lot = static_cast<uint32_t>(Full[0x1002 + K]);
                F->Packed = static_cast<uint32_t>(Full[0x1402 + K]);
                Have = true;
            }
        }
    }
    if (!Have && StateMgr) {
        int32_t* Compact = Game<SlotFindFn>(kSlotFind)(reinterpret_cast<int32_t*>(StateMgr + 0x24), kCompactSlots,
                                                      static_cast<int32_t>(F->Area));
        if (Compact) {
            const uint32_t K = Game<SlotIndexFn>(kSlotIndex)(Compact, static_cast<int32_t>(F->Id));
            if (K < kCompactMax) {
                F->Lot = static_cast<uint32_t>(Compact[0x182 + K]);
                F->Packed = static_cast<uint32_t>(Compact[0x242 + K]);
                Have = true;
            }
        }
    }
    // Never opened at home: no record, or one that was never rolled and never taken.
    if (!Have || (!F->Lot && !(F->Packed & kPackedTaken))) {
        F->Untouched = true;
        return;
    }
    // Rolled at home, taken bit clear, something still lying there.
    if (!F->Lot || (F->Packed & kPackedTaken) || ((F->Packed >> 10) & 0x3FF) == 0) return;
    uint32_t Lot = F->Lot, Packed = F->Packed;
    Game<HandleRestoreFn>(kHandleRestore)(StateMgr, Obj, &Lot, &Packed);
    const uintptr_t Ctrl  = *reinterpret_cast<uintptr_t*>(Obj + 0xB8);
    const uintptr_t Model = Ctrl ? *reinterpret_cast<uintptr_t*>(Ctrl + 8) : 0;
    const bool Enable = Model && ((*reinterpret_cast<uint8_t*>(Model + 0xE8) >> 1) & 1);
    Game<ChestRegFn>(kChestRegister)(Box, Enable ? 1 : 0);
    F->Put = true;
}

// The roll the game makes for a chest whose lid reaches the open states 90/120 with
// nothing ever rolled or taken (exe+0x1D0520: checks the handle itself, then makes the drop
// with this player's own dice).
constexpr uint32_t kChestOpenRoll = 0x1D0520;   // (box component)

bool RollChestSafe(uintptr_t Box, uint32_t* LotAfter) {
    *LotAfter = 0;
    __try {
        Game<CompFn>(kChestOpenRoll)(Box);
        *LotAfter = *reinterpret_cast<uint32_t*>(Box + kChestLotOff);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool PutChestLeftoversSafe(uintptr_t Box, PendingPod* Pend, int32_t PendCount, LeftoverFacts* F) {
    __try {
        PutChestLeftoversImpl(Box, Pend, PendCount, F);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void __fastcall ChestLidDetour(uintptr_t Box, uint8_t State) {
    g_chestLid(Box, State);
    const uint32_t Tens = State / 10 * 10;
    if (!Box || Tens == 10 || Tens == 50 || Tens == 70 || Tens == 90 || Tens == 120) return;
    if (!g_ok.load() || !g_enabled.load() || g_broken.load() || !MultiplayerActiveSafe()) return;
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    std::vector<PendingPod> Pods = OwnerPods(Owner, true, 0);
    LeftoverFacts F{};
    if (!PutChestLeftoversSafe(Box, Pods.data(), static_cast<int32_t>(Pods.size()), &F)) {
        LOG_WARNING("[LOOT] putting a chest's leftovers from your own save threw -- left as it is");
        return;
    }
    // A chest the host opened that this player never opened at home (17.09, second report
    // point 7: empty in the host's world, full at home). The host's lid arrives settled
    // open (60/80), and those states roll nothing -- so it is rolled the way the open
    // states 90/120 roll a never-opened chest, for this player. Its pickup is remembered
    // like any other, so the chest at home is empty afterwards.
    const uint32_t Settled = State / 10 * 10;
    if (!F.Put && F.Untouched && (Settled == 60 || Settled == 80)) {
        uint32_t LotAfter = 0;
        const bool Rolled = RollChestSafe(Box, &LotAfter);
        LOG_INFO("[LOOT] chest %u in area %u: the host's lid is open (%u), the chest empty here and never opened in "
                 "your own world -- rolled for you: %s (lot %u)", F.Id, F.Area, State,
                 Rolled ? (LotAfter ? "done" : "nothing came of it") : "threw", LotAfter);
        return;
    }
    if (!F.Put) return;
    g_chestLeftoversPut.fetch_add(1);
    LOG_INFO("[LOOT] chest %u in area %u: the host's lid is open (%u) and the chest empty here, but your own save "
             "still has something in it (lot %u, handle %08X) -- put in", F.Id, F.Area, State, F.Lot, F.Packed);
}

// --- objects broken in someone else's world ---------------------------------------
StatePacketFn g_statePacket = nullptr;

struct BrokenObject {
    uintptr_t Obj;
    uint32_t  Area;
    uint32_t  Id;
};

// The object a state packet names, when the packet has left it broken, and the
// loaded area it belongs to.
bool FindBrokenImpl(const uint32_t* Data, BrokenObject* Out) {
    if ((*Data & 0xF) != 1) return false;
    const uintptr_t Obj = Game<ObjFromRefFn>(kObjFromRef)(Data);
    if (!Obj || Game<ObjStateFn>(kObjBrokenState)(Obj) != 2) return false;
    const uint32_t Id = ObjectId(Obj);
    if (!Id) return false;   // not saved: nothing at home to carry it over to
    const uintptr_t MapMgr = MapManager();
    if (!MapMgr) return false;
    const uintptr_t Areas = *reinterpret_cast<uintptr_t*>(MapMgr + 8);
    if (!Areas) return false;
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
            if (ResolveObject(Objects[I]) != Obj) continue;
            *Out = BrokenObject{ Obj, *reinterpret_cast<uint32_t*>(Area + 8), Id };
            return true;
        }
    }
    return false;
}

bool FindBrokenSafe(const uint32_t* Data, BrokenObject* Out) {
    __try {
        return FindBrokenImpl(Data, Out);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RememberBroken(const BrokenObject& B, uint8_t State) {
    const std::string Owner = PlayerSync::GetInstance().GetLocalCharacterName();
    if (Owner.empty()) return;
    {
        std::lock_guard<std::mutex> Lock(g_pendingMutex);
        for (const Pending& E : g_pending) {
            if (E.Wall && E.Owner == Owner && E.Area == B.Area && E.Object == B.Id) return;   // known already
        }
        Pending E;
        E.Owner = Owner;
        E.Area = B.Area;
        E.Object = B.Id;
        E.Wall = true;
        E.State = State;
        g_pending.push_back(E);
        SavePendingLocked();
    }
    LOG_INFO("[LOOT] object %u in area %u was broken in the host's world (state %u) -- %s's own world will have it "
             "broken too", B.Id, B.Area, State, Owner.c_str());
}

void __fastcall StatePacketDetour(void* Listener, uint32_t* Data, int64_t Size) {
    g_statePacket(Listener, Data, Size);
    if (Size != 0xC || !Data || !g_ok.load() || !g_enabled.load() || g_broken.load()) return;
    if (!MultiplayerActiveSafe()) return;   // a guest in someone else's world
    BrokenObject B{};
    if (!FindBrokenSafe(Data, &B)) return;
    RememberBroken(B, reinterpret_cast<const uint8_t*>(Data)[4]);
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
    // Chests: an emptied chest never rolls its contents a second time.
    if (g_ok.load()) {
        Hook(kChestInit, reinterpret_cast<void*>(&ChestInitDetour),
             reinterpret_cast<void**>(&g_chestInit), "the chest lot roll (an emptied chest stays empty)");
        // A chest opened at home and left full is not empty in the host's world.
        Hook(kChestLidState, reinterpret_cast<void*>(&ChestLidDetour),
             reinterpret_cast<void**>(&g_chestLid), "the chest lid state (leftovers from your own save)");
        // Walls: an object the host's world broke is broken at home as well.
        Hook(kStatePacket, reinterpret_cast<void*>(&StatePacketDetour),
             reinterpret_cast<void**>(&g_statePacket), "the object state packet (broken walls)");
    }

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
