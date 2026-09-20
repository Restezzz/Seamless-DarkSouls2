// Peer-to-peer networking for seamless co-op
//
// UDP-based P2P with handshake, heartbeat, and timeout.
// Host creates a session with a password.
// Client sends handshake with password -> host validates -> sends response.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#pragma comment(lib, "ws2_32.lib")

#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/sync.h"
#include "../../include/hooks.h"
#include "../../include/ui.h"
#include "../../include/utils.h"
#include "../../include/ui_settings.h"
#include <atomic>
#include <chrono>
#include <algorithm>

using namespace DS2Coop::Network;
using namespace DS2Coop::Utils;

constexpr uint32_t PACKET_MAGIC = 0x44533243; // 'DS2C'
constexpr uint64_t HEARTBEAT_INTERVAL_MS = 5000;
// Twenty seconds: four missed heartbeats in a row. It was a full minute, and on
// 16.09 a guest whose game had crashed went on standing in the host's lobby list
// until it came back under a new id. Radmin VPN between two machines does not go
// silent for four heartbeats running without the link really being gone.
constexpr uint64_t TIMEOUT_DURATION_MS = 20000;

static uint64_t NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()
    ).count();
}

PeerManager& PeerManager::GetInstance() {
    static PeerManager instance;
    return instance;
}

bool PeerManager::Initialize(uint16_t port) {
    if (m_initialized) return true;

    LOG_INFO("Initializing peer manager on port %u...", port);

    WSADATA wsaData;
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0) {
        LOG_ERROR("Failed to initialize Winsock");
        return false;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        LOG_ERROR("Failed to create socket: %d", WSAGetLastError());
        WSACleanup();
        return false;
    }

    // Non-blocking mode
    u_long mode = 1;
    if (ioctlsocket(sock, FIONBIO, &mode) != 0) {
        LOG_ERROR("Failed to set non-blocking mode");
        closesocket(sock);
        WSACleanup();
        return false;
    }

    // Bind
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == SOCKET_ERROR) {
        LOG_ERROR("Failed to bind socket on port %u: %d", port, WSAGetLastError());
        closesocket(sock);
        WSACleanup();
        return false;
    }

    m_socket = reinterpret_cast<void*>(sock);
    m_port = port;
    m_localPlayerId = static_cast<uint64_t>(GetCurrentProcessId()) ^ NowMs();
    m_initialized = true;

    LOG_INFO("Peer manager initialized, local ID: %llu, port: %u", m_localPlayerId, port);
    return true;
}

void PeerManager::Shutdown() {
    if (!m_initialized) return;

    LOG_INFO("Shutting down peer manager...");

    LeaveSession();

    if (m_socket) {
        closesocket(reinterpret_cast<SOCKET>(m_socket));
        m_socket = nullptr;
    }

    WSACleanup();
    m_initialized = false;
}

// Whatever reached the port while no session was up belongs to an old one: the
// socket is only read inside a session, so it waits there. The Disconnect a
// host sent when it closed its previous lobby turned the next join down as
// "wrong password" (12.09 03:01:06). It is thrown away before a session starts.
// Every read of the socket takes this: the network thread drains it in
// HandleIncomingPackets, and a lobby created or joined from the menu (the
// render thread) throws old packets away here.
static std::mutex g_socketReadMutex;

static void DiscardPendingPackets(SOCKET sock) {
    std::lock_guard<std::mutex> lock(g_socketReadMutex);
    constexpr int kMaxDrain = 4096;   // a flood cannot keep us here
    char buffer[2048];
    int dropped = 0;
    for (int i = 0; i < kMaxDrain; ++i) {
        sockaddr_in from{};
        int fromLen = sizeof(from);
        const int got = recvfrom(sock, buffer, sizeof(buffer), 0, reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got != SOCKET_ERROR) {
            ++dropped;
            continue;
        }
        const int error = WSAGetLastError();
        if (error != WSAEMSGSIZE && error != WSAECONNRESET) break;   // WSAEWOULDBLOCK: nothing left
    }
    if (dropped) LOG_INFO("Dropped %d packet(s) left over from before this session", dropped);
}

