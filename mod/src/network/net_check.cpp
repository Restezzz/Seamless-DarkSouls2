// The connection check behind "Check connection" in the menu.
//
// 17.09 a friend could not join and nothing on screen said why. His game logged
// in, and one to six minutes later the server gave up on him ("Attempted
// retransmission of packet max number of times, assuming connection has died").
// A ping from the host showed it: every size up to 1472 bytes reached him, 2000
// and 4000 never did -- nothing that travels as IP fragments survived that path
// -- and once everything was lost for twenty seconds. The host ran a TUN proxy
// (Happ) that part of Radmin's own traffic went through, and the friend's DNS
// answered with fake-IP addresses, the mark of another one.
//
// So the check finds out from inside the game what took a console and an hour:
//   * which adapter carries the traffic to the server and to the internet, and
//     which VPN or TUN adapters are up;
//   * whether DNS answers with fake-IP addresses (198.18.0.0/15);
//   * whether the server's login and auth ports answer, and whether the game
//     still hears from the server (session_hooks.cpp);
//   * in a lobby, datagrams of 64 to 8000 bytes both ways between the two
//     players over the mod's own channel -- the path the server's packets take
//     too when the host runs the server. Out: a probe of N bytes, answered small.
//     Back: a small ask, answered with N bytes. The sizes take turns round by
//     round, so an outage costs every size a little and a size limit costs one
//     size everything.
// The partner answers only with this build; an older one ignores the probes.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <WinSock2.h>
#include <WS2tcpip.h>
#include <Windows.h>
#include <iphlpapi.h>

#include "../../include/net_check.h"
#include "../../include/network.h"
#include "../../include/session.h"
#include "../../include/hooks.h"
#include "../../include/mod.h"
#include "../../include/ui_settings.h"
#include "../../include/utils.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "ws2_32.lib")

using namespace DS2Coop::Utils;
using DS2Coop::UI::Format;
using DS2Coop::UI::Tr;

