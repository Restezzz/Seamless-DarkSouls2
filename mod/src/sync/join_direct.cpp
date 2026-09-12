// Groundwork for a join with no summon sign at all (docs §3.27).
//
// Asked for on 12.09. What a guest's entry into another player's world actually
// needs is this, and the signature is read off raw bytes rather than guessed:
//
//   bool exe+0x2C6330(mp, SessionJoinInfo* sess, MatchingParam* mparam,
//                     uint32 summonerPlayerId)
//     mp = *(*(exe+0x1616CF8) + 0x18)
//     requires mp+0x40 == 0        (no join controller yet)
//     requires mp+0x48 == mp+0x50  (the accept vector is empty: I am not hosting)
//
// Four server-push paths funnel into that one entry, and which one it is comes
// down to a single byte -- the "kind" in mparam, which exe+0x2AAA60 builds
// entirely from local state: 0 arena, 1 summon sign, 2 break-in, 3 visitor.
// Three of the four never involve a sign object, so the sign is not structurally
// required. Nothing load-bearing comes from it either: the sign id is compared
// in exactly one place inside the listener exe+0x2A0FF0, the guest's arrival
// position comes from the arrival message, and what the two sides actually share
// is the session-name string.
//
// The one thing that cannot be invented is the NRSessionSearchResult inside
// sess: it carries an opaque payload (pointer and size at +0x58/+0x60) that the
// platform layer uses to connect for real, so it has to describe the host's
// genuinely live session. The cheapest way to find out whether it can be
// replayed is to capture one from a normal summon -- which is what this file
// does, and nothing more. No call, no write.
//
// Hooked at exe+0x2C6330 rather than at the listener exe+0x2A0FF0 on purpose:
// the entry's signature is established, the listener's is not.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>

#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/utils.h"

#include <cstdint>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t kNetRoot   = 0x1616CF8;   // *(exe+...) = network root; +0x18 multiplayer manager
constexpr uint32_t kJoinEntry = 0x2C6330;    // (mp, sess, mparam, summonerId) -> a guest goes in

// sess is 0xD8 as copied into the controller, of which the first 0xA8 is the
// session descriptor; mparam is seven dwords plus two bytes.
constexpr uint32_t kSessBytes   = 0xB8;
constexpr uint32_t kMParamBytes = 0x20;
constexpr uint32_t kBlobPeek    = 64;        // first bytes of the opaque payload

using JoinEntryFn = bool(__fastcall*)(void*, void*, void*, uint32_t);
void* g_joinEntryOriginal = nullptr;

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