bool PeerManager::CreateSession(const std::string& password) {
    if (!m_initialized) return false;

    LOG_INFO("Creating session with password...");
    DiscardPendingPackets(reinterpret_cast<SOCKET>(m_socket));

    m_isHost = true;
    m_connected = true;
    m_sessionPassword = password;

    LOG_INFO("Session created, waiting for peers on port %u...", m_port);
    return true;
}

bool PeerManager::JoinSession(const std::string& address, uint16_t port, const std::string& password) {
    if (!m_initialized) return false;

    LOG_INFO("Joining session at %s:%u...", address.c_str(), port);
    DiscardPendingPackets(reinterpret_cast<SOCKET>(m_socket));

    m_sessionPassword = password;

    // Build handshake packet
    HandshakePacket handshake{};
    handshake.header.magic = PACKET_MAGIC;
    handshake.header.type = PacketType::Handshake;
    handshake.header.size = sizeof(HandshakePacket);
    handshake.header.sequence = 0;
    handshake.header.timestamp = NowMs();
    handshake.version = 1;
    handshake.playerId = m_localPlayerId;
    std::string charName = DS2Coop::Sync::PlayerSync::GetInstance().GetLocalCharacterName();
    strncpy_s(handshake.playerName, charName.empty() ? "Player" : charName.c_str(), sizeof(handshake.playerName));
    // Send password + Steam ID so host can whitelist our signs
    std::string steamId = DS2Coop::Hooks::ProtobufHooks::GetLocalSteamId();
    std::string pwWithSteam = password + "|" + steamId;
    strncpy_s(handshake.password, pwWithSteam.c_str(), sizeof(handshake.password));

    // Send to host
    sockaddr_in hostAddr{};
    hostAddr.sin_family = AF_INET;
    hostAddr.sin_port = htons(port);
    inet_pton(AF_INET, address.c_str(), &hostAddr.sin_addr);

    SOCKET sock = reinterpret_cast<SOCKET>(m_socket);
    if (sendto(sock, reinterpret_cast<const char*>(&handshake), sizeof(handshake), 0,
               reinterpret_cast<sockaddr*>(&hostAddr), sizeof(hostAddr)) == SOCKET_ERROR) {
        LOG_ERROR("Failed to send handshake: %d", WSAGetLastError());
        return false;
    }

    // Register host as a peer
    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        PeerInfo hostPeer{};
        hostPeer.playerId = 0; // Will be set when host responds
        hostPeer.playerName = "Host";
        hostPeer.address = hostAddr.sin_addr.s_addr;
        hostPeer.port = port;
        hostPeer.lastHeartbeat = NowMs();
        hostPeer.connected = true;
        m_peers.push_back(hostPeer);
    }

    m_isHost = false;
    m_connected = true;
    m_handshakeConfirmed = false;
    m_connectingTimestampMs = NowMs();

    LOG_INFO("Handshake sent, waiting for response...");
    return true;
}

void PeerManager::LeaveSession() {
    if (!m_connected) return;

    LOG_INFO("Leaving session...");

    // Send disconnect to all peers
    PacketHeader disconnectPacket{};
    disconnectPacket.magic = PACKET_MAGIC;
    disconnectPacket.type = PacketType::Disconnect;
    disconnectPacket.size = sizeof(PacketHeader);
    disconnectPacket.sequence = 0;
    disconnectPacket.timestamp = NowMs();

    BroadcastPacket(&disconnectPacket);

    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        m_peers.clear();
    }
    m_connected = false;
    m_isHost = false;
    m_sessionPassword.clear();

    LOG_INFO("Session left");
}