namespace DS2Coop::Network {

namespace {

constexpr uint32_t  kMagic          = 0x44533243;   // 'DS2C', as peer_manager.cpp
constexpr std::array<uint32_t, 10> kSizes = { 64, 512, 1024, 1200, 1300, 1400, 1472, 2000, 4000, 8000 };
constexpr uint32_t  kRounds         = 8;
constexpr uint32_t  kLargestWhole   = 1472;         // the biggest UDP payload one 1500-byte packet carries
constexpr uint32_t  kMaxProbe       = 8000;         // the receive buffer takes 8192 (peer_manager.cpp)
constexpr ULONGLONG kLateMs         = 3000;         // answers still counted this long after the last probe
constexpr uint16_t  kAuthPort       = 50000;
constexpr long      kConnectTimeoutMs = 3000;
// What one partner's probes can make this side send. A whole check asks for 160
// answers adding up to some 170 KB over about eight seconds; twice that per ten
// seconds lets "Check again" run straight after, and caps anything else at a
// trickle.
constexpr ULONGLONG kBudgetWindowMs = 10000;
constexpr uint32_t  kBudgetReplies  = 400;
constexpr uint32_t  kBudgetBytes    = 512 * 1024;

struct SizeTally {
    std::array<bool, kRounds> Out{};   // an Ack came back for this round
    std::array<bool, kRounds> In{};    // a Back arrived for this round
    uint64_t RttSum = 0;
    uint32_t RttCount = 0;
    uint32_t OutCount() const { return static_cast<uint32_t>(std::count(Out.begin(), Out.end(), true)); }
    uint32_t InCount() const { return static_cast<uint32_t>(std::count(In.begin(), In.end(), true)); }
};

struct CheckState {
    std::mutex  Mutex;
    bool        Running = false;
    bool        Done = false;
    bool        LocalDone = false;
    bool        ProbeWanted = false;
    bool        ProbeDone = false;
    bool        ProbeCut = false;      // the lobby closed during the packet test
    bool        IsHost = false;
    HANDLE      Worker = nullptr;      // the local checks' thread, waited for on shutdown
    uint32_t    Run = 0;
    uint64_t    PeerId = 0;
    std::string PeerName;
    uint32_t    Step = 0;
    ULONGLONG   LastSendAt = 0;
    std::array<SizeTally, kSizes.size()> Tally{};
    std::vector<CheckLine>   Local;
    std::vector<CheckLine>   Lines;
    std::string              Report;
    std::vector<std::string> Tunnels;          // names of VPN/TUN adapters that are up
    bool        RouteBad = false;
    bool        PortsBad = false;
};

CheckState g_state;
std::atomic<bool> g_stopping{ false };

ULONGLONG Now() { return GetTickCount64(); }

// Network thread only.
struct ReplyBudget {
    uint64_t  PeerId = 0;
    ULONGLONG WindowStart = 0;
    uint32_t  Replies = 0;
    uint32_t  Bytes = 0;
};
std::array<ReplyBudget, 8> g_budgets{};

bool SpendBudget(uint64_t PeerId, uint32_t Bytes) {
    const ULONGLONG At = Now();
    ReplyBudget* Slot = nullptr;
    for (ReplyBudget& B : g_budgets) {
        if (B.WindowStart && B.PeerId == PeerId) {
            Slot = &B;
            break;
        }
    }
    if (!Slot) {   // a free slot, else the one with the oldest window
        Slot = &g_budgets[0];
        for (ReplyBudget& B : g_budgets) {
            if (B.WindowStart < Slot->WindowStart) Slot = &B;
        }
        *Slot = ReplyBudget{ PeerId, At, 0, 0 };
    }
    if (At - Slot->WindowStart >= kBudgetWindowMs) *Slot = ReplyBudget{ PeerId, At, 0, 0 };
    if (Slot->Replies + 1 > kBudgetReplies || Slot->Bytes + Bytes > kBudgetBytes) return false;
    ++Slot->Replies;
    Slot->Bytes += Bytes;
    return true;
}

int SizeIndex(uint32_t Bytes) {
    for (size_t I = 0; I < kSizes.size(); ++I) {
        if (kSizes[I] == Bytes) return static_cast<int>(I);
    }
    return -1;
}

// --- sending ---------------------------------------------------------------------

bool SendProbe(NetProbeKind Kind, uint32_t DatagramBytes, uint32_t AboutBytes, uint32_t Run, uint32_t Round,
               uint64_t SentAt, uint64_t PeerId) {
    thread_local std::vector<uint8_t> Buffer(kMaxProbe, 0);
    const uint32_t Size = std::clamp<uint32_t>(DatagramBytes, sizeof(NetProbePacket), kMaxProbe);
    for (uint32_t I = sizeof(NetProbePacket); I < Size; ++I) Buffer[I] = static_cast<uint8_t>(I * 31u);

    NetProbePacket P{};
    P.header.magic = kMagic;
    P.header.type = PacketType::NetProbe;
    P.header.size = Size;
    P.header.sequence = Round;
    P.header.timestamp = SentAt;
    P.kind = static_cast<uint8_t>(Kind);
    P.run = Run;
    P.round = Round;
    P.bytes = AboutBytes;
    P.sentAt = SentAt;
    std::memcpy(Buffer.data(), &P, sizeof(P));
    return PeerManager::GetInstance().SendPacket(reinterpret_cast<const PacketHeader*>(Buffer.data()), PeerId);
}

// --- local facts -------------------------------------------------------------------

enum class AdapterKind { Physical, LanVpn, Tunnel, Virtual };

struct Adapter {
    ULONG       Index = 0;
    std::string Name;
    std::string Description;
    ULONG       Mtu = 0;
    AdapterKind Kind = AdapterKind::Physical;
};

std::string Narrow(const wchar_t* Text) {
    if (!Text || !*Text) return {};
    const int Len = WideCharToMultiByte(CP_UTF8, 0, Text, -1, nullptr, 0, nullptr, nullptr);
    if (Len <= 1) return {};
    std::string Out(static_cast<size_t>(Len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, Text, -1, Out.data(), Len, nullptr, nullptr);
    return Out;
}

std::string Lower(std::string Text) {
    for (char& C : Text) C = static_cast<char>(std::tolower(static_cast<unsigned char>(C)));
    return Text;
}

// Radmin, Hamachi, ZeroTier and Tailscale put the players on one network: that
// is where the server should be reached. Anything else that tunnels -- a VPN, a
// proxy in TUN mode -- is what 17.09 was about.
AdapterKind Classify(const std::string& Name, const std::string& Description) {
    const std::string Text = Lower(Name + " " + Description);
    auto Has = [&](const char* Word) { return Text.find(Word) != std::string::npos; };
    if (Has("radmin") || Has("hamachi") || Has("zerotier") || Has("tailscale")) return AdapterKind::LanVpn;
    if (Has("vethernet") || Has("hyper-v") || Has("virtualbox") || Has("vmware") || Has("wsl")) {
        return AdapterKind::Virtual;
    }
    static const char* const kTunnels[] = {
        "wintun", "wireguard", "tap-windows", "tap-win32", "openvpn", "happ", "clash", "mihomo", "sing-box",
        "singbox", "v2ray", "xray", "hiddify", "nekoray", "nekobox", "outline", "amnezia", "warp", "cloudflare",
        "proton", "adguard", "tun",
    };
    for (const char* Word : kTunnels) {
        if (Has(Word)) return AdapterKind::Tunnel;
    }
    return AdapterKind::Physical;
}

std::vector<Adapter> ListAdapters() {
    const ULONG Flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG Size = 32 * 1024;
    std::vector<unsigned char> Buffer(Size);
    ULONG Rc = GetAdaptersAddresses(AF_INET, Flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()), &Size);
    if (Rc == ERROR_BUFFER_OVERFLOW) {
        Buffer.resize(Size);
        Rc = GetAdaptersAddresses(AF_INET, Flags, nullptr, reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()), &Size);
    }
    std::vector<Adapter> Out;
    if (Rc != NO_ERROR) return Out;
    for (auto* A = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(Buffer.data()); A; A = A->Next) {
        if (A->OperStatus != IfOperStatusUp || A->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
        bool HasIpv4 = false;
        for (auto* U = A->FirstUnicastAddress; U; U = U->Next) {
            if (U->Address.lpSockaddr && U->Address.lpSockaddr->sa_family == AF_INET) {
                HasIpv4 = true;
                break;
            }
        }
        if (!HasIpv4) continue;
        Adapter Info;
        Info.Index = A->IfIndex;
        Info.Name = Narrow(A->FriendlyName);
        Info.Description = Narrow(A->Description);
        Info.Mtu = A->Mtu;
        Info.Kind = Classify(Info.Name, Info.Description);
        Out.push_back(std::move(Info));
    }
    return Out;
}

const Adapter* RouteTo(const std::vector<Adapter>& Adapters, const in_addr& Target) {
    sockaddr_in Dst{};
    Dst.sin_family = AF_INET;
    Dst.sin_addr = Target;
    DWORD Index = 0;
    if (GetBestInterfaceEx(reinterpret_cast<sockaddr*>(&Dst), &Index) != NO_ERROR) return nullptr;
    for (const Adapter& A : Adapters) {
        if (A.Index == Index) return &A;
    }
    return nullptr;
}

bool ResolveIpv4(const std::string& Host, in_addr* Out) {
    if (inet_pton(AF_INET, Host.c_str(), Out) == 1) return true;
    addrinfo Hints{};
    Hints.ai_family = AF_INET;
    Hints.ai_socktype = SOCK_STREAM;
    addrinfo* Result = nullptr;
    if (getaddrinfo(Host.c_str(), nullptr, &Hints, &Result) != 0 || !Result) return false;
    *Out = reinterpret_cast<sockaddr_in*>(Result->ai_addr)->sin_addr;
    freeaddrinfo(Result);
    return true;
}

uint32_t HostOrder(const in_addr& A) { return ntohl(A.s_addr); }

bool IsFakeIp(const in_addr& A) { return (HostOrder(A) & 0xFFFE0000u) == 0xC6120000u; }   // 198.18.0.0/15

bool IsFakeIp(const std::string& Text) {
    in_addr A{};
    return !Text.empty() && inet_pton(AF_INET, Text.c_str(), &A) == 1 && IsFakeIp(A);
}

bool IsVpnLanAddress(const in_addr& A) {
    const uint32_t First = HostOrder(A) >> 24;
    return First == 26 || First == 25;   // Radmin VPN, Hamachi
}

bool IsLoopback(const in_addr& A) { return (HostOrder(A) >> 24) == 127; }

// Milliseconds a TCP connect took, -1 if the port did not answer in time.
// WSAConnect, not connect: the game's connect is hooked to redirect and log it
// (network_hooks.cpp), and this is not the game connecting.
int ConnectMs(const in_addr& Address, uint16_t Port) {
    SOCKET S = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (S == INVALID_SOCKET) return -1;
    u_long NonBlocking = 1;
    ioctlsocket(S, FIONBIO, &NonBlocking);
    sockaddr_in Dst{};
    Dst.sin_family = AF_INET;
    Dst.sin_port = htons(Port);
    Dst.sin_addr = Address;
    const ULONGLONG Start = Now();
    int Result = -1;
    if (WSAConnect(S, reinterpret_cast<sockaddr*>(&Dst), sizeof(Dst), nullptr, nullptr, nullptr, nullptr) == 0) {
        Result = static_cast<int>(Now() - Start);
    } else if (WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set Write;
        fd_set Error;
        FD_ZERO(&Write);
        FD_ZERO(&Error);
        FD_SET(S, &Write);
        FD_SET(S, &Error);
        timeval Timeout{ kConnectTimeoutMs / 1000, (kConnectTimeoutMs % 1000) * 1000 };
        if (select(0, nullptr, &Write, &Error, &Timeout) > 0 && FD_ISSET(S, &Write)) {
            int Err = 0;
            int Len = sizeof(Err);
            if (getsockopt(S, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&Err), &Len) == 0 && Err == 0) {
                Result = static_cast<int>(Now() - Start);
            }
        }
    }
    closesocket(S);
    return Result;
}

std::string PortAnswer(int Ms) {
    return Ms >= 0 ? Format(Tr("%d ms", "%d мс"), Ms) : std::string(Tr("no answer", "не отвечает"));
}

struct LocalInput {
    std::string ServerIp;
    uint16_t    LoginPort = 50031;
};

void FinishLocked();

void RunLocalChecks(const LocalInput& In) {
    WSADATA Wsa;
    const bool WsaUp = WSAStartup(MAKEWORD(2, 2), &Wsa) == 0;

    std::vector<CheckLine>   Lines;
    std::vector<std::string> Tunnels;
    bool RouteBad = false;
    bool PortsBad = false;
    const std::vector<Adapter> Adapters = ListAdapters();

    // VPN and proxy adapters.
    {
        std::string LanVpn, Tunnel;
        for (const Adapter& A : Adapters) {
            if (A.Kind == AdapterKind::LanVpn) LanVpn += (LanVpn.empty() ? "" : ", ") + A.Name;
            if (A.Kind == AdapterKind::Tunnel) {
                Tunnel += (Tunnel.empty() ? "" : ", ") + A.Name + " (" + A.Description + ")";
                Tunnels.push_back(A.Name);
            }
        }
        if (!Tunnel.empty()) {
            Lines.push_back({ CheckLevel::Warn, Tr("VPN or proxy adapters are up", "Работают VPN или прокси-адаптеры"),
                              Tunnel + Tr(". A VPN or a proxy in TUN mode can carry Radmin's own traffic and lose large packets.",
                                          ". VPN или прокси в режиме TUN может пропускать через себя сам туннель Radmin и терять крупные пакеты.") });
        } else {
            Lines.push_back({ CheckLevel::Ok, Tr("No VPN or proxy adapters", "VPN и прокси-адаптеров нет"),
                              LanVpn.empty() ? std::string() : std::string(Tr("Players' network: ", "Сеть игроков: ")) + LanVpn + "." });
        }
    }

    // Route to the server.
    in_addr Server{};
    const bool HaveServer = !In.ServerIp.empty() && ResolveIpv4(In.ServerIp, &Server);
    const std::string RouteTitle = Format(Tr("Route to the server %s", "Маршрут к серверу %s"), In.ServerIp.c_str());
    if (!HaveServer) {
        Lines.push_back({ CheckLevel::Fail, RouteTitle, Tr("server_ip in ds2_seamless_coop.ini is not an address.",
                                                           "server_ip в ds2_seamless_coop.ini — не адрес.") });
        RouteBad = true;
    } else if (IsLoopback(Server)) {
        Lines.push_back({ CheckLevel::Ok, RouteTitle, Tr("this computer.", "этот компьютер.") });
    } else if (const Adapter* Via = RouteTo(Adapters, Server)) {
        const std::string Through = Format(Tr("through %s (MTU %lu).", "через %s (MTU %lu)."), Via->Name.c_str(), Via->Mtu);
        if (Via->Kind == AdapterKind::LanVpn) {
            Lines.push_back({ CheckLevel::Ok, RouteTitle, Through });
        } else if (IsVpnLanAddress(Server)) {
            Lines.push_back({ CheckLevel::Fail, RouteTitle,
                              Through + Tr(" Not through Radmin VPN: is Radmin connected to the host's network? Or a VPN or proxy is taking this traffic.",
                                           " Не через Radmin VPN: подключён ли Radmin к сети хоста? Или трафик забирает VPN или прокси.") });
            RouteBad = true;
        } else {
            Lines.push_back({ CheckLevel::Info, RouteTitle, Through });
        }
    } else {
        Lines.push_back({ CheckLevel::Fail, RouteTitle, Tr("no route: this computer is not on the server's network.",
                                                           "маршрута нет: компьютер не в сети сервера.") });
        RouteBad = true;
    }

    // Route to the internet: a TUN proxy there may carry the VPN's own tunnel.
    {
        in_addr Internet{};
        inet_pton(AF_INET, "1.1.1.1", &Internet);
        if (const Adapter* Via = RouteTo(Adapters, Internet)) {
            if (Via->Kind == AdapterKind::Tunnel) {
                Lines.push_back({ CheckLevel::Warn, Tr("Internet traffic", "Интернет-трафик"),
                                  Format(Tr("goes through %s. Radmin VPN's own tunnel may go through it too: put RvControlSvc.exe and DarkSoulsII.exe into its exceptions (direct).",
                                            "идёт через %s. Сам туннель Radmin VPN тоже может идти через него: добавь RvControlSvc.exe и DarkSoulsII.exe в исключения (напрямую, direct)."),
                                         Via->Name.c_str()) });
            } else {
                Lines.push_back({ CheckLevel::Ok, Tr("Internet traffic", "Интернет-трафик"),
                                  Format(Tr("goes through %s (MTU %lu).", "идёт через %s (MTU %lu)."), Via->Name.c_str(), Via->Mtu) });
            }
        }
    }

    // DNS with fake-IP answers: a proxy in TUN mode. (The slow steps from here
    // on are skipped when the mod is shutting down.)
    if (!g_stopping.load()) {
        in_addr Probe{};
        const bool Resolved = ResolveIpv4("store.steampowered.com", &Probe);
        const bool Fake = (Resolved && IsFakeIp(Probe)) || IsFakeIp(Hooks::WinsockHooks::GetLastLoginTarget());
        if (Fake) {
            Lines.push_back({ CheckLevel::Warn, Tr("DNS", "DNS"),
                              Tr("answers with 198.18.x.x addresses: a proxy with fake-IP DNS is on (Clash, sing-box, Happ, Hiddify and the like). Radmin VPN and the game belong in its exceptions.",
                                 "отдаёт адреса 198.18.x.x: включён прокси с fake-IP (Clash, sing-box, Happ, Hiddify и т. п.). Radmin VPN и игру стоит добавить в его исключения.") });
        } else if (Resolved) {
            Lines.push_back({ CheckLevel::Ok, Tr("DNS", "DNS"), Tr("ordinary answers.", "обычные ответы.") });
        } else {
            Lines.push_back({ CheckLevel::Info, Tr("DNS", "DNS"), Tr("did not answer: no internet?", "не ответил: нет интернета?") });
        }
    }

    // The server's login and auth ports.
    if (HaveServer && !g_stopping.load()) {
        const int Login = ConnectMs(Server, In.LoginPort);
        const int Auth = ConnectMs(Server, kAuthPort);
        const std::string Detail = Format(Tr("login %u: %s, auth %u: %s.", "вход %u: %s, авторизация %u: %s."),
                                          static_cast<unsigned>(In.LoginPort), PortAnswer(Login).c_str(),
                                          static_cast<unsigned>(kAuthPort), PortAnswer(Auth).c_str());
        if (Login >= 0 && Auth >= 0) {
            Lines.push_back({ CheckLevel::Ok, Tr("Server ports", "Порты сервера"), Detail });
        } else {
            Lines.push_back({ CheckLevel::Fail, Tr("Server ports", "Порты сервера"),
                              Detail + Tr(" The server is off, the ini has another address, or Radmin is not connected.",
                                          " Сервер выключен, в ini другой адрес или Radmin не подключён.") });
            PortsBad = true;
        }
    }

    // Does the game still hear from the server?
    {
        const ULONGLONG Last = Hooks::GetLastServerMessageTime();
        if (Hooks::IsServerLineStalled()) {
            Lines.push_back({ CheckLevel::Fail, Tr("Game server", "Игровой сервер"),
                              Tr("the game gets no answers from the server: restart the game. If it happens again, the path loses packets (see below).",
                                 "игра не получает ответов сервера: перезапусти игру. Если повторится — путь теряет пакеты (см. ниже).") });
        } else if (Last) {
            Lines.push_back({ CheckLevel::Ok, Tr("Game server", "Игровой сервер"),
                              Format(Tr("last message %llu s ago.", "последнее сообщение %llu с назад."),
                                     static_cast<unsigned long long>((Now() - Last) / 1000)) });
        } else {
            Lines.push_back({ CheckLevel::Info, Tr("Game server", "Игровой сервер"),
                              Tr("the game has not talked to the server in this run yet.", "игра в этом запуске ещё не общалась с сервером.") });
        }
    }

    if (WsaUp) WSACleanup();

    std::lock_guard<std::mutex> Lock(g_state.Mutex);
    g_state.Local = std::move(Lines);
    g_state.Tunnels = std::move(Tunnels);
    g_state.RouteBad = RouteBad;
    g_state.PortsBad = PortsBad;
    g_state.LocalDone = true;
    if (!g_state.ProbeWanted || g_state.ProbeDone) FinishLocked();
}

DWORD WINAPI LocalChecksThread(LPVOID Param) {
    const std::unique_ptr<LocalInput> In(static_cast<LocalInput*>(Param));
    RunLocalChecks(*In);
    return 0;
}

// --- the verdict ---------------------------------------------------------------------

enum class PathResult { Clean, FragmentsLost, Lossy, NoAnswer };

struct DirectionSummary {
    PathResult  Result = PathResult::NoAnswer;
    uint32_t    LossPercent = 100;
    std::string Sizes;   // "64: 8/8 · 512: 8/8 · …"
};

DirectionSummary Summarize(bool Outbound) {
    DirectionSummary S;
    uint32_t SmallGot = 0, SmallSent = 0, LargeGot = 0, LargeSent = 0;
    for (size_t I = 0; I < kSizes.size(); ++I) {
        const uint32_t Got = Outbound ? g_state.Tally[I].OutCount() : g_state.Tally[I].InCount();
        if (!S.Sizes.empty()) S.Sizes += " \xC2\xB7 ";
        S.Sizes += Format("%u: %u/%u", kSizes[I], Got, kRounds);
        if (kSizes[I] <= kLargestWhole) {
            SmallGot += Got;
            SmallSent += kRounds;
        } else {
            LargeGot += Got;
            LargeSent += kRounds;
        }
    }
    const uint32_t Got = SmallGot + LargeGot, Sent = SmallSent + LargeSent;
    S.LossPercent = Sent ? 100u - (100u * Got) / Sent : 100u;
    if (Got == 0) {
        S.Result = PathResult::NoAnswer;
    } else if (SmallGot * 4 >= SmallSent * 3 && LargeGot * 4 <= LargeSent) {
        S.Result = PathResult::FragmentsLost;
    } else if (Got * 10 >= Sent * 9) {
        S.Result = PathResult::Clean;
    } else {
        S.Result = PathResult::Lossy;
    }
    return S;
}

CheckLine DirectionLine(const DirectionSummary& S, const std::string& Title) {
    CheckLine Line{ CheckLevel::Ok, Title, S.Sizes };
    switch (S.Result) {
    case PathResult::Clean:
        Line.level = CheckLevel::Ok;
        break;
    case PathResult::FragmentsLost:
        Line.level = CheckLevel::Fail;
        Line.detail += Tr(". Nothing over 1472 bytes gets through.", ". Больше 1472 байт не проходит ничего.");
        break;
    case PathResult::Lossy:
        Line.level = CheckLevel::Warn;
        Line.detail += Format(Tr(". %u%% lost.", ". Потеряно %u%%."), S.LossPercent);
        break;
    case PathResult::NoAnswer:
        Line.level = CheckLevel::Warn;
        Line.detail += Tr(". No answer at all.", ". Ответов нет совсем.");
        break;
    }
    return Line;
}

const char* LevelTag(CheckLevel Level) {
    switch (Level) {
    case CheckLevel::Ok:   return Tr("[OK]", "[ОК]");
    case CheckLevel::Warn: return Tr("[WARN]", "[ВНИМАНИЕ]");
    case CheckLevel::Fail: return Tr("[FAIL]", "[ОШИБКА]");
    default:               return Tr("[INFO]", "[ИНФО]");
    }
}

void FinishLocked() {
    std::vector<CheckLine> Lines = g_state.Local;
    bool Fragments = false, NoAnswer = false, OneWay = false, Lossy = false;
    uint32_t WorstLoss = 0;
    std::string DeadWay;
    const bool Probed = g_state.ProbeWanted && !g_state.ProbeCut;

    if (g_state.ProbeCut) {
        Lines.push_back({ CheckLevel::Warn, Tr("Packets between players", "Пакеты между игроками"),
                          Tr("the test was cut short: the lobby closed before it finished.",
                             "проверка прервана: лобби закрылось раньше, чем она закончилась.") });
    } else if (Probed) {
        const DirectionSummary Out = Summarize(true);
        const DirectionSummary In = Summarize(false);
        Lines.push_back(DirectionLine(Out, Format(Tr("Packets from me to %s", "Пакеты от меня к %s"), g_state.PeerName.c_str())));
        Lines.push_back(DirectionLine(In, Format(Tr("Packets from %s to me", "Пакеты от %s ко мне"), g_state.PeerName.c_str())));
        const SizeTally& Small = g_state.Tally[0];
        if (Small.RttCount) {
            Lines.push_back({ CheckLevel::Info, Tr("Delay", "Задержка"),
                              Format(Tr("about %llu ms there and back.", "около %llu мс туда и обратно."),
                                     static_cast<unsigned long long>(Small.RttSum / Small.RttCount)) });
        }
        Fragments = Out.Result == PathResult::FragmentsLost || In.Result == PathResult::FragmentsLost;
        NoAnswer = Out.Result == PathResult::NoAnswer && In.Result == PathResult::NoAnswer;
        OneWay = !NoAnswer && (Out.Result == PathResult::NoAnswer || In.Result == PathResult::NoAnswer);
        if (OneWay) {
            DeadWay = Out.Result == PathResult::NoAnswer
                          ? Format(Tr("from me to %s", "от меня к %s"), g_state.PeerName.c_str())
                          : Format(Tr("from %s to me", "от %s ко мне"), g_state.PeerName.c_str());
        }
        Lossy = Out.Result == PathResult::Lossy || In.Result == PathResult::Lossy;
        WorstLoss = std::max(Out.LossPercent, In.LossPercent);
    } else {
        Lines.push_back({ CheckLevel::Info, Tr("Packets between players", "Пакеты между игроками"),
                          Tr("not tested: run the check in a lobby with the partner.",
                             "не проверялись: запусти проверку в лобби с напарником.") });
    }

    std::string TunnelList;
    for (const std::string& Name : g_state.Tunnels) TunnelList += (TunnelList.empty() ? "" : ", ") + Name;

    CheckLine Verdict{ CheckLevel::Ok, Tr("Verdict", "Вывод"), "" };
    if (g_state.RouteBad) {
        Verdict.level = CheckLevel::Fail;
        Verdict.detail = Tr("the traffic to the server does not go through the players' network. Fix that first.",
                            "трафик к серверу идёт не через сеть игроков. Сначала исправить это.");
    } else if (g_state.PortsBad) {
        Verdict.level = CheckLevel::Fail;
        Verdict.detail = Tr("the server does not answer on its ports: is it running, is the ini address right, is Radmin connected?",
                            "сервер не отвечает на своих портах: запущен ли он, верный ли адрес в ini, подключён ли Radmin?");
    } else if (Fragments) {
        Verdict.level = CheckLevel::Fail;
        Verdict.detail = std::string(Tr("datagrams over 1472 bytes do not get through between you. The game server's large packets die the same way, and after a few failed retries it drops the player. The usual cause is a VPN or proxy in TUN mode on the path",
                                        "пакеты больше 1472 байт между вами не проходят. Крупные пакеты игрового сервера гибнут так же, и после нескольких неудачных повторов сервер отключает игрока. Обычная причина — VPN или прокси в режиме TUN на пути"))
                       + (TunnelList.empty() ? std::string(Tr(" (on this or the partner's computer)", " (на этом компьютере или у напарника)"))
                                             : " (" + TunnelList + ")")
                       + Tr(": put Radmin VPN (RvControlSvc.exe) and DarkSoulsII.exe into its exceptions on both computers, or turn it off while playing.",
                            ": добавь Radmin VPN (RvControlSvc.exe) и DarkSoulsII.exe в его исключения на обоих компьютерах или выключай его на время игры.");
    } else if (OneWay) {
        Verdict.level = CheckLevel::Fail;
        Verdict.detail = Format(Tr("nothing gets through %s, while the other way works: a firewall, antivirus or VPN on one of the computers blocks that direction.",
                                   "%s не доходит ничего, хотя в обратную сторону всё работает: это направление режет фаервол, антивирус или VPN на одном из компьютеров."),
                                DeadWay.c_str());
    } else if (NoAnswer) {
        Verdict.level = CheckLevel::Warn;
        Verdict.detail = Tr("the partner did not answer the packet test: an older mod version there, or no connection at all.",
                            "напарник не ответил на проверку пакетов: у него старая версия мода или связи нет совсем.");
    } else if (Lossy) {
        Verdict.level = CheckLevel::Warn;
        Verdict.detail = Format(Tr("the line loses packets of every size (up to %u%%): an unstable connection, or Radmin going through a relay.",
                                   "линия теряет пакеты любого размера (до %u%%): нестабильная связь или Radmin через ретранслятор."),
                                WorstLoss);
    } else if (g_state.ProbeCut) {
        Verdict.level = CheckLevel::Warn;
        Verdict.detail = Tr("the packet test did not finish because the lobby closed. Run the check again in the lobby.",
                            "проверка пакетов не закончилась: лобби закрылось. Запусти проверку ещё раз в лобби.");
    } else if (!TunnelList.empty()) {
        Verdict.level = CheckLevel::Warn;
        Verdict.detail = Tr("nothing was lost during the check, but a VPN or proxy adapter is up. If the connection still drops, put Radmin VPN and the game into its exceptions.",
                            "за проверку ничего не потерялось, но работает VPN или прокси-адаптер. Если связь всё равно рвётся — добавь Radmin VPN и игру в его исключения.");
    } else if (Probed) {
        Verdict.detail = Tr("packets of every size get through both ways. If the connection still drops, run the check on the partner's computer.",
                            "пакеты всех размеров проходят в обе стороны. Если связь всё равно рвётся — запусти проверку на компьютере напарника.");
    } else {
        Verdict.level = CheckLevel::Info;
        Verdict.detail = Tr("nothing wrong on this computer. Run the check in a lobby to test the packets between you.",
                            "на этом компьютере ничего плохого не видно. Запусти проверку в лобби, чтобы проверить пакеты между вами.");
    }
    Lines.push_back(Verdict);

    SYSTEMTIME Time;
    GetLocalTime(&Time);
    std::string Report = Format(Tr("Seamless Co-op v%s: connection check, %04u-%02u-%02u %02u:%02u\n",
                                   "Seamless Co-op v%s: проверка связи, %04u-%02u-%02u %02u:%02u\n"),
                                MOD_VERSION, Time.wYear, Time.wMonth, Time.wDay, Time.wHour, Time.wMinute);
    if (g_state.ProbeWanted) {   // who the packets were tested with, cut short or not
        Report += Format(Tr("Role: %s, partner: %s\n", "Роль: %s, напарник: %s\n"),
                         g_state.IsHost ? Tr("host", "хост") : Tr("guest", "гость"), g_state.PeerName.c_str());
    }
    for (const CheckLine& Line : Lines) {
        Report += std::string(LevelTag(Line.level)) + " " + Line.title;
        if (!Line.detail.empty()) Report += ": " + Line.detail;
        Report += "\n";
        LOG_INFO("[CHECK] %s %s: %s", LevelTag(Line.level), Line.title.c_str(), Line.detail.c_str());
    }

    g_state.Lines = std::move(Lines);
    g_state.Report = std::move(Report);
    g_state.Done = true;
    g_state.Running = false;
}

} // namespace

