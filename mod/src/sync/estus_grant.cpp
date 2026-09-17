// The Estus Flask from the mod's menu, for a player who has none (17.09, point 8).
//
// A guest talked to the Emerald Herald in the host's world, got no flask, and the
// Herald at home no longer offers it: the talk's progress reached this save, the
// item did not. Asked for: a menu button like the soapstones one, shown only while
// the flask is missing, that never collides with the Herald giving one and never
// resets a flask that has been upgraded since.
//
// What the code says (docs §3.44):
//   * the flask is one item, 60155000, whatever its level; the levels and the uses
//     live in its inventory entry (+0x24 uses, +0x25 shards, +0x26 bone dust);
//   * exe+0x1A6AC0(bag, id, &flags) counts it -- the bonfire menu asks exactly this
//     (flags 3; 7 also counts the item box);
//   * ItemGive exe+0x1A7470(bag, items, n, mode) adds it; the first-flask set-up
//     exe+0x1AE470 runs only when there is no flask entry yet, so a give never
//     touches the levels of a flask that exists;
//   * every add goes through exe+0x1AA810(bag, item). While this player stands in
//     the game with a flask already counted, a second flask for this player's own
//     bag is dropped there -- the Herald's gift included -- so there is never a
//     second entry.
// All inventory calls run on the game thread (exe+0x1BB0F0 writes a lookup cache).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/ui.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <atomic>
#include <cstdint>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kCountItem      = 0x1A6AC0;    // (bag, item id, u8* flags) -> count
constexpr uint32_t  kItemGive       = 0x1A7470;    // (bag, items, count, mode) -> AL: given
constexpr uint32_t  kAddItem        = 0x1AA810;    // (bag, item): the add every give ends in
constexpr int32_t   kEstusFlask     = 60155000;    // 0x0395E478, every level
constexpr uint8_t   kCountFlags     = 7;           // bag and item box
constexpr int32_t   kInGame         = 0x1E;        // [GMImp+0x24AC]
constexpr ULONGLONG kCheckEveryMs   = 1000;
constexpr ULONGLONG kCountFreshMs   = 2500;        // a count this old still guards the add

#pragma pack(push, 1)
struct GiveItem {                                  // what exe+0x198A10 builds for a script's gift
    int32_t  kind;                                 // 0, as the game passes
    int32_t  id;
    float    durability;
    int16_t  count;
    uint8_t  upgrade;
    uint8_t  infusion;
};
#pragma pack(pop)

using CountFn = int32_t(__fastcall*)(void* bag, int32_t id, uint8_t* flags);
using GiveFn  = bool(__fastcall*)(void* bag, GiveItem* items, int32_t count, int32_t mode);
using AddFn   = void(__fastcall*)(void* bag, GiveItem* item);

AddFn g_addOriginal = nullptr;

std::atomic<int>       g_state{ -1 };       // -1 unknown, 0 no flask, 1 has one
std::atomic<uintptr_t> g_stateBag{ 0 };     // the bag that count was taken in
std::atomic<ULONGLONG> g_stateAt{ 0 };
std::atomic<bool>      g_grantWanted{ false };
std::atomic<uint32_t>  g_duplicatesDropped{ 0 };

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

