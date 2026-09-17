// NPC progress for both players (17.09, points 2 and 8; docs §3.44).
//
// Talk progress. A talk script records what was said with its flag command
// (exe+0x462169 -> exe+0x474A60(flags, id, value), return exe+0x46216E). That setter
// drops the write when there is a session, the world was entered by a multiplayer
// warp (exe+0x5135F0) and exe+0x25CDB0(id) is false -- every id under 1 000 000 in
// the groups from 3 up, the groups 10 and 20 talk progress is kept in among them. So
// a guest's Emerald Herald never got past her first line and never offered levelling
// up. While a talk is open ([[[GMImp+0x70]+0x48]+0x40] != 0, docs §3.14) a guest's
// talk-script write the game would drop is made with the game's own setter
// exe+0x4750B0 instead, without the game's flag packet to the others.
//
// Gifts. An item a talk gives -- command 0x1FDBB (exe+0x198A10) or an item lot
// through 0x2014A (exe+0x199CC0 / exe+0x19A3C0) -- ends in exe+0x1AC3D0(inventory,
// items, count), a jump into ItemGive, called from exactly those three places.
// Map events run the same commands, so only a give made while a talk is open counts.
// What a talk gave goes to the partner (packet NpcGift), whose game adds each item
// only if that player has none of it: asked for on 17.09 -- an item an NPC gives
// goes to both, unless the other player already has it (a key, the Estus Flask).

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

#pragma intrinsic(_ReturnAddress)

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp  = 0x16148F0;
constexpr uint32_t  kNetRoot         = 0x1616CF8;
constexpr uint32_t  kJoinCtrlVtable  = 0x10D7BD8;
constexpr uint32_t  kFlagSet         = 0x474A60;   // (flags, id, value): the setter with the multiplayer guard
constexpr uint32_t  kFlagSetRaw      = 0x4750B0;   // (flags, id, value) -> AL: the bit changed
constexpr uint32_t  kFlagGet         = 0x474230;   // (flags, id) -> AL
constexpr uint32_t  kFlagGuestMay    = 0x25CDB0;   // (id) -> AL: a guest's write is let through
constexpr uint32_t  kTalkFlagReturn  = 0x46216E;   // the script's flag command
constexpr uint32_t  kGiveWrap        = 0x1AC3D0;   // (inventory, items, count) -> AL: jumps into ItemGive
constexpr uint32_t  kGiveItemReturn  = 0x198B33;   // command 0x1FDBB, one item
constexpr uint32_t  kGiveLotReturnA  = 0x199DA3;   // command 0x2014A, an item lot
constexpr uint32_t  kGiveLotReturnB  = 0x19A4D6;   // command 0x2014A, an item lot, the other path
constexpr uint32_t  kCountItem       = 0x1A6AC0;   // (bag, id, u8* flags) -> count
constexpr uint32_t  kItemGive        = 0x1A7470;   // (bag, items, count, mode) -> AL
constexpr uint8_t   kCountFlags      = 7;          // bag and item box
constexpr uint32_t  kMaxGiftItems    = 16;
constexpr ULONGLONG kGiftKeepMs      = 10 * 60 * 1000;   // a gift waits this long for the player to stand in the game
constexpr uint32_t  kTalkWritesLogged = 300;

using FlagSetFn  = void(__fastcall*)(void* flags, uint32_t id, char value);
using FlagRawFn  = bool(__fastcall*)(void* flags, uint32_t id, char value);
using FlagGetFn  = bool(__fastcall*)(void* flags, uint32_t id);
using GuestMayFn = bool(__fastcall*)(uint32_t id);
using GiveWrapFn = bool(__fastcall*)(void* inventory, Network::NpcGiftItem* items, int32_t count, int64_t mode);
using CountFn    = int32_t(__fastcall*)(void* bag, int32_t id, uint8_t* flags);
using ItemGiveFn = bool(__fastcall*)(void* bag, Network::NpcGiftItem* items, int32_t count, int32_t mode);