void StartNetCheck() {
    auto& Lobby = Session::SessionManager::GetInstance();
    const auto Players = Lobby.GetPlayers();
    const uint64_t LocalId = PeerManager::GetInstance().GetLocalPlayerId();
    const bool Ready = Lobby.IsActive() && (Lobby.IsHost() || PeerManager::GetInstance().IsHandshakeConfirmed());
    const auto& Config = SeamlessCoopMod::GetInstance().GetConfig();
    LocalInput In{ Config.server_ip, Config.server_port };

    if (g_stopping.load()) return;
    std::lock_guard<std::mutex> Lock(g_state.Mutex);
    {
        if (g_state.Running) return;
        if (g_state.Worker) {   // the last check's thread: long done, since the check finished
            WaitForSingleObject(g_state.Worker, 1000);
            CloseHandle(g_state.Worker);
            g_state.Worker = nullptr;
        }
        g_state.Done = false;
        g_state.LocalDone = false;
        g_state.ProbeDone = false;
        g_state.ProbeCut = false;
        g_state.Step = 0;
        g_state.LastSendAt = 0;
        g_state.Tally = {};
        g_state.Local.clear();
        g_state.Lines.clear();
        g_state.Report.clear();
        g_state.Tunnels.clear();
        g_state.RouteBad = false;
        g_state.PortsBad = false;
        g_state.PeerId = 0;
        g_state.PeerName.clear();
        for (const auto& Player : Players) {
            if (Player.playerId == 0 || Player.playerId == LocalId) continue;
            g_state.PeerId = Player.playerId;
            g_state.PeerName = Player.playerName;
            break;
        }
        g_state.IsHost = Lobby.IsHost();
        g_state.ProbeWanted = Ready && g_state.PeerId != 0;
        g_state.Run = static_cast<uint32_t>(Now()) | 1u;
        LOG_INFO("[CHECK] connection check started%s%s", g_state.ProbeWanted ? " with " : " (no partner: this computer only)",
                 g_state.ProbeWanted ? g_state.PeerName.c_str() : "");
    }
    // Under the lock, so a thread that finishes at once still finds Running set.
    auto* Input = new LocalInput(std::move(In));
    g_state.Worker = CreateThread(nullptr, 0, LocalChecksThread, Input, 0, nullptr);
    if (!g_state.Worker) {
        delete Input;
        LOG_WARNING("[CHECK] could not start the check's thread (%lu)", GetLastError());
        return;
    }
    g_state.Running = true;
}