void PeerManager::Update() {
    if (!m_initialized || !m_connected) return;

    // Don't hold the lock across the entire update — each sub-function
    // acquires it as needed. Holding it here caused deadlocks because
    // HandleIncomingPackets → HandleHandshakePacket → PacketHandler →
    // SessionManager::AddPlayer takes m_playersMutex, while
    // SessionManager::LeaveSession → PeerManager::LeaveSession takes
    // m_peersMutex in the opposite order.
    HandleIncomingPackets();

    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        SendHeartbeats();
    }
    // Takes the lock itself and lets go of it before calling into SessionManager.
    CheckTimeouts();

    // Client-side: check for handshake timeout (10 seconds)
    if (!m_isHost && !m_handshakeConfirmed && m_connectingTimestampMs > 0) {
        uint64_t elapsed = NowMs() - m_connectingTimestampMs;
        if (elapsed > 10000) {
            LOG_WARNING("Handshake timeout — host did not respond in 10s");
            DS2Coop::UI::Overlay::GetInstance().ShowNotification(
                DS2Coop::UI::Tr("The host did not answer. Check the address and that the lobby is open.",
                                "Хост не ответил. Проверьте адрес и что лобби создано."),
                6.0f, DS2Coop::UI::NotifyKind::Error);
            LeaveSession();
        }
    }
}

bool PeerManager::SendPacket(const PacketHeader* packet, uint64_t targetPlayerId) {
    if (!m_initialized || !m_connected || !packet) return false;

    std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
    SOCKET sock = reinterpret_cast<SOCKET>(m_socket);

    for (const auto& peer : m_peers) {
        if ((targetPlayerId == 0 || peer.playerId == targetPlayerId) && peer.connected) {
            sockaddr_in peerAddr{};
            peerAddr.sin_family = AF_INET;
            peerAddr.sin_addr.s_addr = peer.address;
            peerAddr.sin_port = htons(peer.port);

            sendto(sock, reinterpret_cast<const char*>(packet), packet->size, 0,
                   reinterpret_cast<sockaddr*>(&peerAddr), sizeof(peerAddr));

            if (targetPlayerId != 0) return true;
        }
    }

    return true;
}

// The name a peer goes by in notices. The handshake carries it once, and a player who joined before
// its character existed handed over "Player"; PlayerName packets put the real one in later.
void PeerManager::SetPeerName(uint64_t playerId, const std::string& name) {
    std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
    for (auto& peer : m_peers) {
        if (peer.playerId == playerId) peer.playerName = name;
    }
}

void PeerManager::BroadcastPacket(const PacketHeader* packet) {
    if (!m_initialized || !m_connected || !packet) return;

    std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
    SOCKET sock = reinterpret_cast<SOCKET>(m_socket);

    for (const auto& peer : m_peers) {
        if (peer.connected) {
            sockaddr_in peerAddr{};
            peerAddr.sin_family = AF_INET;
            peerAddr.sin_addr.s_addr = peer.address;
            peerAddr.sin_port = htons(peer.port);

            sendto(sock, reinterpret_cast<const char*>(packet), packet->size, 0,
                   reinterpret_cast<sockaddr*>(&peerAddr), sizeof(peerAddr));
        }
    }
}