// [[[GMImp+0xA8]+0x10]+0x10], the bag the soapstone grant uses. 0 while loading.
uintptr_t LocalBagSafe(bool* InGame) {
    *InGame = false;
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm) return 0;
        *InGame = *reinterpret_cast<const uintptr_t*>(Gm + 0xD0) != 0 &&
                  *reinterpret_cast<const int32_t*>(Gm + 0x24AC) == kInGame;
        const uintptr_t A = *reinterpret_cast<const uintptr_t*>(Gm + 0xA8);
        const uintptr_t B = A ? *reinterpret_cast<const uintptr_t*>(A + 0x10) : 0;
        return B ? *reinterpret_cast<const uintptr_t*>(B + 0x10) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int32_t CountFlaskSafe(uintptr_t Bag) {
    __try {
        uint8_t Flags = kCountFlags;
        return reinterpret_cast<CountFn>(ExeBase() + kCountItem)(reinterpret_cast<void*>(Bag), kEstusFlask, &Flags);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool GiveFlaskSafe(uintptr_t Bag, bool* Threw) {
    *Threw = false;
    __try {
        GiveItem Item{ 0, kEstusFlask, 0.0f, 1, 0, 0 };
        return reinterpret_cast<GiveFn>(ExeBase() + kItemGive)(reinterpret_cast<void*>(Bag), &Item, 1, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        *Threw = true;
        return false;
    }
}

void __fastcall AddItemDetour(void* Bag, GiveItem* Item) {
    if (Item && Item->id == kEstusFlask && g_state.load(std::memory_order_relaxed) == 1 &&
        reinterpret_cast<uintptr_t>(Bag) == g_stateBag.load(std::memory_order_relaxed) &&
        GetTickCount64() - g_stateAt.load(std::memory_order_relaxed) < kCountFreshMs) {
        bool InGame = false;
        if (LocalBagSafe(&InGame) == reinterpret_cast<uintptr_t>(Bag) && InGame) {
            g_duplicatesDropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    }
    g_addOriginal(Bag, Item);
}

void Toast(const char* En, const char* Ru, UI::NotifyKind Kind) {
    UI::Overlay::GetInstance().ShowNotification(UI::Tr(En, Ru), 5.0f, Kind);
}

} // namespace

bool InstallEstusGrant() {
    static bool Installed = false;
    if (Installed) return g_addOriginal != nullptr;
    Installed = true;
    const bool Ok = Hooks::HookManager::GetInstance().InstallHook(
        reinterpret_cast<void*>(ExeBase() + kAddItem), reinterpret_cast<void*>(&AddItemDetour),
        reinterpret_cast<void**>(&g_addOriginal));
    if (!Ok) g_addOriginal = nullptr;
    LOG_INFO("[ESTUS] item add exe+0x%X %s", kAddItem,
             Ok ? "hooked: a player who has the flask never gets a second one"
                : "NOT hooked: the flask button stays hidden");
    return Ok;
}

int GetEstusFlaskState() {
    return g_addOriginal ? g_state.load() : -1;
}

void RequestEstusGrant() {
    g_grantWanted.store(true);
}

void EstusGameTick() {
    static ULONGLONG s_checkedAt = 0;
    const ULONGLONG Now = GetTickCount64();
    const bool Wanted = g_grantWanted.load();
    if (!Wanted && Now - s_checkedAt < kCheckEveryMs) return;
    s_checkedAt = Now;

    if (const uint32_t Dropped = g_duplicatesDropped.exchange(0)) {
        LOG_INFO("[ESTUS] a second Estus Flask for this player was not added (%u time(s)) -- the one in the "
                 "inventory keeps its level and uses", Dropped);
    }

    bool InGame = false;
    const uintptr_t Bag = LocalBagSafe(&InGame);
    const int32_t Count = (Bag && InGame) ? CountFlaskSafe(Bag) : -1;
    const int State = Count < 0 ? -1 : Count > 0 ? 1 : 0;
    const int Was = g_state.exchange(State);
    g_stateBag.store(Bag);
    g_stateAt.store(Now);
    if (State != Was && State >= 0) {
        LOG_INFO("[ESTUS] Estus Flask in the inventory: %s (count %d)", State ? "yes" : "no", Count);
    }

    if (!g_grantWanted.exchange(false)) return;
    if (!g_addOriginal) return;
    if (State != 0) {
        LOG_INFO("[ESTUS] flask asked for from the menu, not given: %s",
                 State > 0 ? "there already is one" : "not standing in the game");
        if (State > 0) {
            Toast("You already have the Estus Flask.", "Фляга с эстусом у тебя уже есть.", UI::NotifyKind::Info);
        } else {
            Toast("Could not check the inventory \xE2\x80\x94 try again in the game.",
                  "Не удалось проверить инвентарь \xE2\x80\x94 попробуй уже в игре.", UI::NotifyKind::Warning);
        }
        return;
    }
    bool Threw = false;
    const bool Given = GiveFlaskSafe(Bag, &Threw);
    const int32_t After = CountFlaskSafe(Bag);
    LOG_INFO("[ESTUS] flask asked for from the menu: ItemGive %s, count now %d",
             Threw ? "threw" : Given ? "gave it" : "refused", After);
    if (After > 0) {
        g_state.store(1);
        Toast("Estus Flask added to your inventory.", "Фляга с эстусом добавлена в инвентарь.", UI::NotifyKind::Success);
    } else {
        Toast("The game did not add the flask.", "Игра не выдала флягу.", UI::NotifyKind::Warning);
    }
}

} // namespace DS2Coop::Sync
