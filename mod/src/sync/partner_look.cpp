// The partner's copy made again from its new look (0.2.2, the test of 19.09: "after the characters were
// made I see neither the friend's name nor the look -- only the class; in PvP the friend's bar does not
// go down; all is right only after a new summon").
//
// A player's copy on the other machine is built once, by exe+0x51CE20, from the 0x5E4-byte record the
// game queued at the summon (exe+0x51B0E0): name (+0x29C, into the player slot +0x8C that the HUD reads),
// face (+0x18C, 0xA2 bytes), equipment (+0x5C, 0x130), attributes (+0x250, 11 bytes: the copy's max HP,
// chr+0x170 / +0x174), hollowing (+0x230), covenant (+0x231..). Two players who summon each other before
// making their characters keep copies made from records without a name, with the class default look and
// the default attributes -- and the partner's HP, synced each frame, is clamped to that stale max
// (exe+0x37A260), so its bar stood full. Nothing re-applies a record to a live character (the face mesh,
// exe+0x33D3D0, is made once per model), so the copy is destroyed and queued again with a new record,
// inside the player-list tick (travel_sync.cpp).
//
// This file is the sender's half: every few seconds, on the game thread, this player's own record is
// built the way the game builds it for a summon (exe+0x51BA70, as the join controller does at
// exe+0x2C1FE0), and its first 0x2DC bytes go to the partner when the look (name, face, attributes,
// hollowing) differs from the last one sent -- and again now and then, as a packet can be lost. The
// receiver compares it with the record its copy was made from and does nothing when they match
// (docs §3.51).

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

#include <atomic>
#include <cstdint>
#include <cstring>

using namespace DS2Coop::Utils;