void PeerManager::HandleIncomingPackets() {
    SOCKET sock = reinterpret_cast<SOCKET>(m_socket);

    // Receive all pending packets into a local buffer first,
    // then process them. This avoids holding m_peersMutex while
    // calling into SessionManager (which takes m_playersMutex),
    // preventing cross-mutex deadlocks.
    struct ReceivedPacket {
        char data[8192];
        int size;
        sockaddr_in sender;
    };
    std::vector<ReceivedPacket> packets;

    // Drain the socket under the read lock: a lobby created or joined from the
    // menu drains it too (DiscardPendingPackets). The lock is let go before the
    // packets are handled.
    std::unique_lock<std::mutex> readLock(g_socketReadMutex);
    while (true) {
        ReceivedPacket pkt{};
        int senderLen = sizeof(pkt.sender);
        pkt.size = recvfrom(sock, pkt.data, sizeof(pkt.data), 0,
                           reinterpret_cast<sockaddr*>(&pkt.sender), &senderLen);

        if (pkt.size == SOCKET_ERROR) {
            int error = WSAGetLastError();
            // A port that answered "unreachable" to one of our sends (the other
            // game closed) or an oversized datagram: that one is used up, the
            // rest are still there. These made 13,000 "Socket error: 10054"
            // lines in one log and held the other packets back until the next
            // update.
            if (error == WSAECONNRESET || error == WSAEMSGSIZE) continue;
            if (error != WSAEWOULDBLOCK) {
                LOG_ERROR("Socket error: %d", error);
            }
            break;
        }

        if (pkt.size >= static_cast<int>(sizeof(PacketHeader))) {
            PacketHeader* hdr = reinterpret_cast<PacketHeader*>(pkt.data);
            if (hdr->magic == PACKET_MAGIC) {
                packets.push_back(pkt);
            }
        }
    }
    readLock.unlock();

    // Process each packet
    for (auto& pkt : packets) {
        PacketHeader* header = reinterpret_cast<PacketHeader*>(pkt.data);

        if (header->type == PacketType::Handshake && pkt.size >= static_cast<int>(sizeof(HandshakePacket))) {
            HandshakePacket* hs = reinterpret_cast<HandshakePacket*>(pkt.data);
            HandleHandshakePacket(hs, pkt.sender);
            continue;
        }

        if (header->type == PacketType::Disconnect && !m_isHost && m_handshakeConfirmed) {
            // The host closed the session. We are standing in its world, so go
            // home the way a dropped session does rather than just going quiet.
            // Before the handshake the same packet means "rejected", below.
            LOG_INFO("[SEAMLESS] The host closed the session — returning to our own world");
            DS2Coop::UI::Overlay::GetInstance().ShowNotification(
                DS2Coop::UI::Tr("The host closed the lobby. Returning to your world.",
                                "Хост закрыл лобби. Возвращаемся в свой мир."),
                5.0f, DS2Coop::UI::NotifyKind::Warning);
            DS2Coop::Sync::RequestLeaveWorld();
            DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(false);
            DS2Coop::Session::SessionManager::GetInstance().LeaveSession();
            return;
        }

        if (header->type == PacketType::Disconnect && !m_isHost) {
            LOG_WARNING("Received disconnect from host — wrong password or rejected");
            DS2Coop::UI::Overlay::GetInstance().ShowNotification(
                DS2Coop::UI::Tr("The host refused the connection. Wrong password?",
                                "Хост отклонил подключение. Неверный пароль?"),
                5.0f, DS2Coop::UI::NotifyKind::Error);
            LeaveSession();
            return;
        }

        // Update peer heartbeat under lock
        PeerInfo senderInfo{};
        bool knownSender = false;
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            for (auto& peer : m_peers) {
                if (peer.address == pkt.sender.sin_addr.s_addr) {
                    peer.lastHeartbeat = NowMs();
                    senderInfo.playerId = peer.playerId;
                    senderInfo.playerName = peer.playerName;
                    knownSender = true;
                    break;
                }
            }
        }
        // Everything past the handshake comes from a player the handshake let in.
        // A datagram with the right magic from anyone else -- no password, no
        // lobby -- is not handed to the game-facing handlers at all.
        if (!knownSender) {
            static std::atomic<uint32_t> s_strangers{ 0 };
            const uint32_t Count = s_strangers.fetch_add(1) + 1;
            if (Count <= 3 || Count % 500 == 0) {
                LOG_WARNING("[NET] ignored a packet of type 0x%02X from an address not in the lobby (%u so far)",
                            static_cast<unsigned>(header->type), Count);
            }
            continue;
        }
        senderInfo.address = pkt.sender.sin_addr.s_addr;
        senderInfo.port = ntohs(pkt.sender.sin_port);
        senderInfo.lastHeartbeat = NowMs();
        senderInfo.connected = true;

        // Process outside lock — PacketHandler may call into SessionManager
        PacketHandler::GetInstance().HandlePacket(header, senderInfo);
    }
}

