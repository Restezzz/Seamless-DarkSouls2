// How the two players stand towards each other: no damage, friendly fire, or a
// real fight.
//
// Asked for on 12.09, three positions: (a) hits between the players do nothing,
// which is how it works today; (b) hits land, but neither player can lock the
// camera on to the other; (c) the partner counts as a hostile character, the way
// an invader does.
//
// All hostility in this game comes out of one 28x28 byte matrix at
// exe+0x1583E40 (row stride 0x1C). A character's effective team is
//
//   pt = [chr+0xB0]                                  // PlayerType
//   if   [pt+0x4D]   team = *(u32*)(exe+0x1584150 + 4*pt[0x3D])
//   elif [pt+0x4E]   team = *(u32*)(exe+0x15841C0 + 4*pt[0x3D])
//   else             team =  pt[0x3D]
//
// and three predicates read matrix[a*28 + b]: exe+0x44A6E0 "hostile" (the value
// is 1 or 3), exe+0x44A690 "same team" (0), exe+0x44A6B0 "hostile, strict" (1).
// The damage filter -- exe+0x13A6A0, vtable slot [40] of all four
// DamageActionCtrl classes -- reads nothing else about the two characters, so
// the team byte decides damage by itself. Lock-on is a separate test
// (exe+0x382630 slot [18]) with its own gate: status flag 0x37, the byte at
// [*([chr+0xB8]+0x18) + 0x0B] mask 0x80, whose constant appears in exactly two
// places in the whole executable, both of them targeting. That is the only
// reason mode (b) is possible at all -- damage and lock-on otherwise consult the
// same matrix cell, so no pair of team numbers can separate them.
//
// Nothing is written yet, on purpose. What the matrix values mean is read off
// the shape of the code rather than measured, and the team numbers in this
// project's own older notes -- 513, 515, 1799 -- came from a memory scan that
// was landing in an unrelated field; the team is a single byte, 0..27. So this
// ships as a probe: one line naming both players' bytes and the matrix cells
// between them, in both directions. The modes get written once a session has
// said what those numbers actually are (docs §3.25).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/session.h"
#include "../../include/utils.h"

#include <cstdint>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;  // *(exe+...) = GameManagerImp; +0xD0 local player
constexpr uint32_t  kTeamMatrix     = 0x1583E40;  // 28 x 28 bytes, row stride 0x1C
constexpr uint32_t  kTeamMapA       = 0x1584150;  // used when PlayerType+0x4D is set
constexpr uint32_t  kTeamMapB       = 0x15841C0;  // used when PlayerType+0x4E is set
constexpr uint32_t  kTeamCount      = 28;
constexpr ptrdiff_t kTypeInChr      = 0xB0;       // chr+0xB0 -> PlayerType
constexpr ptrdiff_t kPhantomId      = 0x3C;       // PlayerType+0x3C ChrNetworkPhantomId
constexpr ptrdiff_t kTeamByte       = 0x3D;       // PlayerType+0x3D TeamType, 0..27
constexpr ptrdiff_t kTeamMapFlagA   = 0x4D;
constexpr ptrdiff_t kTeamMapFlagB   = 0x4E;

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

// One character's four bytes and the team the game would actually use for it.
struct Side {
    bool     Ok;
    uint8_t  PhantomId;
    uint8_t  TeamByte;
    uint8_t  MapFlagA;
    uint8_t  MapFlagB;
    uint32_t Effective;
};

Side ReadSide(uintptr_t Chr) {
    Side S{};
    if (!Chr) return S;
    __try {
        uintptr_t Type = 0;
        if (!ReadPtr(Chr + kTypeInChr, &Type)) return S;
        S.PhantomId = *reinterpret_cast<const uint8_t*>(Type + kPhantomId);
        S.TeamByte  = *reinterpret_cast<const uint8_t*>(Type + kTeamByte);
        S.MapFlagA  = *reinterpret_cast<const uint8_t*>(Type + kTeamMapFlagA);
        S.MapFlagB  = *reinterpret_cast<const uint8_t*>(Type + kTeamMapFlagB);
        if (S.MapFlagA) {
            S.Effective = *reinterpret_cast<const uint32_t*>(ExeBase() + kTeamMapA + 4u * S.TeamByte);
        } else if (S.MapFlagB) {
            S.Effective = *reinterpret_cast<const uint32_t*>(ExeBase() + kTeamMapB + 4u * S.TeamByte);
        } else {
            S.Effective = S.TeamByte;
        }
        S.Ok = true;
        return S;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        S.Ok = false;
        return S;
    }
}

// The raw relation byte, or -1 when either team is outside the table.
int MatrixCell(uint32_t A, uint32_t B) {
    if (A >= kTeamCount || B >= kTeamCount) return -1;
    __try {
        return *reinterpret_cast<const uint8_t*>(ExeBase() + kTeamMatrix + A * kTeamCount + B);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

const char* Meaning(int Cell) {
    switch (Cell) {
        case 0:  return "same team, no damage";
        case 1:  return "hostile";
        case 2:  return "neither (allied other team?)";
        case 3:  return "hostile, second kind";
        default: return "unknown";
    }
}

} // namespace

// Once every five seconds while a session is running and the partner's character
// is on hand, and again whenever any of the numbers change. Reads only.
void PvpModesGameTick() {
    if (!Session::SessionManager::GetInstance().IsActive()) return;

    const uintptr_t Partner = GetPartnerCharacter(5000);
    if (!Partner) return;

    uintptr_t Gm = 0, Local = 0;
    if (!ReadPtr(ExeBase() + kGameManagerImp, &Gm) || !ReadPtr(Gm + 0xD0, &Local)) return;

    const Side Mine   = ReadSide(Local);
    const Side Theirs = ReadSide(Partner);
    if (!Mine.Ok || !Theirs.Ok) return;

    const int Out = MatrixCell(Mine.Effective, Theirs.Effective);
    const int In  = MatrixCell(Theirs.Effective, Mine.Effective);

    static ULONGLONG s_at = 0;
    static uint64_t  s_last = 0;
    const uint64_t Now = (static_cast<uint64_t>(Mine.PhantomId) << 56) |
                         (static_cast<uint64_t>(Mine.TeamByte) << 48) |
                         (static_cast<uint64_t>(Theirs.PhantomId) << 40) |
                         (static_cast<uint64_t>(Theirs.TeamByte) << 32) |
                         (static_cast<uint64_t>(static_cast<uint8_t>(Out)) << 8) |
                          static_cast<uint64_t>(static_cast<uint8_t>(In));
    const ULONGLONG Tick = GetTickCount64();
    if (Now == s_last && Tick - s_at < 5000) return;
    s_last = Now;
    s_at = Tick;

    LOG_INFO("[PVP] me: phantom id %u, team byte %u (maps %u/%u) -> team %u | partner: phantom id %u, "
             "team byte %u (maps %u/%u) -> team %u | matrix me->them %d (%s), them->me %d (%s)",
             Mine.PhantomId, Mine.TeamByte, Mine.MapFlagA, Mine.MapFlagB, Mine.Effective,
             Theirs.PhantomId, Theirs.TeamByte, Theirs.MapFlagA, Theirs.MapFlagB, Theirs.Effective,
             Out, Meaning(Out), In, Meaning(In));
}

} // namespace DS2Coop::Sync