namespace DS2Coop::Sync {

namespace {

constexpr uint32_t  kGameManagerImp = 0x16148F0;
constexpr uint32_t  kNetRoot        = 0x1616CF8;
constexpr uint32_t  kBuildRecord    = 0x51BA70;   // (player list, peer id*, u32[7], record) -> AL
constexpr uint32_t  kIdStringInit   = 0x2D8AF0;   // (record + 0x25B): the id string's own init
constexpr uint32_t  kPeerIdDtor     = 0xA3DB80;   // (peer id object, 0x40 bytes)
constexpr uint32_t  kRecordSize     = 0x5E4;
constexpr uint32_t  kPacketMagic    = 0x44533243;
constexpr ULONGLONG kBuildEveryMs   = 3000;
constexpr ULONGLONG kResendMs       = 20000;

std::atomic<bool> g_on{ true };   // ini partner_look_refresh

uintptr_t ExeBase() {
    static const uintptr_t Base = reinterpret_cast<uintptr_t>(GetModuleHandle(nullptr));
    return Base;
}

// In the world with a character, and no event holding the view (the making at the crones' holds it from
// the first line to the end: ESD 132242, [[GMImp+0x70]+0x1B4]).
bool ReadyToBuildSafe() {
    __try {
        const uintptr_t Gm = *reinterpret_cast<const uintptr_t*>(ExeBase() + kGameManagerImp);
        if (!Gm || !*reinterpret_cast<const uintptr_t*>(Gm + 0xD0)) return false;
        if (*reinterpret_cast<const int32_t*>(Gm + 0x24AC) != 0x1E) return false;
        if (*reinterpret_cast<const uint8_t*>(Gm + 0x24B1) & 0x02) return false;   // a warp under way
        const uintptr_t EvMgr = *reinterpret_cast<const uintptr_t*>(Gm + 0x70);
        return EvMgr && *reinterpret_cast<const int32_t*>(EvMgr + 0x1B4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// This player's record, as exe+0x2C1FE0 builds it before it sends it (packet 0x0D): the record zeroed,
// the id string made, the own peer id from the net base (vt[0x38], the same object exe+0x51BA70 asks to
// tell its own record from a partner's), and the seven session words left 0.
bool BuildOwnRecordSafe(uint8_t* Record) {
    __try {
        const uintptr_t Root = *reinterpret_cast<const uintptr_t*>(ExeBase() + kNetRoot);
        if (!Root) return false;
        const uintptr_t NetBase = *reinterpret_cast<const uintptr_t*>(Root + 0x00);
        const uintptr_t List = *reinterpret_cast<const uintptr_t*>(Root + 0x20);
        if (!NetBase || !List) return false;
        std::memset(Record, 0, kRecordSize);
        reinterpret_cast<void(__fastcall*)(uint8_t*)>(ExeBase() + kIdStringInit)(Record + 0x25B);
        alignas(16) uint8_t Id[0x40] = {};
        const uintptr_t Vtbl = *reinterpret_cast<const uintptr_t*>(NetBase);
        using OwnIdFn = void*(__fastcall*)(uintptr_t, void*);
        void* Own = reinterpret_cast<OwnIdFn>(*reinterpret_cast<const uintptr_t*>(Vtbl + 0x38))(NetBase, Id);
        uint32_t Seven[7] = {};
        using BuildFn = uint64_t(__fastcall*)(uintptr_t, void*, uint32_t*, uint8_t*);
        const bool Built = (reinterpret_cast<BuildFn>(ExeBase() + kBuildRecord)(List, Own, Seven, Record) & 0xFF) != 0;
        reinterpret_cast<void(__fastcall*)(void*)>(ExeBase() + kPeerIdDtor)(Id);
        return Built;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool AllZero(const uint8_t* P, size_t N) {
    for (size_t I = 0; I < N; ++I) {
        if (P[I]) return false;
    }
    return true;
}

uint32_t LookHash(const uint8_t* Record) {
    uint32_t Hash = 2166136261u;
    auto Mix = [&Hash](const uint8_t* P, size_t N) {
        for (size_t I = 0; I < N; ++I) {
            Hash ^= P[I];
            Hash *= 16777619u;
        }
    };
    Mix(Record + 0x29C, 0x40);   // name
    Mix(Record + 0x18C, 0xA2);   // face
    Mix(Record + 0x250, 11);     // attributes
    Mix(Record + 0x230, 1);      // hollowing
    return Hash;
}

void SendLook(const uint8_t* Record, uint32_t Seq) {
    Network::PartnerLookPacket Packet{};
    Packet.header.magic = kPacketMagic;
    Packet.header.type = Network::PacketType::PartnerLook;
    Packet.header.size = sizeof(Packet);
    Packet.header.timestamp = GetTickCount64();
    Packet.seq = Seq;
    std::memcpy(Packet.record, Record, sizeof(Packet.record));
    Network::PeerManager::GetInstance().BroadcastPacket(&Packet.header);
}

// Someone to send to. The handshake flag is set only on a guest ("the host answered"), so on 19.09
// the host never sent its look and the guest kept the host's copy from before the making; a host
// has a partner once a guest is in the lobby.
bool HavePartner() {
    auto& Lobby = Session::SessionManager::GetInstance();
    if (!Lobby.IsActive()) return false;
    if (Lobby.IsHost()) return Lobby.GetPlayers().size() >= 2;
    return Network::PeerManager::GetInstance().IsHandshakeConfirmed();
}

} // namespace

void PartnerLookTick() {
    static ULONGLONG s_at = 0, s_sentAt = 0;
    static uint32_t s_hash = 0, s_seq = 0, s_buildFails = 0;
    if (!g_on.load()) return;
    const ULONGLONG Now = GetTickCount64();
    if (Now - s_at < kBuildEveryMs) return;
    s_at = Now;
    if (!HavePartner()) return;
    if (!ReadyToBuildSafe() || PlayerSync::GetInstance().GetOwnCharacterName().empty()) return;
    alignas(16) static uint8_t s_record[kRecordSize];
    if (!BuildOwnRecordSafe(s_record)) {
        if (++s_buildFails <= 3) LOG_WARNING("[LOOK] building my own record threw or failed -- nothing sent");
        return;
    }
    if (AllZero(s_record + 0x18C, 0xA2) || !*reinterpret_cast<const uint16_t*>(s_record + 0x29C)) return;
    const uint32_t Hash = LookHash(s_record);
    const bool IsNew = Hash != s_hash;
    if (!IsNew && Now - s_sentAt < kResendMs) return;
    // A resend keeps its number: the partner drops a number it already has, and a new one would be
    // a new look to compare (19.09, a line every 20 s on the host).
    if (IsNew) {
        ++s_seq;
        LOG_INFO("[LOOK] my character's look (name, face, attributes) is new -- sent to the partner (#%u)", s_seq);
    }
    s_hash = Hash;
    s_sentAt = Now;
    SendLook(s_record, s_seq);
}

void SetPartnerLookRefresh(bool On) {
    g_on.store(On);
    SetPartnerLookSwap(On);   // travel_sync.cpp: the receiver's half
}

} // namespace DS2Coop::Sync