void PeerManager::HandleHandshakePacket(const HandshakePacket* hs, const sockaddr_in& senderAddr) {
    LOG_INFO("Received handshake from %s (ID: %llu)", hs->playerName, hs->playerId);

    // Version check
    if (hs->version != 1) {
        LOG_ERROR("Version mismatch from peer: expected 1, got %u", hs->version);
        return;
    }

    if (m_isHost) {
        // Host: validate password and accept/reject
        // Password field is "password|steamid" — extract just the password part
        std::string receivedPw(hs->password);
        size_t pipePos = receivedPw.find('|');
        std::string justPassword = (pipePos != std::string::npos) ? receivedPw.substr(0, pipePos) : receivedPw;

        if (m_sessionPassword != justPassword) {
            LOG_WARNING("Peer rejected: wrong password");
            // Send rejection (a disconnect packet)
            PacketHeader reject{};
            reject.magic = PACKET_MAGIC;
            reject.type = PacketType::Disconnect;
            reject.size = sizeof(PacketHeader);
            reject.timestamp = NowMs();

            SOCKET sock = reinterpret_cast<SOCKET>(m_socket);
            sendto(sock, reinterpret_cast<const char*>(&reject), sizeof(reject), 0,
                   reinterpret_cast<const sockaddr*>(&senderAddr), sizeof(senderAddr));
            return;
        }

        // The same machine handshaking again under a new id is the same player
        // back after a crash or a restart. Before the seamless timeout existed
        // the old entry stayed in the lobby for good -- two "ee" on 12.09 -- and
        // even now it would linger for minutes. It makes way for the new one.
        //
        // Registration is under the peer lock as well: this runs on the network
        // thread while the game thread sends through the same list, and a
        // push_back that moves the vector under a reader is a use-after-free.
        std::vector<uint64_t> stale;
        bool isNewPeer = false;
        unsigned newPort = 0;
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            for (auto it = m_peers.begin(); it != m_peers.end();) {
                if (it->address == senderAddr.sin_addr.s_addr && it->playerId != hs->playerId) {
                    LOG_INFO("Peer %s (ID: %llu) is back as ID %llu -- dropping the old entry",
                             it->playerName.c_str(), it->playerId, hs->playerId);
                    stale.push_back(it->playerId);
                    it = m_peers.erase(it);
                } else {
                    ++it;
                }
            }

            // Password matches - register peer
            bool alreadyKnown = false;
            for (auto& peer : m_peers) {
                if (peer.playerId == hs->playerId) {
                    peer.lastHeartbeat = NowMs();
                    peer.connected = true;
                    alreadyKnown = true;
                    break;
                }
            }
            if (!alreadyKnown) {
                PeerInfo newPeer{};
                newPeer.playerId = hs->playerId;
                newPeer.playerName = hs->playerName;
                newPeer.address = senderAddr.sin_addr.s_addr;
                newPeer.port = ntohs(senderAddr.sin_port);
                newPeer.lastHeartbeat = NowMs();
                newPeer.connected = true;
                m_peers.push_back(newPeer);
                isNewPeer = true;
                newPort = newPeer.port;
            }
        }
        // Outside the peer lock: SessionManager takes its own, in the other order.
        // Quietly -- the same player joins again right after, it is no departure.
        for (uint64_t id : stale) DS2Coop::Session::SessionManager::GetInstance().RemovePlayer(id, true);

        if (isNewPeer) {
            LOG_INFO("Peer accepted: %s (ID: %llu) from port %u",
                     hs->playerName, hs->playerId, newPort);

            // Extract peer's Steam ID from password field ("password|steamid")
            std::string pw(hs->password);
            size_t pipe = pw.find('|');
            if (pipe != std::string::npos && pipe + 1 < pw.size()) {
                DS2Coop::Hooks::ProtobufHooks::AddSessionSteamId(pw.substr(pipe + 1));
            }

            // First real peer connected — activate seamless disconnect blocking now
            DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(true);
            LOG_INFO("[SEAMLESS] First peer connected — seamless mode ON");
        }

        // Send handshake response back so the joiner knows we accepted
        HandshakePacket response{};
        response.header.magic = PACKET_MAGIC;
        response.header.type = PacketType::Handshake;
        response.header.size = sizeof(HandshakePacket);
        response.header.sequence = 1; // Sequence 1 = response
        response.header.timestamp = NowMs();
        response.version = 1;
        response.playerId = m_localPlayerId;
        std::string hostName = DS2Coop::Sync::PlayerSync::GetInstance().GetLocalCharacterName();
        strncpy_s(response.playerName, hostName.empty() ? "Host" : hostName.c_str(), sizeof(response.playerName));
        response.password[0] = '\0'; // never echo the password back

        SOCKET sock = reinterpret_cast<SOCKET>(m_socket);
        sendto(sock, reinterpret_cast<const char*>(&response), sizeof(response), 0,
               reinterpret_cast<const sockaddr*>(&senderAddr), sizeof(senderAddr));

        LOG_INFO("Sent handshake response to %s", hs->playerName);
    } else {
        // Client: this is the host's response to our handshake
        LOG_INFO("Received handshake response from host (ID: %llu)", hs->playerId);

        // Update the host peer entry with the real player ID -- under the peer
        // lock, the game thread reads these fields while it sends.
        {
            std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
            for (auto& peer : m_peers) {
                if (peer.address == senderAddr.sin_addr.s_addr) {
                    peer.playerId = hs->playerId;
                    peer.playerName = hs->playerName;
                    peer.lastHeartbeat = NowMs();
                    peer.connected = true;
                    break;
                }
            }
        }

        m_handshakeConfirmed = true;
        LOG_INFO("Connected to host: %s", hs->playerName);

        // Handshake confirmed — activate seamless disconnect blocking now
        DS2Coop::Hooks::ProtobufHooks::SetSeamlessActive(true);
        LOG_INFO("[SEAMLESS] Handshake confirmed — seamless mode ON");
        // The host accepted us: start the automatic summon from our side.
        const bool SignQueued = DS2Coop::Sync::RequestAutoSignPlacement();

        // Show connection notification -- the summon promise only with a sign on its way.
        DS2Coop::UI::Overlay::GetInstance().ShowNotification(
            SignQueued ? DS2Coop::UI::Tr("Connected. The host will summon you in a moment.",
                                         "Подключено. Хост сейчас вас призовёт.")
                       : DS2Coop::UI::Tr("Connected.", "Подключено."),
            5.0f, DS2Coop::UI::NotifyKind::Success);
    }

    // Forward to packet handler for session manager integration
    PeerInfo senderInfo{};
    senderInfo.playerId = hs->playerId;
    senderInfo.playerName = hs->playerName;
    senderInfo.address = senderAddr.sin_addr.s_addr;
    senderInfo.port = ntohs(senderAddr.sin_port);
    senderInfo.lastHeartbeat = NowMs();
    senderInfo.connected = true;

    PacketHandler::GetInstance().HandlePacket(&hs->header, senderInfo);
}