void ShutdownNetCheck() {
    g_stopping.store(true);
    HANDLE Worker = nullptr;
    {
        std::lock_guard<std::mutex> Lock(g_state.Mutex);
        Worker = g_state.Worker;
        g_state.Worker = nullptr;
    }
    if (!Worker) return;
    // The slow steps notice the flag; a connect already waiting takes at most 3 s.
    if (WaitForSingleObject(Worker, 7000) != WAIT_OBJECT_0) {
        LOG_WARNING("[CHECK] the check's thread did not finish in time");
    }
    CloseHandle(Worker);
}

NetCheckView GetNetCheckView() {
    std::lock_guard<std::mutex> Lock(g_state.Mutex);
    NetCheckView View;
    View.running = g_state.Running;
    View.done = g_state.Done;
    View.lines = g_state.Done ? g_state.Lines : g_state.Local;
    View.report = g_state.Report;
    if (g_state.Done) {
        View.progress = 1.0f;
    } else if (g_state.ProbeWanted) {
        const float Total = static_cast<float>(kRounds * kSizes.size());
        const float Sent = std::min(1.0f, static_cast<float>(g_state.Step) / Total);
        const float Late = g_state.Step >= kRounds * kSizes.size()
                               ? std::min(1.0f, static_cast<float>(Now() - g_state.LastSendAt) / static_cast<float>(kLateMs))
                               : 0.0f;
        View.progress = 0.1f * (g_state.LocalDone ? 1.0f : 0.0f) + 0.7f * Sent + 0.2f * Late;
    } else {
        View.progress = g_state.LocalDone ? 1.0f : 0.4f;
    }
    return View;
}

