// Packet handler - processes incoming P2P packets
//
// Routes packets to the appropriate subsystem (session, sync, etc.)

#include "../../include/network.h"
#include "../../include/hooks.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/utils.h"
#include <unordered_map>

using namespace DS2Coop::Network;
using namespace DS2Coop::Utils;

PacketHandler& PacketHandler::GetInstance() {
    static PacketHandler instance;
    return instance;
}

void PacketHandler::HandlePacket(const PacketHeader* packet, const PeerInfo& sender) {
    if (!packet) return;

    switch (packet->type) {
        case PacketType::Handshake:
            if (packet->size >= sizeof(HandshakePacket))
                HandleHandshake(reinterpret_cast<const HandshakePacket*>(packet), sender);
            break;

        case PacketType::Heartbeat:
            break;

        case PacketType::Disconnect:
            LOG_INFO("Player %s disconnected", sender.playerName.c_str());
            {
                auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
                sessionMgr.RemovePlayer(sender.playerId);
            }
            break;

        case PacketType::PlayerPosition:
            if (packet->size >= sizeof(PlayerPositionPacket))
                HandlePlayerPosition(reinterpret_cast<const PlayerPositionPacket*>(packet));
            break;

        case PacketType::PlayerState:
            if (packet->size >= sizeof(PlayerStatePacket))
                HandlePlayerState(reinterpret_cast<const PlayerStatePacket*>(packet));
            break;

        // Recorded, not passed on: NotifyPlayerDeath / NotifyPlayerRespawn
        // broadcast, so calling them here would bounce the news back forever.
        case PacketType::PlayerDeath:
            LOG_INFO("Remote player %s died", sender.playerName.c_str());
            DS2Coop::Session::SessionManager::GetInstance().SetPlayerAlive(sender.playerId, false);
            DS2Coop::Sync::NotePartnerLife(false);
            break;

        case PacketType::PlayerRespawn:
            LOG_INFO("Remote player %s respawned", sender.playerName.c_str());
            DS2Coop::Session::SessionManager::GetInstance().SetPlayerAlive(sender.playerId, true);
            DS2Coop::Sync::NotePartnerLife(true);
            break;

        case PacketType::BossState:
            if (packet->size >= sizeof(BossStatePacket)) {
                const auto* State = reinterpret_cast<const BossStatePacket*>(packet);
                DS2Coop::Sync::NotePartnerBoss(State->active, State->phase);
            }
            break;

        case PacketType::MapOriginInfo:
            if (packet->size >= sizeof(MapOriginPacket)) {
                const auto* Origin = reinterpret_cast<const MapOriginPacket*>(packet);
                DS2Coop::Hooks::NoteRemoteMapOrigin(Origin->area, Origin->x, Origin->y, Origin->z);
            }
            break;

        case PacketType::FlagBulk:
            if (packet->size >= sizeof(FlagBulkPacket)) {
                const auto* Bulk = reinterpret_cast<const FlagBulkPacket*>(packet);
                DS2Coop::Sync::NoteRemoteFlagBulk(Bulk->group, Bulk->bits, Bulk->bytes);
            }
            break;

        case PacketType::BonfireList:
            if (packet->size >= sizeof(BonfireListPacket)) {
                const auto* List = reinterpret_cast<const BonfireListPacket*>(packet);
                DS2Coop::Sync::NotePartnerBonfires(List->entries, List->count);
            }
            break;

        case PacketType::BossDoorCrossed:
            if (packet->size >= sizeof(BossDoorPacket)) {
                const auto* Door = reinterpret_cast<const BossDoorPacket*>(packet);
                DS2Coop::Sync::NoteHostCrossedBossFog(Door->flag);
            }
            break;

        case PacketType::BossDefeated:
            if (packet->size >= sizeof(BossDefeatedPacket))
                HandleBossDefeated(reinterpret_cast<const BossDefeatedPacket*>(packet));
            break;

        case PacketType::SignPlaced:
            LOG_INFO("Remote player placed a sign — refreshing the sign list");
            DS2Coop::Sync::RequestImmediateSignPoll();
            DS2Coop::Sync::ArmAutoSummon();
            break;

        case PacketType::EventFlag:
            if (packet->size >= sizeof(EventFlagPacket))
                HandleEventFlag(reinterpret_cast<const EventFlagPacket*>(packet));
            break;

        case PacketType::WorldReset:
            LOG_INFO("Remote player %s rested at a bonfire -- resetting the world here too",
                     sender.playerName.c_str());
            DS2Coop::Sync::RequestWorldReset(sender.playerName);
            break;

        case PacketType::BonfireRest:
            LOG_INFO("Remote player rested at bonfire");
            break;

        case PacketType::FogGateTransition:
            LOG_INFO("Remote player entered fog gate");
            break;

        default:
            LOG_DEBUG("Unknown packet type: %u from %s",
                      static_cast<uint8_t>(packet->type), sender.playerName.c_str());
            break;
    }
}

