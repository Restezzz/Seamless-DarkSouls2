#pragma once

#include <string>
#include <vector>

// The connection check behind "Check connection" in the menu (net_check.cpp).
//
// Local part, any time: which adapter carries the traffic to the server and to
// the internet, which VPN and TUN adapters are up, fake-IP DNS, the server's
// login and auth ports, and when the game last heard from the server. In a
// lobby: datagrams of 64 to 8000 bytes both ways between this player and the
// partner, over the mod's own channel.
namespace DS2Coop::Network {

struct PacketHeader;
struct PeerInfo;

enum class CheckLevel { Ok, Warn, Fail, Info };

struct CheckLine {
    CheckLevel  level = CheckLevel::Info;
    std::string title;
    std::string detail;
};

struct NetCheckView {
    bool                   running = false;
    bool                   done = false;
    float                  progress = 0.0f;   // 0..1 while running
    std::vector<CheckLine> lines;             // what is known so far
    std::string            report;            // plain text for the clipboard, once done
};

void         StartNetCheck();      // from the menu; ignored while a check runs
NetCheckView GetNetCheckView();    // any thread
void         ShutdownNetCheck();   // mod shutdown: waits (bounded) for the check's own thread

// The mod's update thread: the loop in SeamlessCoopMod, which ticks with or
// without a lobby, and the packet handler it drives.
void NetCheckTick();
void NetCheckOnProbe(const PacketHeader* packet, const PeerInfo& sender);

} // namespace DS2Coop::Network