void NetCheckTick() {
    uint32_t Bytes = 0, Run = 0, Round = 0;
    uint64_t Peer = 0;
    const ULONGLONG At = Now();
    const bool LobbyUp = Session::SessionManager::GetInstance().IsActive();
    {
        std::lock_guard<std::mutex> Lock(g_state.Mutex);
        if (!g_state.Running || !g_state.ProbeWanted || g_state.ProbeDone) return;
        if (!LobbyUp) {
            // The lobby closed mid-test: nothing more can arrive, and the check
            // must not wait for ticks that stopped with it.
            g_state.ProbeDone = true;
            g_state.ProbeCut = true;
            if (g_state.LocalDone) FinishLocked();
            return;
        }
        const uint32_t Total = kRounds * static_cast<uint32_t>(kSizes.size());
        if (g_state.Step >= Total) {
            if (At - g_state.LastSendAt >= kLateMs) {
                g_state.ProbeDone = true;
                if (g_state.LocalDone) FinishLocked();
            }
            return;
        }
        Bytes = kSizes[g_state.Step % kSizes.size()];
        Round = g_state.Step / static_cast<uint32_t>(kSizes.size());
        Run = g_state.Run;
        Peer = g_state.PeerId;
        ++g_state.Step;
        g_state.LastSendAt = At;
    }
    // Outside the lock: sending takes the peer list's lock.
    SendProbe(NetProbeKind::Out, Bytes, Bytes, Run, Round, At, Peer);
    SendProbe(NetProbeKind::Ask, sizeof(NetProbePacket), Bytes, Run, Round, At, Peer);
}