FlagSetFn  g_flagSetOriginal  = nullptr;
GiveWrapFn g_giveWrapOriginal = nullptr;
std::atomic<bool>     g_enabled{ true };
std::atomic<uint32_t> g_talkWrites{ 0 };

// Gifts from the partner, parked by the network thread for the game thread.
struct PendingGift {
    Network::NpcGiftItem Items[kMaxGiftItems];
    uint32_t             Count;
    ULONGLONG            At;
    char                 From[32];
};
std::mutex  g_giftMutex;
PendingGift g_gifts[8];
uint32_t    g_giftCount = 0;

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

// A talk is open: the NPC handle the talk command wrote into EventTalkManager.
bool TalkOpen() {
    uintptr_t Gm = 0, Events = 0, Talk = 0, Handle = 0;
    return ReadPtr(ExeBase() + kGameManagerImp, &Gm) && ReadPtr(Gm + 0x70, &Events) && ReadPtr(Events + 0x48, &Talk) &&
           ReadPtr(Talk + 0x40, &Handle);
}

bool GuestInHostWorld() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive() || Lobby.IsHost()) return false;
    uintptr_t Root = 0, Mp = 0, Ctrl = 0, Vtbl = 0;
    if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp) || !ReadPtr(Mp + 0x40, &Ctrl)) return false;
    if (!ReadPtr(Ctrl, &Vtbl) || Vtbl != ExeBase() + kJoinCtrlVtable) return false;
    __try {
        return *reinterpret_cast<const int32_t*>(Ctrl + 0xF8) == 7;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool GuestMaySafe(uint32_t Id, bool* Ok) {
    __try {
        *Ok = true;
        return reinterpret_cast<GuestMayFn>(ExeBase() + kFlagGuestMay)(Id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Ok = false;
        return true;
    }
}

// -1 when it could not be read.
int ReadFlagSafe(void* Flags, uint32_t Id) {
    __try {
        return reinterpret_cast<FlagGetFn>(ExeBase() + kFlagGet)(Flags, Id) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool WriteFlagRawSafe(void* Flags, uint32_t Id, char Value, bool* Threw) {
    *Threw = false;
    __try {
        return reinterpret_cast<FlagRawFn>(ExeBase() + kFlagSetRaw)(Flags, Id, Value);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Threw = true;
        return false;
    }
}

void __fastcall FlagSetDetour(void* Flags, uint32_t Id, char Value) {
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    if (Ret != kTalkFlagReturn || !g_enabled.load(std::memory_order_relaxed) || !Flags || !TalkOpen() ||
        !GuestInHostWorld()) {
        g_flagSetOriginal(Flags, Id, Value);
        return;
    }
    bool Ok = false;
    if (GuestMaySafe(Id, &Ok) || !Ok) {   // the game lets this one through itself
        g_flagSetOriginal(Flags, Id, Value);
        return;
    }
    const int Before = ReadFlagSafe(Flags, Id);
    bool Threw = false;
    const bool Changed = WriteFlagRawSafe(Flags, Id, Value, &Threw);
    const int After = ReadFlagSafe(Flags, Id);
    const uint32_t N = g_talkWrites.fetch_add(1) + 1;
    if (N <= kTalkWritesLogged) {
        LOG_INFO("[TALK] a talk script set flag %u = %d, a guest's write the game drops -- written here: %s "
                 "(was %d, now %d)", Id, Value ? 1 : 0, Threw ? "threw" : Changed ? "changed" : "no change",
                 Before, After);
    }
}

// The player's own inventory ([[GMImp+0xA8]+0x10]) and bag ([inventory+0x10]).
bool LocalInventorySafe(uintptr_t* Inventory, uintptr_t* Bag, bool* InGame) {
    *Inventory = 0;
    *Bag = 0;
    *InGame = false;
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return false;
        *InGame = *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) != 0 &&
                  *reinterpret_cast<const int32_t*>(Gm + 0x24AC) == 0x1E;
        const uintptr_t A = *reinterpret_cast<const uintptr_t*>(Gm + 0xA8);
        *Inventory = A ? *reinterpret_cast<const uintptr_t*>(A + 0x10) : 0;
        *Bag = *Inventory ? *reinterpret_cast<const uintptr_t*>(*Inventory + 0x10) : 0;
        return *Inventory && *Bag;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool CopyGiftSafe(const Network::NpcGiftItem* Items, int32_t Count, Network::NpcGiftPacket* Packet) {
    __try {
        uint32_t N = Count > 0 ? static_cast<uint32_t>(Count) : 0;
        if (N > kMaxGiftItems) N = kMaxGiftItems;
        std::memcpy(Packet->items, Items, N * sizeof(Network::NpcGiftItem));
        Packet->count = N;
        return N > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool __fastcall GiveWrapDetour(void* Inventory, Network::NpcGiftItem* Items, int32_t Count, int64_t Mode) {
    const uintptr_t Ret = reinterpret_cast<uintptr_t>(_ReturnAddress()) - ExeBase();
    const bool Given = g_giveWrapOriginal(Inventory, Items, Count, Mode);
    if (!Given || !Items || (Ret != kGiveItemReturn && Ret != kGiveLotReturnA && Ret != kGiveLotReturnB)) return Given;
    if (!g_enabled.load(std::memory_order_relaxed) || !Session::SessionManager::GetInstance().IsActive() || !TalkOpen()) {
        return Given;
    }
    uintptr_t Own = 0, Bag = 0;
    bool InGame = false;
    if (!LocalInventorySafe(&Own, &Bag, &InGame) || Own != reinterpret_cast<uintptr_t>(Inventory)) return Given;

    Network::NpcGiftPacket Packet{};
    if (!CopyGiftSafe(Items, Count, &Packet)) return Given;
    Packet.header.magic = 0x44533243;
    Packet.header.type = Network::PacketType::NpcGift;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
    for (uint32_t I = 0; I < Packet.count; ++I) {
        LOG_INFO("[TALK] an NPC gave me item %d x%d -- the partner gets it too if it has none",
                 Packet.items[I].id, Packet.items[I].count);
    }
    return Given;
}

int32_t CountItemSafe(uintptr_t Bag, int32_t Id) {
    __try {
        uint8_t Flags = kCountFlags;
        return reinterpret_cast<CountFn>(ExeBase() + kCountItem)(reinterpret_cast<void*>(Bag), Id, &Flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool GiveItemSafe(uintptr_t Bag, Network::NpcGiftItem Item, bool* Threw) {
    *Threw = false;
    __try {
        return reinterpret_cast<ItemGiveFn>(ExeBase() + kItemGive)(reinterpret_cast<void*>(Bag), &Item, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Threw = true;
        return false;
    }
}

// Game thread: each item the partner got that this player has none of.
void ApplyGift(const PendingGift& Gift, uintptr_t Bag) {
    int Added = 0;
    for (uint32_t I = 0; I < Gift.Count; ++I) {
        const Network::NpcGiftItem& Item = Gift.Items[I];
        if (Item.id <= 0 || Item.count <= 0) continue;
        const int32_t Have = CountItemSafe(Bag, Item.id);
        if (Have != 0) {
            LOG_INFO("[TALK] %s got item %d x%d from an NPC -- %s", Gift.From, Item.id, Item.count,
                     Have > 0 ? "I have it already, not added" : "my inventory could not be read, not added");
            continue;
        }
        bool Threw = false;
        const bool Given = GiveItemSafe(Bag, Item, &Threw);
        const int32_t After = CountItemSafe(Bag, Item.id);
        LOG_INFO("[TALK] %s got item %d x%d from an NPC and I had none -- ItemGive %s, count now %d", Gift.From,
                 Item.id, Item.count, Threw ? "threw" : Given ? "gave it" : "refused", After);
        if (After > 0) ++Added;
    }
    if (Added > 0) {
        UI::Overlay::GetInstance().ShowNotification(
            UI::Format(UI::Tr("%s got an item from an NPC \xE2\x80\x94 you have it now too",
                              "%s получил предмет от NPC \xE2\x80\x94 теперь он есть и у тебя"), Gift.From),
            5.0f, UI::NotifyKind::Success);
    }
}

} // namespace

bool InstallNpcProgress(bool Enabled) {
    static bool Installed = false;
    g_enabled.store(Enabled);
    if (!Installed) {
        Installed = true;
        auto& Hooks = Hooks::HookManager::GetInstance();
        if (!Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kFlagSet), reinterpret_cast<void*>(&FlagSetDetour),
                               reinterpret_cast<void**>(&g_flagSetOriginal))) {
            g_flagSetOriginal = nullptr;
            LOG_WARNING("[TALK] could not hook the flag setter exe+0x%X", kFlagSet);
        }
        if (!Hooks.InstallHook(reinterpret_cast<void*>(ExeBase() + kGiveWrap), reinterpret_cast<void*>(&GiveWrapDetour),
                               reinterpret_cast<void**>(&g_giveWrapOriginal))) {
            g_giveWrapOriginal = nullptr;
            LOG_WARNING("[TALK] could not hook the talk give exe+0x%X", kGiveWrap);
        }
    }
    LOG_INFO("[TALK] NPC progress for both players %s", Enabled
        ? "ON: a guest's talk progress is kept, and what an NPC gives goes to the partner too if it has none"
        : "off (npc_progress=false)");
    return g_flagSetOriginal && g_giveWrapOriginal;
}

void NotePartnerNpcGift(const void* Items, uint32_t Count, const std::string& From) {
    if (!Items || !Count || !g_enabled.load()) return;
    PendingGift Gift{};
    Gift.Count = Count > kMaxGiftItems ? kMaxGiftItems : Count;
    std::memcpy(Gift.Items, Items, Gift.Count * sizeof(Network::NpcGiftItem));
    Gift.At = GetTickCount64();
    strncpy_s(Gift.From, sizeof(Gift.From), From.c_str(), _TRUNCATE);
    std::lock_guard<std::mutex> Lock(g_giftMutex);
    if (g_giftCount < sizeof(g_gifts) / sizeof(g_gifts[0])) {
        g_gifts[g_giftCount++] = Gift;
    } else {
        LOG_WARNING("[TALK] %s's NPC gift dropped: eight are already waiting", From.c_str());
    }
}

void NpcProgressGameTick() {
    PendingGift Taken[8];
    uint32_t TakenCount = 0;
    {
        std::lock_guard<std::mutex> Lock(g_giftMutex);
        if (!g_giftCount) return;
        const ULONGLONG Now = GetTickCount64();
        uintptr_t Inventory = 0, Bag = 0;
        bool InGame = false;
        const bool Ready = LocalInventorySafe(&Inventory, &Bag, &InGame) && InGame;
        uint32_t Kept = 0;
        for (uint32_t I = 0; I < g_giftCount; ++I) {
            if (Ready) {
                Taken[TakenCount++] = g_gifts[I];
            } else if (Now - g_gifts[I].At < kGiftKeepMs) {
                g_gifts[Kept++] = g_gifts[I];
            } else {
                LOG_WARNING("[TALK] %s's NPC gift dropped: I was not in the game for ten minutes", g_gifts[I].From);
            }
        }
        g_giftCount = Kept;
    }
    if (!TakenCount) return;
    uintptr_t Inventory = 0, Bag = 0;
    bool InGame = false;
    if (!LocalInventorySafe(&Inventory, &Bag, &InGame)) return;
    for (uint32_t I = 0; I < TakenCount; ++I) ApplyGift(Taken[I], Bag);
}

} // namespace DS2Coop::Sync