void PacketHandler::HandleHandshake(const HandshakePacket* packet, const PeerInfo& sender) {
    if (!packet) return;

    LOG_INFO("Handshake from %s (ID: %llu)", packet->playerName, packet->playerId);

    // Register this player in the session
    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.AddPlayer(packet->playerId, packet->playerName);
}

// Track last sequence per player to discard out-of-order UDP packets
static std::unordered_map<uint64_t, uint32_t> g_lastPosSequence;

void PacketHandler::HandlePlayerPosition(const PlayerPositionPacket* packet) {
    if (!packet) return;

    // Drop out-of-order packets (handles uint32 wrap-around)
    uint32_t& lastSeq = g_lastPosSequence[packet->playerId];
    int32_t seqDiff = static_cast<int32_t>(packet->header.sequence - lastSeq);
    if (seqDiff < 0 && seqDiff > -1000) {
        return; // old packet, discard
    }
    lastSeq = packet->header.sequence;

    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerPosition(packet->playerId, packet->x, packet->y, packet->z);
    sessionMgr.UpdatePlayerArea(packet->playerId, packet->onlineAreaId);

    auto& playerSync = DS2Coop::Sync::PlayerSync::GetInstance();
    playerSync.ApplyRemotePlayerPosition(packet->playerId,
                                         packet->x, packet->y, packet->z,
                                         packet->rotX, packet->rotY, packet->rotZ);
}

void PacketHandler::HandlePlayerState(const PlayerStatePacket* packet) {
    if (!packet) return;

    LOG_DEBUG("Player %llu state: HP %d/%d",
              packet->playerId, packet->health, packet->maxHealth);

    auto& sessionMgr = DS2Coop::Session::SessionManager::GetInstance();
    sessionMgr.UpdatePlayerHealth(packet->playerId, packet->health, packet->maxHealth);

    auto& playerSync = DS2Coop::Sync::PlayerSync::GetInstance();
    playerSync.ApplyRemotePlayerState(packet->playerId,
                                      packet->health, packet->maxHealth,
                                      packet->stamina, packet->maxStamina);
}

void PacketHandler::HandleBossDefeated(const BossDefeatedPacket* packet) {
    if (!packet) return;

    LOG_INFO("Boss %u was defeated by remote player", packet->bossId);

    auto& progressSync = DS2Coop::Sync::ProgressSync::GetInstance();
    progressSync.SyncBossDefeat(packet->bossId);
}

void PacketHandler::HandleEventFlag(const EventFlagPacket* packet) {
    if (!packet) return;

    // Apply it, do not hand it to ProgressSync::SyncEventFlag: that function
    // broadcasts what it is given, so calling it from the receive path made
    // every flag bounce between the two clients forever. It also never wrote
    // the flag into the game, which is why flag syncing had no visible effect.
    DS2Coop::Sync::ApplyRemoteEventFlag(packet->flagId, packet->flagValue);
}
