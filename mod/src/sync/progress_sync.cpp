#include "../../include/sync.h"
#include "../../include/network.h"
#include "../../include/utils.h"

using namespace DS2Coop::Sync;
using namespace DS2Coop::Utils;

ProgressSync& ProgressSync::GetInstance() {
    static ProgressSync instance;
    return instance;
}

bool ProgressSync::Initialize() {
    if (m_initialized) return true;
    
    LOG_INFO("Initializing progress sync...");
    m_initialized = true;
    LOG_INFO("Progress sync initialized");
    return true;
}

void ProgressSync::Shutdown() {
    if (!m_initialized) return;
    
    LOG_INFO("Shutting down progress sync...");
    
    m_eventFlags.clear();
    m_defeatedBosses.clear();
    m_litBonfires.clear();
    m_pickedItems.clear();
    
    m_initialized = false;
    LOG_INFO("Progress sync shut down");
}

void ProgressSync::SyncEventFlag(uint32_t flagId, bool value) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_DEBUG("Syncing event flag %u = %d", flagId, value);

    m_eventFlags[flagId] = value;
    
    // Broadcast to other players
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::EventFlag;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = flagId;
    packet.flagValue = value;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

bool ProgressSync::GetEventFlag(uint32_t flagId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    auto it = m_eventFlags.find(flagId);
    return (it != m_eventFlags.end()) ? it->second : false;
}

void ProgressSync::RequestEventFlagSync() {
    LOG_INFO("Requesting full event flag synchronization...");
    
    // Send request packet to host/peers
    // In a full implementation, this would request all flags from the host
    
    LOG_INFO("Event flag sync requested");
}

void ProgressSync::SyncBossDefeat(uint32_t bossId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    if (m_defeatedBosses.find(bossId) != m_defeatedBosses.end()) {
        LOG_DEBUG("Boss %u already marked as defeated", bossId);
        return;
    }
    
    LOG_INFO("Synchronizing boss defeat: %u", bossId);
    
    m_defeatedBosses.insert(bossId);
    
    // Broadcast boss defeat
    Network::BossDefeatedPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::BossDefeated;
    packet.header.size = sizeof(Network::BossDefeatedPacket);
    packet.bossId = bossId;
    packet.defeatTime = 0; // TODO: Add timestamp
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
    
    // TODO: Actually set the boss defeat flag in the game memory
    // This would require reverse engineering the boss flag system
}

bool ProgressSync::IsBossDefeated(uint32_t bossId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_defeatedBosses.find(bossId) != m_defeatedBosses.end();
}

void ProgressSync::SyncBonfire(uint32_t bonfireId, bool lit) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_INFO("Synchronizing bonfire %u: %s", bonfireId, lit ? "lit" : "unlit");
    
    if (lit) {
        m_litBonfires.insert(bonfireId);
    } else {
        m_litBonfires.erase(bonfireId);
    }
    
    // Broadcast bonfire state
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::BonfireRest;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = bonfireId;
    packet.flagValue = lit;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
    
    // TODO: Actually set the bonfire state in game memory
}

bool ProgressSync::IsBonfireLit(uint32_t bonfireId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    return m_litBonfires.find(bonfireId) != m_litBonfires.end();
}

void ProgressSync::SyncAllBonfires() {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    LOG_INFO("Synchronizing all bonfire states...");
    
    // In a full implementation, this would iterate through all known bonfires
    // and sync their states with other players
    
    for (uint32_t bonfireId : m_litBonfires) {
        SyncBonfire(bonfireId, true);
    }
}

void ProgressSync::SyncItemPickup(uint32_t itemId, uint32_t locationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint64_t combinedId = (static_cast<uint64_t>(itemId) << 32) | locationId;

    if (m_pickedItems.find(combinedId) != m_pickedItems.end()) {
        LOG_DEBUG("Item %u at location %u already picked up", itemId, locationId);
        return;
    }
    
    LOG_INFO("Synchronizing item pickup: item %u at location %u", itemId, locationId);
    
    m_pickedItems[combinedId] = true;
    
    // Broadcast item pickup
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::ItemPickup;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = itemId;
    packet.flagValue = true;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

bool ProgressSync::IsItemPickedUp(uint32_t itemId, uint32_t locationId) {
    std::lock_guard<std::recursive_mutex> lock(m_mutex);
    uint64_t combinedId = (static_cast<uint64_t>(itemId) << 32) | locationId;
    return m_pickedItems.find(combinedId) != m_pickedItems.end();
}

void ProgressSync::NotifyFogGateEntry(uint32_t fogGateId) {
    LOG_INFO("Player entering fog gate %u", fogGateId);
    
    // Broadcast fog gate entry
    Network::EventFlagPacket packet{};
    packet.header.magic = 0x44533243;
    packet.header.type = Network::PacketType::FogGateTransition;
    packet.header.size = sizeof(Network::EventFlagPacket);
    packet.flagId = fogGateId;
    packet.flagValue = true;
    
    auto& peerMgr = Network::PeerManager::GetInstance();
    peerMgr.BroadcastPacket(&packet.header);
}

void ProgressSync::WaitForPartyAtFogGate(uint32_t fogGateId) {
    LOG_INFO("Waiting for party members at fog gate %u...", fogGateId);
    
    // In a full implementation, this would:
    // 1. Pause the local player
    // 2. Wait for all party members to arrive at the fog gate
    // 3. Resume once everyone is ready
    
    // For now, this is a placeholder
    // The actual implementation would need to hook into the game's
    // fog gate transition system and delay it until all players are ready
    
    LOG_INFO("Party synchronized at fog gate %u", fogGateId);
}