void NetCheckOnProbe(const PacketHeader* Packet, const PeerInfo& Sender) {
    if (!Packet || Packet->size < sizeof(NetProbePacket)) return;
    const auto* P = reinterpret_cast<const NetProbePacket*>(Packet);
    // The partner answers probes whether or not it runs a check of its own -- the
    // asker is the one checking -- so what the answers may cost is bounded per
    // sender instead (SpendBudget), and only the check's own sizes are served.
    switch (static_cast<NetProbeKind>(P->kind)) {
    case NetProbeKind::Out:
        if (!SpendBudget(Sender.playerId, sizeof(NetProbePacket))) break;
        SendProbe(NetProbeKind::Ack, sizeof(NetProbePacket), Packet->size, P->run, P->round, P->sentAt, Sender.playerId);
        break;
    case NetProbeKind::Ask:
        if (SizeIndex(P->bytes) < 0 || !SpendBudget(Sender.playerId, P->bytes)) break;
        SendProbe(NetProbeKind::Back, P->bytes, P->bytes, P->run, P->round, P->sentAt, Sender.playerId);
        break;
    case NetProbeKind::Ack:
    case NetProbeKind::Back: {
        const bool IsAck = static_cast<NetProbeKind>(P->kind) == NetProbeKind::Ack;
        if (!IsAck && P->bytes != Packet->size) break;
        const int Index = SizeIndex(P->bytes);
        if (Index < 0 || P->round >= kRounds) break;
        std::lock_guard<std::mutex> Lock(g_state.Mutex);
        if (!g_state.Running || P->run != g_state.Run) break;
        SizeTally& Tally = g_state.Tally[static_cast<size_t>(Index)];
        if (IsAck) {
            if (!Tally.Out[P->round]) {
                Tally.Out[P->round] = true;
                const ULONGLONG At = Now();
                if (At >= P->sentAt && At - P->sentAt < 10000) {
                    Tally.RttSum += At - P->sentAt;
                    ++Tally.RttCount;
                }
            }
        } else {
            Tally.In[P->round] = true;
        }
        break;
    }
    default:
        break;
    }
}

} // namespace DS2Coop::Network