// Hex into a caller-owned buffer; returns how many bytes were readable.
uint32_t HexBytes(const void* From, uint32_t Count, char* Out, uint32_t OutSize) {
    static const char Digits[] = "0123456789ABCDEF";
    uint32_t Done = 0;
    __try {
        const uint8_t* P = reinterpret_cast<const uint8_t*>(From);
        for (uint32_t I = 0; I < Count && (I * 2 + 3) < OutSize; ++I) {
            Out[I * 2 + 0] = Digits[P[I] >> 4];
            Out[I * 2 + 1] = Digits[P[I] & 0xF];
            Done = I + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    Out[Done * 2] = '\0';
    return Done;
}

void LogManagerState(const char* When) {
    __try {
        uintptr_t Root = 0, Mp = 0;
        if (!ReadPtr(ExeBase() + kNetRoot, &Root) || !ReadPtr(Root + 0x18, &Mp)) return;
        const uint8_t   Busy  = *reinterpret_cast<const uint8_t*>(Mp + 0x08);
        uintptr_t Slot28 = 0, Ctrl = 0, Begin = 0, End = 0;
        ReadPtr(Mp + 0x28, &Slot28);
        ReadPtr(Mp + 0x40, &Ctrl);
        ReadPtr(Mp + 0x48, &Begin);
        ReadPtr(Mp + 0x50, &End);
        const long long Guests = (Begin && End) ? static_cast<long long>((End - Begin) / 8) : 0;
        int State = -1, Reason = -1, Map = -1, LeaveReason = -1, Players = -1;
        if (Ctrl) {
            State       = *reinterpret_cast<const int*>(Ctrl + 0xF8);
            Reason      = *reinterpret_cast<const int*>(Ctrl + 0x120);
            Map         = *reinterpret_cast<const int*>(Ctrl + 0x19C);
            LeaveReason = *reinterpret_cast<const int*>(Ctrl + 0x1CC);
            Players     = *reinterpret_cast<const int*>(Ctrl + 0x1068);
        }
        LOG_INFO("[JOIN] %s: busy %u, mp+0x28 %s, join ctrl %p, accept vector %lld entr%s | "
                 "state %d, reason %d, map %d, leave reason %d, players %d",
                 When, static_cast<unsigned>(Busy), Slot28 ? "set" : "null",
                 reinterpret_cast<void*>(Ctrl), Guests, Guests == 1 ? "y" : "ies",
                 State, Reason, Map, LeaveReason, Players);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[JOIN] %s: reading the multiplayer manager threw -- left alone", When);
    }
}

// Read only. The point is the session descriptor: if a real one can be captured
// here and replayed, a sign-free join is a matter of calling this function
// ourselves; if the payload turns out to be tied to one-shot state, it cannot.
bool __fastcall JoinEntryDetour(void* Mp, void* Sess, void* MParam, uint32_t SummonerId) {
    LogManagerState("a join is starting");

    char Hex[kSessBytes * 2 + 4];
    const uint32_t Got = HexBytes(Sess, kSessBytes, Hex, sizeof(Hex));
    LOG_INFO("[JOIN] summoner id %u, sess %p (%u bytes readable): %s",
             SummonerId, Sess, Got, Hex);

    char MHex[kMParamBytes * 2 + 4];
    const uint32_t MGot = HexBytes(MParam, kMParamBytes, MHex, sizeof(MHex));
    __try {
        if (MParam && MGot >= 0x1A) {
            const uint8_t* M = reinterpret_cast<const uint8_t*>(MParam);
            LOG_INFO("[JOIN] matching param: type %u, kind %u (1 sign, 2 break-in, 3 visitor), "
                     "flag %u, tier %u | raw %s",
                     static_cast<unsigned>(M[0]), static_cast<unsigned>(M[0x18]),
                     static_cast<unsigned>(M[0x19]), static_cast<unsigned>(M[0x11]), MHex);
        } else {
            LOG_INFO("[JOIN] matching param %p raw %s", MParam, MHex);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[JOIN] reading the matching param threw");
    }

    // The opaque payload the platform layer connects with.
    __try {
        if (Sess) {
            uintptr_t Blob = 0;
            ReadPtr(reinterpret_cast<uintptr_t>(Sess) + 0x58, &Blob);
            const uint64_t Size = *reinterpret_cast<const uint64_t*>(reinterpret_cast<uintptr_t>(Sess) + 0x60);
            if (Blob && Size) {
                char BHex[kBlobPeek * 2 + 4];
                const uint32_t Peek = HexBytes(reinterpret_cast<const void*>(Blob),
                                               Size < kBlobPeek ? static_cast<uint32_t>(Size) : kBlobPeek,
                                               BHex, sizeof(BHex));
                LOG_INFO("[JOIN] session payload at %p, %llu bytes, first %u: %s",
                         reinterpret_cast<void*>(Blob), static_cast<unsigned long long>(Size), Peek, BHex);
            } else {
                LOG_INFO("[JOIN] session payload: pointer %p, size %llu -- nothing to read",
                         reinterpret_cast<void*>(Blob), static_cast<unsigned long long>(Size));
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        LOG_WARNING("[JOIN] reading the session payload threw");
    }

    const bool Ok = reinterpret_cast<JoinEntryFn>(g_joinEntryOriginal)(Mp, Sess, MParam, SummonerId);
    LOG_INFO("[JOIN] the game %s the join", Ok ? "took" : "REFUSED");
    LogManagerState("just after");
    return Ok;
}

} // namespace

bool InstallJoinProbe() {
    static bool Installed = false;
    if (Installed) return g_joinEntryOriginal != nullptr;
    Installed = true;
    if (!Hooks::HookManager::GetInstance().InstallHook(reinterpret_cast<void*>(ExeBase() + kJoinEntry),
                                                       reinterpret_cast<void*>(&JoinEntryDetour),
                                                       &g_joinEntryOriginal)) {
        LOG_WARNING("[JOIN] could not watch exe+0x%X -- no session descriptor will be captured", kJoinEntry);
        return false;
    }
    LOG_INFO("[JOIN] watching how a guest goes in (exe+0x%X): the session descriptor is captured from a "
             "normal summon, which is what a sign-free join would have to replay", kJoinEntry);
    return true;
}

} // namespace DS2Coop::Sync