void PeerManager::SendHeartbeats() {
    uint64_t now = NowMs();

    if (now - m_lastHeartbeatMs < HEARTBEAT_INTERVAL_MS) return;

    PacketHeader heartbeat{};
    heartbeat.magic = PACKET_MAGIC;
    heartbeat.type = PacketType::Heartbeat;
    heartbeat.size = sizeof(PacketHeader);
    heartbeat.sequence = 0;
    heartbeat.timestamp = now;

    BroadcastPacket(&heartbeat);
    m_lastHeartbeatMs = now;
}

void PeerManager::CheckTimeouts() {
    // In seamless mode the game session runs through ds3os and P2P UDP
    // heartbeats over a VPN are bursty, so the ordinary limit is too quick --
    // it made the HUD flicker "Session empty" while everyone was playing. But
    // never timing out left a crashed player in the lobby for good (12.09).
    // This much silence means the other game is gone.
    constexpr uint64_t kSeamlessTimeoutMs = 180000;
    const uint64_t limit = DS2Coop::Hooks::ProtobufHooks::IsSeamlessActive() ? kSeamlessTimeoutMs
                                                                              : TIMEOUT_DURATION_MS;
    std::vector<uint64_t> timedOut;
    {
        std::lock_guard<std::recursive_mutex> lock(m_peersMutex);
        const uint64_t now = NowMs();
        for (auto it = m_peers.begin(); it != m_peers.end();) {
            if (it->connected && now > it->lastHeartbeat && now - it->lastHeartbeat > limit) {
                LOG_WARNING("Peer %s (ID: %llu) timed out", it->playerName.c_str(), it->playerId);
                timedOut.push_back(it->playerId);
                it = m_peers.erase(it);
            } else {
                ++it;
            }
        }
    }
    // Outside the peer lock, like every other call into SessionManager here.
    for (uint64_t id : timedOut) DS2Coop::Session::SessionManager::GetInstance().RemovePlayer(id);
}
