// A character's own death, below the session layer (docs §3.28).
//
// Asked for on 12.09: a guest who dies should not be thrown out and summoned
// again. The session half of that already exists (death_sync.cpp holds the
// phantom branch back in boss fights), but the guest still ends up on the
// ground, because the game has no way to put a dead character back on its feet:
// every place that writes HP -- exe+0x16A300, 0x16A4B0, 0x16A560, 0x16A5A0,
// 0x16A400 and the init store -- leaves every death marker alone, nothing ever
// writes ChrDeadActionCtrl+0x10 back to 0, and the only "revive" in the game is
// the generator re-initialising a record, which is a fresh spawn.
//
// What there is instead: a death is *requested* by one byte, and consumed later.
//
//   [chr+0xB8]+0x759 = 1        a death has been asked for
//   +0x75C..+0x76D              who/what/how (0x768 is the death type, 10 plain)
//   exe+0x13C720                ChrDeadActionCtrl::Update consumes it -- and is
//                               the ONLY place in the executable that clears
//                               +0x759 -- then sets its own state, the status
//                               bits (0x4000, 0x8000, 0x400000 in +0x4C8), the
//                               ragdoll, the animation, and finally the records
//                               that end in "warp home" or "warp to a bonfire".
//
// So the cheap way to keep a guest alive in the host's world is to deny the
// death before it is consumed: put HP back and clear that byte. Nothing has
// happened yet at that point. That is not built here, on purpose -- this file
// only watches, because all of the above is static analysis and none of it has
// been seen running. Note also that denying a death means the mod then has to
// decide what happens instead, or the guest is simply immortal.
//
// Deliberately no hook. exe+0x13C720's signature is not established (an Update
// may or may not take a delta), and getting a detour's signature wrong on a
// per-frame function is how a mod crashes a game. Every field below is a plain
// read off the local character, taken from the game tick the mod already has.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/utils.h"

#include <cstdint>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;  // *(exe+...) = GameManagerImp; +0xD0 local player
constexpr ptrdiff_t kStatusInChr    = 0xB8;       // chr+0xB8 -> the status block
constexpr ptrdiff_t kHp             = 0x168;      // chr+0x168 HP, +0x16C floor, +0x170 cap, +0x174 max
constexpr ptrdiff_t kDeathAsked     = 0x759;      // status: a death has been requested
constexpr ptrdiff_t kDeathType      = 0x768;      // status: which kind of death (10 = plain)
constexpr ptrdiff_t kWarpPending    = 0x5FC;      // status: RequestWarp counter -- blocks the consumer
constexpr ptrdiff_t kDeathVariant   = 0x5D0;      // status: picks state 1 or 2 in the consumer
constexpr ptrdiff_t kStatusBits     = 0x4C8;      // status: 0x4000 dying, 0x8000 dead, 0x400000 died once

struct Snapshot {
    bool     Ok;
    int32_t  Hp;
    int32_t  Floor;
    int32_t  Cap;
    int32_t  Max;
    uint8_t  Asked;
    uint32_t Type;
    uint32_t WarpPending;
    uint32_t Variant;
    uint64_t Bits;
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
        return false;
    }
}

Snapshot Take() {
    Snapshot S{};
    __try {
        uintptr_t Gm = 0, Chr = 0, Status = 0;
        if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm)) return S;
        if (!ReadPtr(Gm + 0xD0, &Chr)) return S;
        if (!ReadPtr(Chr + kStatusInChr, &Status)) return S;
        S.Hp          = *reinterpret_cast<const int32_t*>(Chr + kHp);
        S.Floor       = *reinterpret_cast<const int32_t*>(Chr + kHp + 4);
        S.Cap         = *reinterpret_cast<const int32_t*>(Chr + kHp + 8);
        S.Max         = *reinterpret_cast<const int32_t*>(Chr + kHp + 12);
        S.Asked       = *reinterpret_cast<const uint8_t*>(Status + kDeathAsked);
        S.Type        = *reinterpret_cast<const uint32_t*>(Status + kDeathType);
        S.WarpPending = *reinterpret_cast<const uint32_t*>(Status + kWarpPending);
        S.Variant     = *reinterpret_cast<const uint32_t*>(Status + kDeathVariant);
        S.Bits        = *reinterpret_cast<const uint64_t*>(Status + kStatusBits);
        S.Ok = true;
        return S;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S.Ok = false;
        return S;
    }
}

constexpr uint64_t kInteresting = 0x4000ull | 0x8000ull | 0x400000ull;

} // namespace

// Called from the game tick. Says nothing while nothing happens: a line goes out
// when the death request appears or clears, when HP crosses zero, or when one of
// the three death bits changes.
void ChrDeathTick() {
    const Snapshot Now = Take();
    if (!Now.Ok) return;

    static bool     s_have = false;
    static Snapshot s_last{};
    static uint32_t s_lines = 0;

    const bool AskedChanged = !s_have || Now.Asked != s_last.Asked;
    const bool AliveChanged = !s_have || (Now.Hp <= 0) != (s_last.Hp <= 0);
    const bool BitsChanged  = !s_have || (Now.Bits & kInteresting) != (s_last.Bits & kInteresting);

    s_have = true;
    const Snapshot Was = s_last;
    s_last = Now;

    if (!AskedChanged && !AliveChanged && !BitsChanged) return;
    if (s_lines >= 60) return;
    ++s_lines;

    LOG_INFO("[DEAD] death asked %u (was %u), HP %d/%d (floor %d, cap %d), type %u, variant %u, "
             "warp pending %u, bits 0x%llX -> dying %d, dead %d, died once %d",
             static_cast<unsigned>(Now.Asked), static_cast<unsigned>(Was.Asked),
             Now.Hp, Now.Max, Now.Floor, Now.Cap, Now.Type, Now.Variant, Now.WarpPending,
             static_cast<unsigned long long>(Now.Bits),
             (Now.Bits & 0x4000ull) ? 1 : 0, (Now.Bits & 0x8000ull) ? 1 : 0,
             (Now.Bits & 0x400000ull) ? 1 : 0);
}

} // namespace DS2Coop::Sync
